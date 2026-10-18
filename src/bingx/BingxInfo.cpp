#include "bingx/BingxInfo.h"
#include "base/InfoHelper.h"

#include <simdjson.h>

BingxInfo::BingxInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void BingxInfo::syncExchangeInfo() {
    getSwapContracts();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}


// ============================================================================
// GET open-api.bingx.com/openApi/swap/v2/quote/contracts
//
// response: {"code":0,"msg":"","data":[{...}]}   code != 0 即失败
//
// data[i] 字段顺序 (2026-10 实测, 共 23 个字段):
//   contractId, symbol, size, quantityPrecision, pricePrecision, feeRate,
//   makerFeeRate, takerFeeRate, tradeMinLimit, tradeMinQuantity, tradeMinUSDT,
//   currency, asset, status, apiStateOpen, apiStateClose, ensureTrigger,
//   triggerFeeRate, brokerState, launchTime, maintainTime, offTime, displayName
//
// 语义:
//   size             = 数量步长 (10^-quantityPrecision), 不是合约面值 -> value 取 1
//   tradeMinQuantity = 最小下单量(币), 可能是 6.4 这种非 10 的幂 (XTZ-USDT)
//   tradeMinUSDT     = 最小名义价值
//   pricePrecision   = 价格小数位, tickSize = 10^-pricePrecision
//   asset/currency   = base / quote (symbol "BTC-USDT" 也能切, 但字段更权威)
//   status           = 1 加密永续; 25 外汇/合成品
//   本接口**不给 maxOrderQty**
// ============================================================================
void BingxInfo::getSwapContracts() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("open-api.bingx.com", "/openApi/swap/v2/quote/contracts", body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("BingxInfo::getSwapContracts error! status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("BingxInfo::getSwapContracts parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        // 顶层顺序: code -> msg -> data
        int64_t code = 0;
        doc["code"].get(code);
        if (code != 0) {
            std::string_view msg;
            doc["msg"].get(msg);
            LOG_ERROR("BingxInfo::getSwapContracts api code={} msg={}", code, msg);
            return;
        }

        auto data = doc["data"].get_array();
        if (data.error() != simdjson::SUCCESS) {
            LOG_ERROR("BingxInfo::getSwapContracts: 'data' missing or not array");
            return;
        }

        int kept = 0;
        int skippedStatus = 0;
        int skippedCurrency = 0;

        for (auto row : data) {
            auto sym = row.get_object();
            if (sym.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // ---- 严格按 JSON 顺序访问 ----
            std::string_view symbol, size, currency, asset;
            int64_t quantityPrecision = 0, pricePrecision = 0, statusInt = 0;
            double tradeMinQuantity = 0.0, tradeMinUSDT = 0.0;

            sym["symbol"].get(symbol);                       // 1
            sym["size"].get(size);                           // 2
            sym["quantityPrecision"].get(quantityPrecision); // 3
            sym["pricePrecision"].get(pricePrecision);       // 4
            sym["tradeMinQuantity"].get(tradeMinQuantity);   // 9
            sym["tradeMinUSDT"].get(tradeMinUSDT);           // 10
            sym["currency"].get(currency);                   // 11
            sym["asset"].get(asset);                         // 12
            sym["status"].get(statusInt);                    // 13

            if (statusInt != 1) {
                ++skippedStatus;
                continue;
            }

            // 只接 USDT / USDC 本位; 币本位在另一套接口上
            if (currency != "USDT" && currency != "USDC") {
                ++skippedCurrency;
                continue;
            }

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            std::string assetStr(asset);
            getBaseMagnifyNum(assetStr, base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = BINGX;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, currency);
            crypto::copy_sv_to_char_array(info.margin, currency);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.instTypeEnum = (currency == "USDC") ? USDC_SWAP : USDT_SWAP;
            info.value = 1;      // 下单量单位 = base 币
            info.calcType = 0;
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            infohelp::priceFromStep(info, 1.0, static_cast<int>(pricePrecision));

            // size 正常就是 10^-quantityPrecision; 万一为空或 0, 用精度兜底
            if (!size.empty() && crypto::fast_atod(size) > 0.0) {
                infohelp::sizeFromDecimal(info, size);
            }
            else {
                infohelp::sizeFromStep(info, 1.0, static_cast<int>(quantityPrecision));
            }

            info.minSize = tradeMinQuantity;
            info.minAmount = tradeMinUSDT;
            // BingX 不给最大下单量, 留 0

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("BingxInfo -> kept={} skip(status!=1)={} skip(currency)={}",
                 kept, skippedStatus, skippedCurrency);

        if (kept == 0) {
            LOG_ERROR("BingxInfo returned 0 instruments. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("BingxInfo::getSwapContracts exception: {}", e.what());
    }
}
