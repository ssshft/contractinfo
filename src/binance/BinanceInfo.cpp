#include "binance/BinanceInfo.h"
#include <simdjson.h>


BinanceInfo::BinanceInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void BinanceInfo::syncExchangeInfo() {
    getSpotInfo();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    getUFutureInfo();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    getCFutureInfo();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}


// ============================================================================
// filter 分派: spot / futures / coinm 通用逻辑, 走同一 helper
// ============================================================================
namespace {
inline void apply_filter(simdjson::ondemand::object& filter, md::InstrumentInfo& info, bool isSpot, bool isCFuture) {
    std::string_view filterType_sv, tickSize_sv, minQty_sv, maxQty_sv, stepSize_sv;
    std::string_view minNotional_sv, notional_sv;

    // 一次前向遍历, 顺序无关, 每个字段只扫一遍, 无 rewind
    for (auto field : filter) {
        auto ku = field.unescaped_key();
        if (ku.error()) {
            continue;
        }

        std::string_view k = ku.value_unsafe();
        auto value = field.value();

        if (k == "filterType") {
            value.get(filterType_sv);
        }
        else if (k == "tickSize") {
            value.get(tickSize_sv);
        }   
        else if (k == "minQty") {
            value.get(minQty_sv);
        }
        else if (k == "maxQty") {
            value.get(maxQty_sv);
        } 
        else if (k == "stepSize") {
            value.get(stepSize_sv);
        }  
        else if (k == "minNotional") {
            value.get(minNotional_sv);
        }
        else if (k == "notional") {
            value.get(notional_sv);
        }  
        // 其他字段 (limit / multiplier* / ...) 我们不关心, 让 simdjson 跳过
    }

    // 拿到所有字段后, 按 filterType 分发, 语义清晰
    if (filterType_sv == "PRICE_FILTER") {
        if (!tickSize_sv.empty()) {
            info.tickSize = crypto::fast_atod(tickSize_sv);
            int d = crypto::countDecimalDigits(tickSize_sv);
            if (d > 18) {
                d = 18;
            }
            info.priceDigits = d;
            info.pricePow10 = crypto::POW10_INT[d];
            info.tickSizeInt = static_cast<int64_t>(std::llround(info.tickSize * static_cast<double>(info.pricePow10)));
            if (info.tickSizeInt <= 0) {
                info.tickSizeInt = 1;
            }
        }
    }
    else if (filterType_sv == "LOT_SIZE") {
        if (isSpot) {
            if (!minQty_sv.empty()) {
                info.lotSize = crypto::fast_atod(minQty_sv);
                int d = crypto::countDecimalDigits(minQty_sv);
                if (d > 18) {
                    d = 18;
                }
                info.sizeDigits = d;
                info.sizePow10 = crypto::POW10_INT[d];
                info.lotSizeInt = static_cast<int64_t>(std::llround(info.lotSize * static_cast<double>(info.sizePow10)));
                if (info.lotSizeInt <= 0) {
                    info.lotSizeInt = 1;
                }
            }
        } else {
            if (!stepSize_sv.empty()) {
                info.lotSize = crypto::fast_atod(stepSize_sv);
                int d = crypto::countDecimalDigits(stepSize_sv);
                if (d > 18) {
                    d = 18;
                }
                info.sizeDigits = d;
                info.sizePow10 = crypto::POW10_INT[d];
                info.lotSizeInt = static_cast<int64_t>(std::llround(info.lotSize * static_cast<double>(info.sizePow10)));
                if (info.lotSizeInt <= 0) {
                    info.lotSizeInt = 1;
                }
            }

            if (isCFuture) {
                if (!minQty_sv.empty()) {
                    info.minAmount = crypto::fast_atod(minQty_sv);
                }
            }
        }

        if (!minQty_sv.empty()) {
            info.minSize = crypto::fast_atod(minQty_sv);
        }

        if (!maxQty_sv.empty()) {
            info.maxSize = crypto::fast_atod(maxQty_sv);
        }
    }
    else if (filterType_sv == "NOTIONAL") {
        std::string_view v = isSpot ? minNotional_sv : notional_sv;
        if (!v.empty()) {
            info.minAmount = crypto::fast_atod(v);
        }
    }
    // MARKET_LOT_SIZE / MAX_NUM_ORDERS / PERCENT_PRICE 我们不用, 忽略
}



} // anonymous namespace


