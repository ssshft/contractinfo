#include "phemex/PhemexInfo.h"
#include "base/InfoHelper.h"
#include "base/JsonField.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace {

// V1 (products) 没有 base 字段，用 symbol 去掉 quote 后缀反推：BTCUSD - USD = BTC
std::string stripQuote(const std::string& sym, const std::string& quote) {
    if (!quote.empty() && sym.size() > quote.size() &&
        sym.compare(sym.size() - quote.size(), quote.size(), quote) == 0) {
        return sym.substr(0, sym.size() - quote.size());
    }
    return sym;
}

bool isLinearSettle(const std::string& settle) {
    return settle == "USDT" || settle == "USDC" || settle == "USD";
}

}  // namespace

PhemexInfo::PhemexInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void PhemexInfo::syncExchangeInfo() {
    getSymbolList();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ============================================================================
// GET https://api.phemex.com/public/products
//
// data.products[]        —— V1，只有币本位永续（type=="Perpetual"，quote=USD）
//   {"symbol":"BTCUSD","type":"Perpetual","contractUnderlyingAssets":"USD",
//    "settleCurrency":"BTC","quoteCurrency":"USD","contractSize":1.0,"lotSize":1,
//    "tickSize":0.1,"priceScale":4,"pricePrecision":1,"maxOrderQty":20000000,
//    "status":"Listed", ...}
//   ⚠️ contractUnderlyingAssets 是 "USD" 而不是 base，别拿它当 base。
//
// data.perpProductsV2[]  —— V2，USDT / USDC 本位
//   {"symbol":"BTCUSDT","type":"PerpetualV2","contractUnderlyingAssets":"BTC",
//    "settleCurrency":"USDT","quoteCurrency":"USDT","tickSize":"0.1",
//    "pricePrecision":1,"baseCurrency":"BTC","status":"Listed",
//    "maxOrderQtyRq":"1000","minOrderValueRv":"1","qtyPrecision":3,
//    "qtyStepSize":"0.001", ...}
//   ⚠️ V2 的 tickSize / qtyStepSize 是**字符串**，V1 的 tickSize / lotSize 是 **double**。
//   ⚠️ V2 没有 contractSize 字段，文档写 "each contract is worth 1 <underlying>"，
//      所以面值取 1。
// ============================================================================
void PhemexInfo::getSymbolList() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("api.phemex.com", "/public/products", body, status)) {
            return;
        }
        if (status != 200) {
            LOG_ERROR("PhemexInfo::getSymbolList http status {}", status);
            return;
        }

        simdjson::dom::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.parse(padded);
        if (doc.error()) {
            LOG_ERROR("PhemexInfo::getSymbolList parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        simdjson::dom::object root;
        if (doc.get_object().get(root) != simdjson::SUCCESS) {
            LOG_ERROR("PhemexInfo: response is not an object");
            return;
        }

        int64_t code = 0;
        if (jf::i64(root, {"code"}, code) && code != 0) {
            LOG_ERROR("PhemexInfo: api code={} msg={}", code, jf::str_or(root, {"msg"}));
            return;
        }

        simdjson::dom::object data;
        if (!jf::object_at(root, "data", data)) {
            LOG_ERROR("PhemexInfo: 'data' missing or not object");
            return;
        }

        int keptV1 = 0, keptV2 = 0, skipStatus = 0, skipType = 0;

        auto emit = [&](const std::string& symbol, std::string baseRaw, const std::string& quote,
                        const std::string& settle, double value, bool inverse,
                        auto&& setPrecision) -> bool {
            if (symbol.empty() || quote.empty()) {
                return false;
            }
            if (baseRaw.empty()) {
                baseRaw = stripQuote(symbol, quote);
            }
            if (baseRaw.empty()) {
                return false;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseRaw, base, magnifyNumber, reduceNumber);

            std::string quoteUpper = crypto::to_upper(quote);
            info.exchangeTypeEnum = PHEMEX;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteUpper);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.value = (value > 0.0) ? value : 1.0;
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (inverse) {
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
                const std::string& m = settle.empty() ? base : settle;
                crypto::copy_sv_to_char_array(info.margin, m);
            }
            else {
                info.calcType = 0;
                info.instTypeEnum = (quoteUpper == "USDC") ? USDC_SWAP : USDT_SWAP;
                crypto::copy_sv_to_char_array(info.margin, info.quote);
            }

            setPrecision(info);
            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            return true;
        };

        // ---- V1: 币本位永续 ----
        simdjson::dom::array arr1;
        if (jf::array_at(data, "products", arr1)) {
            for (auto row : arr1) {
                simdjson::dom::object o;
                if (row.get_object().get(o) != simdjson::SUCCESS) {
                    continue;
                }
                if (jf::str_or(o, {"type"}) != "Perpetual") {
                    ++skipType;
                    continue;
                }
                if (jf::str_or(o, {"status"}) != "Listed") {
                    ++skipStatus;
                    continue;
                }
                std::string symbol = jf::str_or(o, {"symbol"});
                std::string quote  = jf::str_or(o, {"quoteCurrency"});
                std::string settle = jf::str_or(o, {"settleCurrency"});
                double value = 1.0, tick = 0.0, lot = 0.0, maxQty = 0.0;
                jf::dbl(o, {"contractSize"}, value);
                jf::dbl(o, {"tickSize"}, tick);
                jf::dbl(o, {"lotSize"}, lot);
                jf::dbl(o, {"maxOrderQty"}, maxQty);

                const bool inverse = !isLinearSettle(settle);
                // ⚠️ V1 的币本位合约，标的币在 `settleCurrency` 里，不在 symbol 里：
                //    BTCUSD -> settle="BTC"，cETHUSD -> settle="ETH"（那个 c 前缀是
                //    "coin-margined"，不是币名的一部分）。拿 symbol 剥 quote 会得到
                //    "cETH" 这种不存在的币，与其它交易所的同名合约对不上（跨所配对直接失效）。
                if (emit(symbol, inverse ? settle : std::string(), quote, settle, value, inverse,
                         [&](md::InstrumentInfo& info) {
                             infohelp::priceFromDouble(info, tick);
                             infohelp::sizeFromDouble(info, lot);
                             // V1 没有 minOrderQty 字段，但一张起订 ⇒ minSize = lotSize。
                             // 留 0 会让下游把它当"无下限"，下单量校验直接失效。
                             if (lot > 0.0) {
                                 info.minSize = lot;
                             }
                             if (maxQty > 0.0) {
                                 info.maxSize = maxQty;
                             }
                         })) {
                    ++keptV1;
                }
            }
        }

        // ---- V2: USDT / USDC 本位 ----
        simdjson::dom::array arr2;
        if (jf::array_at(data, "perpProductsV2", arr2)) {
            for (auto row : arr2) {
                simdjson::dom::object o;
                if (row.get_object().get(o) != simdjson::SUCCESS) {
                    continue;
                }
                if (jf::str_or(o, {"status"}) != "Listed") {
                    ++skipStatus;
                    continue;
                }
                std::string symbol = jf::str_or(o, {"symbol"});
                std::string baseRaw = jf::str_or(o, {"baseCurrency"});
                std::string quote  = jf::str_or(o, {"quoteCurrency"});
                std::string settle = jf::str_or(o, {"settleCurrency"});
                std::string tickS  = jf::str_or(o, {"tickSize"});
                std::string stepS  = jf::str_or(o, {"qtyStepSize"});
                double maxQty = 0.0, minValue = 0.0;
                jf::dbl(o, {"maxOrderQtyRq"}, maxQty);
                jf::dbl(o, {"minOrderValueRv"}, minValue);

                const bool inverse = !isLinearSettle(settle);
                if (emit(symbol, baseRaw, quote, settle, 1.0, inverse,
                         [&](md::InstrumentInfo& info) {
                             infohelp::priceFromDecimal(info, tickS);
                             infohelp::sizeFromDecimal(info, stepS);
                             if (maxQty > 0.0) {
                                 info.maxSize = maxQty;
                             }
                             if (minValue > 0.0) {
                                 info.minAmount = minValue;
                             }
                         })) {
                    ++keptV2;
                }
            }
        }

        LOG_INFO("PhemexInfo -> v1(coin)={} v2(linear)={} skip(status)={} skip(type)={}",
                 keptV1, keptV2, skipStatus, skipType);
        if (keptV1 + keptV2 == 0) {
            LOG_ERROR("PhemexInfo returned 0 instruments. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("PhemexInfo::getSymbolList exception: {}", e.what());
    }
}
