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


// ============================================================================
// Spot: api.gateio.ws /api/v4/spot/currency_pairs
//   顶层就是数组, 每个 pair 字段顺序:
//     id, base, quote, fee, min_quote_amount, min_base_amount, amount_precision,
//     precision, trade_status, sell_start, buy_start, delisting_time,
//     max_base_amount, max_quote_amount, type
// ============================================================================
void GateioInfo::getSpotInfo() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("api.gateio.ws", "/api/v4/spot/currency_pairs", body, status)) return;
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
            if (pair.error() != simdjson::SUCCESS) continue;

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // 严格 JSON 顺序: id → base → quote → ... → min_quote_amount → min_base_amount
            //                → amount_precision → precision → ... → max_base_amount → max_quote_amount
            std::string_view id, base_sv, quote_sv;
            std::string_view min_quote_amount, min_base_amount;
            std::string_view amount_precision, precision;
            std::string_view max_base_amount, max_quote_amount;

            pair["id"].get(id);
            pair["base"].get(base_sv);
            pair["quote"].get(quote_sv);
            pair["min_quote_amount"].get(min_quote_amount);
            pair["min_base_amount"].get(min_base_amount);
            pair["amount_precision"].get(amount_precision);
            pair["precision"].get(precision);
            pair["max_base_amount"].get(max_base_amount);
            pair["max_quote_amount"].get(max_quote_amount);

            std::string baseAssetStr = crypto::to_upper(std::string(base_sv));
            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseAssetStr, base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = GATEIO;
            info.instTypeEnum     = SPOT;
            crypto::copy_sv_to_char_array(info.originInstId, id);
            crypto::copy_sv_to_char_array(info.base,         base);
            crypto::copy_sv_to_char_array(info.quote,        quote_sv);
            crypto::copy_sv_to_char_array(info.margin,       quote_sv);
            fmt::format_to      (info.instId, "{}-{}", info.base, info.quote);
            info.value          = 1;
            info.magnifyNumber  = magnifyNumber;
            info.reduceNumber   = reduceNumber;

            // Gate 的 precision 是"小数位数", 转换成 tick 值:
            //   precision=8 → tickSize=1e-8
            double pxDigits = crypto::fast_atod(precision);
            double amtDigits = crypto::fast_atod(amount_precision);
            info.tickSize = std::pow(10.0, -pxDigits);
            info.lotSize  = std::pow(10.0, -amtDigits);

            info.minSize   = crypto::fast_atod(min_base_amount);
            info.maxSize   = crypto::fast_atod(max_base_amount);
            info.minAmount = crypto::fast_atod(min_quote_amount);

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
        if (!syncGet("api.gateio.ws", path, body, status)) return;
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
            if (c.error() != simdjson::SUCCESS) continue;

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // 严格 JSON 顺序: name → ... → quanto_multiplier → ... → order_price_round → order_size_min → order_size_max
            std::string_view name, quanto_multiplier, order_price_round, order_size_min, order_size_max;
            c["name"].get(name);
            c["quanto_multiplier"].get(quanto_multiplier);
            c["order_price_round"].get(order_price_round);
            c["order_size_min"].get(order_size_min);
            c["order_size_max"].get(order_size_max);

            std::vector<std::string> vv = crypto::split(std::string(name), "_");
            if (vv.size() < 2) continue;

            std::string baseAssetStr = crypto::to_upper(vv[0]);
            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseAssetStr, base, magnifyNumber, reduceNumber);
            std::string quoteUpper = crypto::to_upper(vv[1]);

            info.exchangeTypeEnum = GATEIO;
            if      (crypto::str_cmp(settle, "usdt")) info.instTypeEnum = USDT_SWAP;
            else if (crypto::str_cmp(settle, "btc"))  info.instTypeEnum = C_SWAP;

            crypto::copy_sv_to_char_array(info.originInstId, name);
            crypto::copy_sv_to_char_array(info.base,         base);
            crypto::copy_sv_to_char_array(info.quote,        quoteUpper);
            crypto::copy_sv_to_char_array(info.margin,       quoteUpper);
            fmt::format_to      (info.instId, "{}-{}", info.base, info.quote);
            info.value          = crypto::fast_atod(quanto_multiplier);
            info.magnifyNumber  = magnifyNumber;
            info.reduceNumber   = reduceNumber;
            info.tickSize       = crypto::fast_atod(order_price_round);
            info.lotSize        = crypto::fast_atod(order_size_min);
            info.minSize        = info.lotSize;
            info.maxSize        = crypto::fast_atod(order_size_max);
            info.minAmount      = info.lotSize;

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
        if (!syncGet("api.gateio.ws", path, body, status)) return;
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
            if (c.error() != simdjson::SUCCESS) continue;

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            std::string_view name, quanto_multiplier, order_price_round, order_size_min, order_size_max;
            c["name"].get(name);
            c["quanto_multiplier"].get(quanto_multiplier);
            c["order_price_round"].get(order_price_round);
            c["order_size_min"].get(order_size_min);
            c["order_size_max"].get(order_size_max);

            std::vector<std::string> vv = crypto::split(std::string(name), "_");
            if (vv.size() < 3) continue;   // 交割合约必须有 3 段

            std::string baseAssetStr = crypto::to_upper(vv[0]);
            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseAssetStr, base, magnifyNumber, reduceNumber);
            std::string quoteUpper = crypto::to_upper(vv[1]);

            info.exchangeTypeEnum = GATEIO;
            if      (crypto::str_cmp(settle, "usdt")) info.instTypeEnum = USDT_FUTURES;
            else if (crypto::str_cmp(settle, "btc"))  info.instTypeEnum = C_FUTURES;

            crypto::copy_sv_to_char_array(info.originInstId, name);
            crypto::copy_sv_to_char_array(info.base,         base);
            crypto::copy_sv_to_char_array(info.quote,        quoteUpper);
            crypto::copy_sv_to_char_array(info.margin,       quoteUpper);

            // Gate delivery 的日期段形如 "20250328", 老代码取后 6 位 (250328)
            std::string thirdStr = vv[2].size() > 2 ? vv[2].substr(2) : vv[2];
            fmt::format_to(info.instId, "{}-{}-{}", info.base, info.quote, thirdStr);

            info.value          = crypto::fast_atod(quanto_multiplier);
            info.magnifyNumber  = magnifyNumber;
            info.reduceNumber   = reduceNumber;
            info.tickSize       = crypto::fast_atod(order_price_round);
            info.lotSize        = crypto::fast_atod(order_size_min);
            info.minSize        = info.lotSize;
            info.maxSize        = crypto::fast_atod(order_size_max);
            info.minAmount      = info.lotSize;

            updateInstrumentInfo(info);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("GateioInfo::getDeliveryInfo exception: {}", e.what());
    }
}