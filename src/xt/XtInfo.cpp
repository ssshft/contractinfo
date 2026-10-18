#include "xt/XtInfo.h"
#include "base/InfoHelper.h"

#include <simdjson.h>

XtInfo::XtInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void XtInfo::syncExchangeInfo() {
    getSymbolList();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}


// ============================================================================
// GET fapi.xt.com/future/market/v1/public/symbol/list
//
// response: {"returnCode":0,"msgInfo":"success","error":null,"result":[{...}]}
//   ⚠️ result 直接就是数组, 不是 {"list":[...]}。写成 doc["result"]["list"] 会拿到空。
//
// result[i] 字段顺序 (2026-10 实测, 共 91 个字段):
//   id, symbol, symbolId, symbolGroupId, pair, contractType, productType,
//   predictEventType, predictEventParam, predictEventSort, underlyingType,
//   contractSize, thanosSwitch, tradeSwitch, openSwitch, isDisplay, isOpenApi,
//   state, initLeverage, initPositionType, baseCoin, spotCoin, quoteCoin,
//   baseCoinPrecision, baseCoinDisplayPrecision, quoteCoinPrecision,
//   quoteCoinDisplayPrecision, quantityPrecision, pricePrecision, ...,
//   minQty, minNotional, maxNotional, maxMarketOrderQty, maxLimitOrderQty, ...,
//   minStepPrice, ...
//
// 语义:
//   contractSize    = 1 张合约的面值 (DOGE 是 10 个 DOGE), 字符串
//   quantityPrecision = 数量小数位, XT 的数量单位是"张", 步长 = 10^-qtyPrec (实测全是 0, 即 1 张)
//   minStepPrice    = 价格步长(字符串), 比 pricePrecision 更直接, 优先用它
//   minQty          = 最小下单张数, maxLimitOrderQty = 限价单最大张数
//   minNotional     = 最小名义价值
//   过滤: contractType=="PERPETUAL" 且 tradeSwitch && openSwitch && isOpenApi && state==0
// ============================================================================
void XtInfo::getSymbolList() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("fapi.xt.com", "/future/market/v1/public/symbol/list", body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("XtInfo::getSymbolList error! status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("XtInfo::getSymbolList parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        // 顶层顺序: returnCode -> msgInfo -> error -> result
        int64_t returnCode = 0;
        doc["returnCode"].get(returnCode);
        if (returnCode != 0) {
            std::string_view msg;
            doc["msgInfo"].get(msg);
            LOG_ERROR("XtInfo::getSymbolList api returnCode={} msg={}", returnCode, msg);
            return;
        }

        auto data = doc["result"].get_array();
        if (data.error() != simdjson::SUCCESS) {
            LOG_ERROR("XtInfo::getSymbolList: 'result' missing or not array");
            return;
        }

        int kept = 0;
        int skippedDated = 0;
        int skippedSwitch = 0;
        int skippedState = 0;

        for (auto row : data) {
            auto sym = row.get_object();
            if (sym.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // ---- 严格按 JSON 顺序访问 ----
            std::string_view symbol, contractType, underlyingType, contractSize;
            std::string_view baseCoin, quoteCoin, minQty, minNotional, maxLimitOrderQty, minStepPrice;
            int64_t quantityPrecision = 0, pricePrecision = 0, state = 0;
            bool tradeSwitch = false, openSwitch = false, isOpenApi = false;

            sym["symbol"].get(symbol);                    // 1
            sym["contractType"].get(contractType);        // 5
            sym["underlyingType"].get(underlyingType);    // 10
            sym["contractSize"].get(contractSize);        // 11
            sym["tradeSwitch"].get(tradeSwitch);          // 13
            sym["openSwitch"].get(openSwitch);            // 14
            sym["isOpenApi"].get(isOpenApi);              // 16
            sym["state"].get(state);                      // 17
            sym["baseCoin"].get(baseCoin);                // 20
            sym["quoteCoin"].get(quoteCoin);              // 22
            sym["quantityPrecision"].get(quantityPrecision); // 27
            sym["pricePrecision"].get(pricePrecision);    // 28
            sym["minQty"].get(minQty);                    // 33
            sym["minNotional"].get(minNotional);          // 34
            sym["maxLimitOrderQty"].get(maxLimitOrderQty);// 37
            sym["minStepPrice"].get(minStepPrice);        // 56

            if (contractType != "PERPETUAL") {
                ++skippedDated;
                continue;
            }
            if (!tradeSwitch || !openSwitch || !isOpenApi) {
                ++skippedSwitch;
                continue;
            }
            if (state != 0) {
                ++skippedState;
                continue;
            }

            std::string baseRaw = crypto::to_upper(std::string(baseCoin));
            std::string quoteUpper = crypto::to_upper(std::string(quoteCoin));

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseRaw, base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = XT;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteUpper);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.value = crypto::fast_atod(contractSize);   // 下单量单位是"张"
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (underlyingType == "COIN_BASED") {
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
                crypto::copy_sv_to_char_array(info.margin, info.base);
            }
            else {
                info.calcType = 0;
                info.instTypeEnum = crypto::str_cmp(info.quote, "USDC") ? USDC_SWAP : USDT_SWAP;
                crypto::copy_sv_to_char_array(info.margin, info.quote);
            }

            if (!minStepPrice.empty() && crypto::fast_atod(minStepPrice) > 0.0) {
                infohelp::priceFromDecimal(info, minStepPrice);
            }
            else {
                infohelp::priceFromStep(info, 1.0, static_cast<int>(pricePrecision));
            }

            infohelp::sizeFromStep(info, 1.0, static_cast<int>(quantityPrecision));

            if (!minQty.empty()) {
                info.minSize = crypto::fast_atod(minQty);
            }
            if (!maxLimitOrderQty.empty()) {
                info.maxSize = crypto::fast_atod(maxLimitOrderQty);
            }
            if (!minNotional.empty()) {
                info.minAmount = crypto::fast_atod(minNotional);
            }

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("XtInfo -> kept={} skip(not_perpetual)={} skip(switch_off)={} skip(state)={}",
                 kept, skippedDated, skippedSwitch, skippedState);

        if (kept == 0) {
            LOG_ERROR("XtInfo returned 0 instruments. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("XtInfo::getSymbolList exception: {}", e.what());
    }
}