// ============================================================================
// Spot: api.binance.com / api/v3/exchangeInfo
//   symbols[] JSON 字段顺序 (Binance schema):
//     symbol, status, baseAsset, baseAssetPrecision,
//     quoteAsset, quotePrecision, ..., filters, ...
//   访问顺序对齐 JSON, 避免 simdjson ondemand 回退。
// ============================================================================
void BinanceInfo::getSpotInfo() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("api.binance.com", "/api/v3/exchangeInfo", body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("getSpotInfo error! status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("getSpotInfo simdjson parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        auto symbols = doc["symbols"].get_array();
        if (symbols.error() != simdjson::SUCCESS) {
            LOG_ERROR("getSpotInfo: 'symbols' missing or not array");
            return;
        }

        for (auto sym_val : symbols) {
            auto sym = sym_val.get_object();
            if (sym.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // ---- 按 JSON 顺序访问 ----
            std::string_view symbol, baseAsset, quoteAsset;
            sym["symbol"].get(symbol);
            sym["baseAsset"].get(baseAsset);
            sym["quoteAsset"].get(quoteAsset);

            // 计算放大倍数 (基于 base 名字前缀数字, e.g. "1000FLOKI" → base=FLOKI mul=1000)
            std::string baseAssetStr(baseAsset);
            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseAssetStr, base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = BINANCE;
            info.instTypeEnum = SPOT;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteAsset);
            crypto::copy_sv_to_char_array(info.margin, quoteAsset);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
            info.value = 1;
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;
            info.calcType = 0;

            // filters 数组 (在对象末尾, JSON 顺序访问符合规则)
            auto filters = sym["filters"].get_array();
            if (filters.error() == simdjson::SUCCESS) {
                for (auto f_val : filters) {
                    auto f = f_val.get_object();
                    if (f.error() != simdjson::SUCCESS) {
                        continue;
                    }
                    apply_filter(f.value(), info, /*isSpot=*/true, /*isCFuture=*/false);
                }
            }

            if (info.pricePow10 <= 0) { 
                info.priceDigits = 0; 
                info.pricePow10 = 1; 
                info.tickSizeInt = 1; 
            }

            if (info.sizePow10 <= 0) { 
                info.sizeDigits = 0; 
                info.sizePow10 = 1; 
                info.lotSizeInt = 1; 
            }

            updateInstrumentInfo(info);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("getSpotInfo exception: {}", e.what());
    }
}


// ============================================================================
// USDT-M Futures: fapi.binance.com / fapi/v1/exchangeInfo
//   symbols[] JSON 字段顺序 (Binance schema):
//     symbol, pair, contractType, deliveryDate, onboardDate, status,
//     maintMarginPercent, requiredMarginPercent,
//     baseAsset, quoteAsset, marginAsset, pricePrecision, ...
//     filters, orderTypes, timeInForce
//   ⚠️ contractType 在 baseAsset 之前, 访问顺序必须一致否则 ondemand 会全对象重扫。
// ============================================================================
void BinanceInfo::getUFutureInfo() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("fapi.binance.com", "/fapi/v1/exchangeInfo", body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("getUFutureInfo error, status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("getUFutureInfo simdjson parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        auto symbols = doc["symbols"].get_array();
        if (symbols.error() != simdjson::SUCCESS) {
            LOG_ERROR("getUFutureInfo: 'symbols' missing or not array");
            return;
        }

        for (auto sym_val : symbols) {
            auto sym = sym_val.get_object();
            if (sym.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // ---- 严格按 JSON 顺序: symbol → contractType → baseAsset → quoteAsset → marginAsset ----
            std::string_view symbol, contractType, baseAsset, quoteAsset, marginAsset;
            sym["symbol"].get(symbol);
            sym["contractType"].get(contractType);
            sym["baseAsset"].get(baseAsset);
            sym["quoteAsset"].get(quoteAsset);
            sym["marginAsset"].get(marginAsset);

            std::string baseAssetStr(baseAsset);
            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseAssetStr, base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = BINANCE;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteAsset);
            crypto::copy_sv_to_char_array(info.margin, marginAsset);
            info.value = 1;
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (contractType.find("PERPETUAL") != std::string_view::npos) {
                fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                if (crypto::str_cmp(info.margin, "USDT")) {
                    info.instTypeEnum = USDT_SWAP;
                    info.calcType = 0;
                }
                if (crypto::str_cmp(info.margin, "USDC")) {
                    info.instTypeEnum = USDC_SWAP;
                    info.calcType = 0;
                }
                else if (crypto::str_cmp(info.margin, "BUSD")) {
                    info.instTypeEnum = BUSD_SWAP;
                    info.calcType = 0;
                }
                else if (crypto::str_cmp(info.base, info.margin)) {
                    info.instTypeEnum = C_SWAP;
                    info.calcType = 1;
                } 
            }
            else if (contractType.find("QUARTER") != std::string_view::npos) {
                std::vector<std::string> vv = crypto::split(info.originInstId, "_");
                if (!vv.empty()) {
                    fmt::format_to(info.instId, "{}-{}-{}", info.base, info.quote, vv.back());
                }
                if (crypto::str_cmp(info.margin, "USDT")) {
                    info.instTypeEnum = USDT_FUTURES;
                    info.calcType = 0;
                }
                else if (crypto::str_cmp(info.margin, "BUSD")) {
                    info.instTypeEnum = BUSD_FUTURES;
                    info.calcType = 0;
                }
                else if (crypto::str_cmp(info.base, info.margin)) {
                    info.instTypeEnum = C_FUTURES;
                    info.calcType = 1;
                }
            }

            auto filters = sym["filters"].get_array();
            if (filters.error() == simdjson::SUCCESS) {
                for (auto f_val : filters) {
                    auto f = f_val.get_object();
                    if (f.error() != simdjson::SUCCESS) {
                        continue;
                    }
                    apply_filter(f.value(), info, /*isSpot=*/false, /*isCFuture=*/false);
                }
            }

            if (info.pricePow10 <= 0) { 
                info.priceDigits = 0; 
                info.pricePow10 = 1; 
                info.tickSizeInt = 1; 
            }

            if (info.sizePow10 <= 0) { 
                info.sizeDigits = 0; 
                info.sizePow10 = 1; 
                info.lotSizeInt = 1; 
            }

            updateInstrumentInfo(info);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("getUFutureInfo exception: {}", e.what());
    }
}


// ============================================================================
// Coin-M Futures: dapi.binance.com / dapi/v1/exchangeInfo
//   symbols[] JSON 字段顺序 (Binance schema):
//     symbol, pair, contractType, deliveryDate, onboardDate, contractStatus,
//     contractSize, marginAsset, maintMarginPercent, requiredMarginPercent,
//     baseAsset, quoteAsset, pricePrecision, ...
//     filters
//   ⚠️ contractType/contractSize/marginAsset 都在 baseAsset 之前, 访问顺序必须对齐。
//   Binance CoinM 返回的字符串本身就大写, 不必再 to_upper。
// ============================================================================
void BinanceInfo::getCFutureInfo() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("dapi.binance.com", "/dapi/v1/exchangeInfo", body, status)) {
            return;
        }
        if (status != 200) {
            LOG_ERROR("getCFutureInfo error, status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("getCFutureInfo simdjson parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        auto symbols = doc["symbols"].get_array();
        if (symbols.error() != simdjson::SUCCESS) {
            LOG_ERROR("getCFutureInfo: 'symbols' missing or not array");
            return;
        }

        for (auto sym_val : symbols) {
            auto sym = sym_val.get_object();
            if (sym.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            std::string_view symbol, contractType, marginAsset, baseAsset, quoteAsset;
            sym["symbol"].get(symbol);
            sym["contractType"].get(contractType);
            sym["marginAsset"].get(marginAsset);
            sym["baseAsset"].get(baseAsset);
            sym["quoteAsset"].get(quoteAsset);

            int contractSize = 0;
            sym["contractSize"].get(contractSize);

            std::string baseAssetStr(baseAsset);
            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseAssetStr, base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = BINANCE;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteAsset);
            crypto::copy_sv_to_char_array(info.margin, marginAsset);
            info.value = contractSize;
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (contractType.find("PERPETUAL") != std::string_view::npos) {
                fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                if (crypto::str_cmp(info.margin, "USDT")) {
                    info.instTypeEnum = USDT_SWAP;
                }
                else if (crypto::str_cmp(info.margin, "BUSD")) {
                    info.instTypeEnum = BUSD_SWAP;
                }
                else if (crypto::str_cmp(info.base, info.margin)) {
                    info.instTypeEnum = C_SWAP;
                }
            }
            else if (contractType.find("QUARTER") != std::string_view::npos) {
                std::vector<std::string> vv = crypto::split(info.originInstId, "_");
                if (!vv.empty()) {
                    fmt::format_to(info.instId, "{}-{}-{}", info.base, info.quote, vv.back());
                }
                if (crypto::str_cmp(info.margin, "USDT")) {
                    info.instTypeEnum = USDT_FUTURES;
                }
                else if (crypto::str_cmp(info.margin, "BUSD")) {
                    info.instTypeEnum = BUSD_FUTURES;
                }
                else if (crypto::str_cmp(info.base, info.margin)) {
                    info.instTypeEnum = C_FUTURES;
                }
            }

            auto filters = sym["filters"].get_array();
            if (filters.error() == simdjson::SUCCESS) {
                for (auto f_val : filters) {
                    auto f = f_val.get_object();
                    if (f.error() != simdjson::SUCCESS) {
                        continue;
                    }
                    apply_filter(f.value(), info, /*isSpot=*/false, /*isCFuture=*/true);
                }
            }

            if (info.pricePow10 <= 0) { 
                info.priceDigits = 0; 
                info.pricePow10 = 1; 
                info.tickSizeInt = 1; 
            }

            if (info.sizePow10 <= 0) { 
                info.sizeDigits = 0; 
                info.sizePow10 = 1; 
                info.lotSizeInt = 1; 
            }

            updateInstrumentInfo(info);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("getCFutureInfo exception: {}", e.what());
    }
}
