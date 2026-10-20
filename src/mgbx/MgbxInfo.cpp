#include "mgbx/MgbxInfo.h"
#include "base/InfoHelper.h"
#include "base/JsonField.h"

#include <chrono>
#include <cstring>
#include <thread>

MgbxInfo::MgbxInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void MgbxInfo::syncExchangeInfo() {
    getSymbolList();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ============================================================================
// 第 1 步：GET open.mgbx.com/fut/v1/public/q/tickers
//   这个接口只有行情（symbol + 价格 + 量），**没有合约规格**，
//   所以只能当"名单"用。规格必须逐个查 symbol/detail。
// ============================================================================
int MgbxInfo::fetchSymbols(std::vector<std::string>& symbols) {
    symbols.clear();

    int status = 0;
    std::string body;
    if (!syncGet("open.mgbx.com", "/fut/v1/public/q/tickers", body, status)) {
        return -1;
    }
    if (status != 200) {
        LOG_ERROR("MgbxInfo::fetchSymbols http status {}", status);
        return -1;
    }

    simdjson::dom::parser parser;
    simdjson::padded_string padded(body);
    auto doc = parser.parse(padded);
    if (doc.error()) {
        LOG_ERROR("MgbxInfo::fetchSymbols parse error: {}", simdjson::error_message(doc.error()));
        return -1;
    }

    simdjson::dom::object root;
    if (doc.get_object().get(root) != simdjson::SUCCESS) {
        LOG_ERROR("MgbxInfo: tickers response is not an object");
        return -1;
    }

    int64_t code = 0;
    if (jf::i64(root, {"code"}, code) && code != 0) {
        LOG_ERROR("MgbxInfo: tickers api code={} msg={}", code, jf::str_or(root, {"msg"}));
        return -1;
    }

    simdjson::dom::array arr;
    if (!jf::array_at(root, "data", arr)) {
        LOG_ERROR("MgbxInfo: tickers 'data' missing or not array");
        return -1;
    }

    for (auto row : arr) {
        simdjson::dom::object o;
        if (row.get_object().get(o) != simdjson::SUCCESS) {
            continue;
        }
        std::string sym = jf::str_or(o, {"s", "symbol"});
        if (!sym.empty()) {
            symbols.push_back(std::move(sym));
        }
    }
    return static_cast<int>(symbols.size());
}

// ============================================================================
// 第 2 步：GET open.mgbx.com/fut/v1/public/symbol/detail?symbol=btc_usdt
//   data 是**对象**不是数组；不带 symbol 返回 {"code":-1,"msg":"invalid symbol"}。
//
// 返回值: 1 = 已写入, 0 = 不可交易但已处理, -1 = 失败要重试。
// 区分 0 和 -1 是为了让"暂停交易的合约"不要每 60 秒重查一次 —— 283 个合约里
// 只要有几个长期停牌，不区分就会变成每轮固定几百个请求。
// ============================================================================
int MgbxInfo::fetchDetail(const std::string& symbol) {
    const std::string target = "/fut/v1/public/symbol/detail?symbol=" + symbol;

    int status = 0;
    std::string body;
    if (!syncGet("open.mgbx.com", target, body, status)) {
        return -1;
    }
    if (status != 200) {
        return -1;
    }

    simdjson::dom::parser parser;
    simdjson::padded_string padded(body);
    auto doc = parser.parse(padded);
    if (doc.error()) {
        return -1;
    }

    simdjson::dom::object root;
    if (doc.get_object().get(root) != simdjson::SUCCESS) {
        return -1;
    }

    int64_t code = 0;
    if (jf::i64(root, {"code"}, code) && code != 0) {
        return -1;
    }

    simdjson::dom::object o;
    if (!jf::object_at(root, "data", o)) {
        return -1;
    }

    // state: 0 = 正常；tradeSwitch: false = 暂停交易
    int64_t state = 0;
    if (jf::i64(o, {"state"}, state) && state != 0) {
        return 0;
    }
    bool tradable = true;
    if (jf::boolean(o, {"tradeSwitch"}, tradable) && !tradable) {
        return 0;
    }

    std::string sym      = jf::str_or(o, {"symbol"}, symbol.c_str());
    std::string baseRaw  = jf::str_or(o, {"baseCoin"});
    std::string quote    = crypto::to_upper(jf::str_or(o, {"quoteCoin"}));
    if (baseRaw.empty() || quote.empty()) {
        // 兜底：从 btc_usdt 里拆（'_' 是这家的分隔符）
        const size_t p = sym.find('_');
        if (p == std::string::npos || p == 0 || p + 1 >= sym.size()) {
            return 0;
        }
        baseRaw = sym.substr(0, p);
        if (quote.empty()) {
            quote = crypto::to_upper(sym.substr(p + 1));
        }
    }

    md::InstrumentInfo info;
    memset(&info, 0, sizeof(md::InstrumentInfo));

    std::string base;
    double magnifyNumber = 1.0, reduceNumber = 1.0;
    getBaseMagnifyNum(crypto::to_upper(baseRaw), base, magnifyNumber, reduceNumber);

    info.exchangeTypeEnum = MGBX;
    crypto::copy_sv_to_char_array(info.originInstId, sym);
    crypto::copy_sv_to_char_array(info.base, base);
    crypto::copy_sv_to_char_array(info.quote, quote);
    fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

    // contractSize = 一张合约代表多少标的币（字符串，如 "0.0001" / "10" / "1000"）
    double value = 0.0;
    if (jf::dbl(o, {"contractSize"}, value) && value > 0.0) {
        info.value = value;
    }
    else {
        info.value = 1.0;
    }
    info.magnifyNumber = magnifyNumber;
    info.reduceNumber = reduceNumber;

    std::string underlyingType = jf::str_or(o, {"underlyingType"});
    if (underlyingType == "COIN_BASED" || quote == "USD") {
        info.instTypeEnum = C_SWAP;
        info.calcType = 1;
        crypto::copy_sv_to_char_array(info.margin, info.base);
    }
    else {
        info.calcType = 0;
        info.instTypeEnum = (quote == "USDC") ? USDC_SWAP : USDT_SWAP;
        crypto::copy_sv_to_char_array(info.margin, info.quote);
    }

    // 价格 tick：优先 minStepPrice（"0.1"），退回 pricePrecision（小数位）
    std::string tick = jf::str_or(o, {"minStepPrice"});
    if (!tick.empty()) {
        infohelp::priceFromDecimal(info, tick);
    }
    else {
        int64_t pricePrec = 0;
        if (jf::i64(o, {"pricePrecision"}, pricePrec)) {
            infohelp::priceFromStep(info, 1.0, static_cast<int>(pricePrec));
        }
    }

    // 张数步长：quantityPrecision 是小数位（0 ⇒ 步长 1 张）
    int64_t qtyPrec = 0;
    if (jf::i64(o, {"quantityPrecision"}, qtyPrec)) {
        infohelp::sizeFromStep(info, 1.0, static_cast<int>(qtyPrec));
    }

    double v = 0.0;
    if (jf::dbl(o, {"minQty"}, v) && v > 0.0) {
        info.minSize = v;
    }
    if (jf::dbl(o, {"minNotional"}, v) && v > 0.0) {
        info.minAmount = v;   // USDT 金额，不是张数
    }

    infohelp::ensurePrecisionDefaults(info);
    updateInstrumentInfo(info);
    return 1;
}

void MgbxInfo::getSymbolList() {
    try {
        std::vector<std::string> symbols;
        const int n = fetchSymbols(symbols);
        if (n < 0) {
            return;
        }

        int fetched = 0, untradable = 0, cached = 0, failed = 0;
        for (const std::string& sym : symbols) {
            if (seen_.find(sym) != seen_.end()) {
                ++cached;
                continue;
            }
            const int r = fetchDetail(sym);
            if (r >= 0) {
                // 0 也要记进 seen_：不可交易是"已处理"，不该每轮重查
                seen_.insert(sym);
                (r == 1 ? fetched : untradable) += 1;
            }
            else {
                ++failed;   // 网络/解析失败，下一轮再试
            }
        }

        LOG_INFO("MgbxInfo -> total={} fetched={} untradable={} cached={} failed={}",
                 n, fetched, untradable, cached, failed);
        if (seen_.empty()) {
            LOG_ERROR("MgbxInfo: 0 instruments after full pass. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("MgbxInfo::getSymbolList exception: {}", e.what());
    }
}
