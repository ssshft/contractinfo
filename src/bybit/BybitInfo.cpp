#include "bybit/BybitInfo.h"
#include <simdjson.h>


BybitInfo::BybitInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void BybitInfo::syncExchangeInfo() {
    std::vector<std::string> vInstType = {"spot", "linear", "inverse"};
    for (size_t i = 0; i < vInstType.size(); ++i) {
        getInfo(vInstType[i]);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}


// ============================================================================
// Bybit v5 GET /v5/market/instruments-info?category=...&limit=1000
//   response: {"retCode":0,"retMsg":"OK","result":{"category":"...","list":[{...}], "nextPageCursor":"..."},"retExtInfo":{},"time":...}
//
// 字段顺序 (list[i]):
//   Spot:    symbol, baseCoin, quoteCoin, innovation, status, marginTrading,
//            lotSizeFilter{basePrecision, quotePrecision, minOrderQty, maxOrderQty, minOrderAmt, maxOrderAmt},
//            priceFilter{tickSize}, riskParameters
//   Linear/Inverse:
//            symbol, contractType, status, baseCoin, quoteCoin, launchTime, deliveryTime,
//            deliveryFeeRate, priceScale,
//            leverageFilter{...}, priceFilter{tickSize},
//            lotSizeFilter{maxOrderQty, minOrderQty, qtyStep, ..., minNotionalValue, ...},
//            unifiedMarginTrade, fundingInterval, settleCoin, ...
//
// ⚠️ Linear/Inverse 里 contractType 在 baseCoin **前面**, 访问顺序必须对齐。
// ============================================================================
void BybitInfo::getInfo(const std::string& instType) {
    try {
        std::string path = "/v5/market/instruments-info?category=" + instType + "&limit=1000";
        int status = 0;
        std::string body;
        if (!syncGet("api.bybit.com", path, body, status)) return;
        if (status != 200) {
            LOG_ERROR("BybitInfo::getInfo error! status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("BybitInfo::getInfo parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        // response 顶层字段顺序: retCode, retMsg, result{...}, retExtInfo, time
        // 直接进入 result.list
        auto result = doc["result"].get_object();
        if (result.error() != simdjson::SUCCESS) {
            LOG_ERROR("BybitInfo::getInfo: 'result' missing or not object");
            return;
        }

        // result 里字段顺序: category, list, nextPageCursor
        auto list = result["list"].get_array();
        if (list.error() != simdjson::SUCCESS) {
            LOG_ERROR("BybitInfo::getInfo: 'result.list' missing or not array");
            return;
        }

        const bool isSpot    = crypto::str_cmp(instType.c_str(), "spot");
        const bool isLinear  = crypto::str_cmp(instType.c_str(), "linear");
        const bool isInverse = crypto::str_cmp(instType.c_str(), "inverse");

        for (auto sym_val : list) {
            auto sym = sym_val.get_object();
            if (sym.error() != simdjson::SUCCESS) continue;

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));
            info.exchangeTypeEnum = BYBIT;
            info.value            = 1;

            std::string_view symbol, baseCoin, quoteCoin, contractType_sv, settleCoin_sv;

            if (isSpot) {
                // Spot 顺序: symbol → baseCoin → quoteCoin → ... → lotSizeFilter{...} → priceFilter{...}
                sym["symbol"].get(symbol);
                sym["baseCoin"].get(baseCoin);
                sym["quoteCoin"].get(quoteCoin);
            } else {
                // Linear/Inverse 顺序: symbol → contractType → status → baseCoin → quoteCoin → ... → settleCoin → ...
                sym["symbol"].get(symbol);
                sym["contractType"].get(contractType_sv);
                sym["baseCoin"].get(baseCoin);
                sym["quoteCoin"].get(quoteCoin);
            }

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            std::string baseCoinStr = crypto::to_upper(std::string(baseCoin));
            getBaseMagnifyNum(baseCoinStr, base, magnifyNumber, reduceNumber);
            std::string quoteCoinUpper = crypto::to_upper(std::string(quoteCoin));

            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base,         base);
            crypto::copy_sv_to_char_array(info.quote,        quoteCoinUpper);
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber  = reduceNumber;

            if (isSpot) {
                // Spot: lotSizeFilter 里 basePrecision → quotePrecision → minOrderQty → maxOrderQty → minOrderAmt → maxOrderAmt
                //       priceFilter    里 tickSize
                info.instTypeEnum = SPOT;
                crypto::copy_sv_to_char_array(info.margin, quoteCoinUpper);
                fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

                auto lot = sym["lotSizeFilter"].get_object();
                if (lot.error() == simdjson::SUCCESS) {
                    std::string_view basePrecision, minOrderQty, maxOrderQty, minOrderAmt;
                    lot["basePrecision"].get(basePrecision);
                    lot["minOrderQty"].get(minOrderQty);
                    lot["maxOrderQty"].get(maxOrderQty);
                    lot["minOrderAmt"].get(minOrderAmt);
                    info.lotSize   = crypto::fast_atod(basePrecision);
                    info.minSize   = crypto::fast_atod(minOrderQty);
                    if (!maxOrderQty.empty()) info.maxSize = crypto::fast_atod(maxOrderQty);
                    info.minAmount = crypto::fast_atod(minOrderAmt);
                }
                auto price = sym["priceFilter"].get_object();
                if (price.error() == simdjson::SUCCESS) {
                    std::string_view tickSize;
                    price["tickSize"].get(tickSize);
                    info.tickSize = crypto::fast_atod(tickSize);
                }
            }
            else if (isLinear || isInverse) {
                // Linear/Inverse: 拆合约名 (e.g. BTCUSDT / BTCUSDT-27DEC24 / BTCUSD / BTCUSDU25)
                // 精确类型区分靠 contractType
                sym["settleCoin"].get(settleCoin_sv);
                std::string settleCoinUpper = crypto::to_upper(std::string(settleCoin_sv));
                crypto::copy_sv_to_char_array(info.margin, settleCoinUpper);

                bool isFutures = false;
                if (contractType_sv == "LinearPerpetual")  info.instTypeEnum = USDT_SWAP;
                else if (contractType_sv == "LinearFutures") { info.instTypeEnum = USDT_FUTURES; isFutures = true; }
                else if (contractType_sv == "InversePerpetual") info.instTypeEnum = C_SWAP;
                else if (contractType_sv == "InverseFutures") { info.instTypeEnum = C_FUTURES;   isFutures = true; }

                if (isFutures) {
                    std::vector<std::string> vv = crypto::split(info.originInstId, "-");
                    if (!vv.empty()) {
                        fmt::format_to(info.instId, "{}-{}-{}", info.base, info.quote, vv.back());
                    }
                } else {
                    fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                }

                // Linear/Inverse: priceFilter → lotSizeFilter (leverageFilter 在 priceFilter 前但我们不用)
                auto price = sym["priceFilter"].get_object();
                if (price.error() == simdjson::SUCCESS) {
                    std::string_view tickSize;
                    price["tickSize"].get(tickSize);
                    info.tickSize = crypto::fast_atod(tickSize);
                }
                auto lot = sym["lotSizeFilter"].get_object();
                if (lot.error() == simdjson::SUCCESS) {
                    // lotSizeFilter 顺序: maxOrderQty → minOrderQty → qtyStep → postOnlyMaxOrderQty → minNotionalValue
                    std::string_view maxOrderQty, minOrderQty, qtyStep, minNotionalValue;
                    lot["maxOrderQty"].get(maxOrderQty);
                    lot["minOrderQty"].get(minOrderQty);
                    lot["qtyStep"].get(qtyStep);
                    lot["minNotionalValue"].get(minNotionalValue);
                    if (!maxOrderQty.empty()) info.maxSize = crypto::fast_atod(maxOrderQty);
                    info.minSize = crypto::fast_atod(minOrderQty);
                    info.lotSize = crypto::fast_atod(qtyStep);
                    info.minAmount = !minNotionalValue.empty()
                                   ? crypto::fast_atod(minNotionalValue)
                                   : info.minSize;
                }
            }

            updateInstrumentInfo(info);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("BybitInfo::getInfo exception: {}", e.what());
    }
}