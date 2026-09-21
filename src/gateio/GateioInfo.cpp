#include "gateio/GateioInfo.h"
#include <simdjson.h>


GateioInfo::GateioInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void GateioInfo::syncExchangeInfo() {
    getSpotInfo();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    getFuturesInfo("usdt");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    getFuturesInfo("btc");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    getDeliveryInfo("usdt");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    getDeliveryInfo("btc");
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

void GateioInfo::getSpotInfo() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("api.gateio.ws", "/api/v4/spot/currency_pairs", body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("GateioInfo::getSpotInfo error! status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("GateioInfo::getSpotInfo parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        // 顶层就是数组
        auto arr = doc.get_array();
        if (arr.error() != simdjson::SUCCESS) {
            LOG_ERROR("GateioInfo::getSpotInfo: top-level not array");
            return;
        }

        for (auto pair_val : arr) {
            auto pair = pair_val.get_object();
            if (pair.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // 严格 JSON 顺序: id → base → quote → ... → min_quote_amount → min_base_amount
            //                → amount_precision → precision → ... → max_base_amount → max_quote_amount
            std::string_view id, base_sv, quote_sv;
            std::string_view min_base_amount, min_quote_amount, max_quote_amount;
            int amount_precision = 0;
            int precision = 0;

            pair["id"].get(id);
            pair["base"].get(base_sv);
            pair["quote"].get(quote_sv);
            pair["min_base_amount"].get(min_base_amount);
            pair["min_quote_amount"].get(min_quote_amount);
            pair["max_quote_amount"].get(max_quote_amount);
            pair["amount_precision"].get(amount_precision);
            pair["precision"].get(precision);

            std::string baseAssetStr = crypto::to_upper(std::string(base_sv));
            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseAssetStr, base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = GATEIO;
            info.instTypeEnum = SPOT;
            crypto::copy_sv_to_char_array(info.originInstId, id);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quote_sv);
            crypto::copy_sv_to_char_array(info.margin, quote_sv);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
            info.value = 1;
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;
            info.calcType = 0;

            // Gate 的 precision 是"小数位数", 转换成 tick 值:
            //   precision=8 → tickSize=1e-8
    
            if (precision < 0) {
                precision = 0;
            }

            if (precision > 18) {
                precision = 18;
            }
            info.priceDigits = precision;
            info.pricePow10 = crypto::POW10_INT[precision];
            info.tickSizeInt = 1;
            info.tickSize = 1.0 / static_cast<double>(info.pricePow10);


            if (amount_precision < 0) {
                amount_precision = 0;
            }

            if (amount_precision > 18) {
                amount_precision = 18;
            }

            info.sizeDigits = amount_precision;
            info.sizePow10  = crypto::POW10_INT[amount_precision];
            info.lotSizeInt = 1;
            info.lotSize = 1.0 / static_cast<double>(info.sizePow10);

            info.minSize = crypto::fast_atod(min_base_amount);
            // info.maxSize = crypto::fast_atod(max_quote_amount);
            info.minAmount = crypto::fast_atod(min_quote_amount);

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
        LOG_ERROR("GateioInfo::getSpotInfo exception: {}", e.what());
    }
}


// ============================================================================
// Futures (perpetual): api.gateio.ws /api/v4/futures/{settle}/contracts
//   顶层是数组, 字段顺序 (Gate schema):
//     name, type, quanto_multiplier, ref_discount_rate, order_price_deviate,
//     maintenance_rate, mark_type, ..., order_price_round, order_size_min,
//     order_size_max, ...
// ============================================================================
void GateioInfo::getFuturesInfo(std::string settle) {
    try {
        std::string path = "/api/v4/futures/" + settle + "/contracts";
        int status = 0;
        std::string body;
        if (!syncGet("api.gateio.ws", path, body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("GateioInfo::getFuturesInfo error! status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("GateioInfo::getFuturesInfo parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        auto arr = doc.get_array();
        if (arr.error() != simdjson::SUCCESS) {
            LOG_ERROR("GateioInfo::getFuturesInfo: top-level not array");
            return;
        }

        for (auto c_val : arr) {
            auto c = c_val.get_object();
            if (c.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // 严格 JSON 顺序: name → ... → quanto_multiplier → ... → order_price_round → order_size_min → order_size_max
            std::string_view order_price_round, name, quanto_multiplier;
            int order_size_min, order_size_max;
            c["order_price_round"].get(order_price_round);
            c["order_size_min"].get(order_size_min);
            c["name"].get(name);
            c["order_size_max"].get(order_size_max);
            c["quanto_multiplier"].get(quanto_multiplier);
            
            std::vector<std::string> vv = crypto::split(std::string(name), "_");
            if (vv.size() < 2) {
                continue;
            }

            std::string baseAssetStr = crypto::to_upper(vv[0]);
            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseAssetStr, base, magnifyNumber, reduceNumber);
            std::string quoteUpper = crypto::to_upper(vv[1]);

            info.exchangeTypeEnum = GATEIO;
            if (crypto::str_cmp(settle, "usdt")) {
                info.instTypeEnum = USDT_SWAP;
                info.calcType = 0;
            }
            else if (crypto::str_cmp(settle, "btc")) {
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
            }

            crypto::copy_sv_to_char_array(info.originInstId, name);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteUpper);
            crypto::copy_sv_to_char_array(info.margin, quoteUpper);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
            info.value = crypto::fast_atod(quanto_multiplier);
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            info.tickSize = crypto::fast_atod(order_price_round);
            int d = countDecimalDigits(order_price_round);
            if (d > 18) {
                d = 18;
            }
            info.priceDigits = d;
            info.pricePow10 = crypto::POW10_INT[d];
            info.tickSizeInt = static_cast<int64_t>(std::llround(info.tickSize * static_cast<double>(info.pricePow10)));
            if (info.tickSizeInt <= 0) {
                info.tickSizeInt = 1;
            }


            if (order_size_min <= 0) {
                order_size_min = 1;
            }
            info.sizeDigits = 0;
            info.sizePow10  = 1;
            info.lotSizeInt = static_cast<int64_t>(order_size_min);
            info.lotSize = static_cast<double>(order_size_min);

            info.minSize = info.lotSize;
            info.maxSize = double(order_size_max);
            info.minAmount = info.lotSize;

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
        LOG_ERROR("GateioInfo::getFuturesInfo exception: {}", e.what());
    }
}


// ============================================================================
// Delivery (交割): api.gateio.ws /api/v4/delivery/{settle}/contracts
//   name 形如 BTC_USDT_20250328, 拆出 base/quote + 交割日期
// ============================================================================
void GateioInfo::getDeliveryInfo(std::string settle) {
    try {
        std::string path = "/api/v4/delivery/" + settle + "/contracts";
        int status = 0;
        std::string body;
        if (!syncGet("api.gateio.ws", path, body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("GateioInfo::getDeliveryInfo error! status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("GateioInfo::getDeliveryInfo parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        auto arr = doc.get_array();
        if (arr.error() != simdjson::SUCCESS) {
            LOG_ERROR("GateioInfo::getDeliveryInfo: top-level not array");
            return;
        }

        for (auto c_val : arr) {
            auto c = c_val.get_object();
            if (c.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            std::string_view order_price_round, name, quanto_multiplier;
            int order_size_min, order_size_max;

            c["order_price_round"].get(order_price_round);
            c["order_size_min"].get(order_size_min);
            c["name"].get(name);
            c["order_size_max"].get(order_size_max);
            c["quanto_multiplier"].get(quanto_multiplier);
            

            std::vector<std::string> vv = crypto::split(std::string(name), "_");
            if (vv.size() < 3) {
                continue;   // 交割合约必须有 3 段
            }

            std::string baseAssetStr = crypto::to_upper(vv[0]);
            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseAssetStr, base, magnifyNumber, reduceNumber);
            std::string quoteUpper = crypto::to_upper(vv[1]);

            info.exchangeTypeEnum = GATEIO;
            if (crypto::str_cmp(settle, "usdt")) {
                info.instTypeEnum = USDT_FUTURES;
                info.calcType = 0;
            }
            else if (crypto::str_cmp(settle, "btc")) {
                info.instTypeEnum = C_FUTURES;
                info.calcType = 1;
            }

            crypto::copy_sv_to_char_array(info.originInstId, name);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteUpper);
            crypto::copy_sv_to_char_array(info.margin, quoteUpper);

            // Gate delivery 的日期段形如 "20250328", 老代码取后 6 位 (250328)
            std::string thirdStr = vv[2].size() > 2 ? vv[2].substr(2) : vv[2];
            fmt::format_to(info.instId, "{}-{}-{}", info.base, info.quote, thirdStr);

            info.value = crypto::fast_atod(quanto_multiplier);
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            info.tickSize = crypto::fast_atod(order_price_round);
            int d = countDecimalDigits(order_price_round);
            if (d > 18) {
                d = 18;
            }
            info.priceDigits = d;
            info.pricePow10 = crypto::POW10_INT[d];
            info.tickSizeInt = static_cast<int64_t>(std::llround(info.tickSize * static_cast<double>(info.pricePow10)));
            if (info.tickSizeInt <= 0) {
                info.tickSizeInt = 1;
            }


            if (order_size_min <= 0) {
                order_size_min = 1;
            }
            info.sizeDigits = 0;
            info.sizePow10  = 1;
            info.lotSizeInt = static_cast<int64_t>(order_size_min);
            info.lotSize = static_cast<double>(order_size_min);

            info.minSize = info.lotSize;
            info.maxSize = double(order_size_max);
            info.minAmount = info.lotSize;


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
        LOG_ERROR("GateioInfo::getDeliveryInfo exception: {}", e.what());
    }
}