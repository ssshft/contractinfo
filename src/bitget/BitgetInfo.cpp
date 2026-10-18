#include "bitget/BitgetInfo.h"
#include "base/InfoHelper.h"

#include <simdjson.h>

BitgetInfo::BitgetInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void BitgetInfo::syncExchangeInfo() {
    getMixInfo("usdt-futures", USDT_SWAP);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    getMixInfo("usdc-futures", USDC_SWAP);
    std::this_thread::sleep_for(std::chrono::seconds(1));
}


// ============================================================================
// GET api.bitget.com/api/v2/mix/market/contracts?productType=...
//
// response: {"code":"00000","msg":"success","requestTime":...,"data":[{...}]}
//   失败时 code != "00000" 且 data 为空数组 —— 所以**不能只看 http 200**。
//
// data[i] 字段顺序 (2026-10 实测):
//   symbol, baseCoin, quoteCoin, buyLimitPriceRatio, sellLimitPriceRatio,
//   feeRateUpRatio, makerFeeRate, takerFeeRate, openCostUpRatio, supportMarginCoins,
//   minTradeNum, priceEndStep, volumePlace, pricePlace, sizeMultiplier, symbolType,
//   minTradeUSDT, maxSymbolOrderNum, maxProductOrderNum, maxPositionNum, symbolStatus,
//   offTime, limitOpenTime, deliveryTime, deliveryStartTime, deliveryPeriod, launchTime,
//   fundInterval, minLever, maxLever, posLimit, maintainTime, openTime,
//   maxMarketOrderQty, maxOrderQty, isRwa
//
// 语义 (这几条是最容易写错的):
//   1. 精度是 "整数步长 + 小数位数": tickSize = priceEndStep / 10^pricePlace。
//      BTCUSDT 的 priceEndStep="1", pricePlace=1 -> 0.1, 不是 1.0。
//   2. 下单量是**币本位数量**(多少个 base 币), 不是张数 -> value = 1。
//      证据: BTCUSDT 的 minTradeNum="0.0001" (0.0001 BTC), 若按张数应是整数 1。
//      sizeMultiplier 才是数量步长 (BTCUSDT 0.0001, SHIBUSDT 10000)。
//   3. volumePlace 只是数量小数位数, 跟步长不是一回事, 不要拿它算 lotSize。
// ============================================================================
void BitgetInfo::getMixInfo(const std::string& productType, InstType instTypeEnum) {
    try {
        std::string path = "/api/v2/mix/market/contracts?productType=" + productType;
        int status = 0;
        std::string body;
        if (!syncGet("api.bitget.com", path, body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("BitgetInfo::getMixInfo error! status: {} productType: {}", status, productType);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("BitgetInfo::getMixInfo parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        // 顶层顺序: code -> msg -> requestTime -> data
        std::string_view code;
        doc["code"].get(code);
        if (!code.empty() && code != "00000") {
            std::string_view msg;
            doc["msg"].get(msg);
            LOG_ERROR("BitgetInfo::getMixInfo api code={} msg={} productType={}", code, msg, productType);
            return;
        }

        auto data = doc["data"].get_array();
        if (data.error() != simdjson::SUCCESS) {
            LOG_ERROR("BitgetInfo::getMixInfo: 'data' missing or not array, productType: {}", productType);
            return;
        }

        int kept = 0;
        int skippedType = 0;
        int skippedStatus = 0;

        for (auto row : data) {
            auto sym = row.get_object();
            if (sym.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // ---- 严格按 JSON 顺序访问 ----
            std::string_view symbol, baseCoin, quoteCoin;
            std::string_view minTradeNum, priceEndStep, sizeMultiplier, symbolType;
            std::string_view minTradeUSDT, symbolStatus, maxMarketOrderQty, maxOrderQty;
            int64_t pricePlace = 0;

            sym["symbol"].get(symbol);
            sym["baseCoin"].get(baseCoin);
            sym["quoteCoin"].get(quoteCoin);
            sym["minTradeNum"].get(minTradeNum);       // 10
            sym["priceEndStep"].get(priceEndStep);     // 11
            // 12 volumePlace 不读: 它只是数量小数位数, 真正的步长在 sizeMultiplier, 读错会差 10^n 倍
            sym["pricePlace"].get(pricePlace);         // 13
            sym["sizeMultiplier"].get(sizeMultiplier); // 14
            sym["symbolType"].get(symbolType);         // 15
            sym["minTradeUSDT"].get(minTradeUSDT);     // 16
            sym["symbolStatus"].get(symbolStatus);     // 20
            sym["maxMarketOrderQty"].get(maxMarketOrderQty);  // 33
            sym["maxOrderQty"].get(maxOrderQty);              // 34

            // 只要永续; 交割合约的 instId 需要加到期后缀, 本工程不做
            if (symbolType != "perpetual") {
                ++skippedType;
                continue;
            }

            if (symbolStatus != "normal") {
                ++skippedStatus;
                continue;
            }

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            std::string baseStr(baseCoin);
            getBaseMagnifyNum(baseStr, base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = BITGET;
            info.instTypeEnum = instTypeEnum;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteCoin);
            crypto::copy_sv_to_char_array(info.margin, quoteCoin);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.value = 1;                 // 下单量单位 = base 币
            info.calcType = 0;              // usdt/usdc 本位都是线性合约
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            infohelp::priceFromStep(info, crypto::fast_atod(priceEndStep), static_cast<int>(pricePlace));
            infohelp::sizeFromDecimal(info, sizeMultiplier);

            if (!minTradeNum.empty()) {
                info.minSize = crypto::fast_atod(minTradeNum);
            }
            if (!maxOrderQty.empty()) {
                info.maxSize = crypto::fast_atod(maxOrderQty);
            }
            else if (!maxMarketOrderQty.empty()) {
                info.maxSize = crypto::fast_atod(maxMarketOrderQty);
            }
            if (!minTradeUSDT.empty()) {
                info.minAmount = crypto::fast_atod(minTradeUSDT);
            }

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("BitgetInfo {} -> kept={} skip(not_perpetual)={} skip(status)={}",
                 productType, kept, skippedType, skippedStatus);

        if (kept == 0) {
            LOG_ERROR("BitgetInfo {} returned 0 instruments. Response schema may have changed.", productType);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("BitgetInfo::getMixInfo exception: {}", e.what());
    }
}
