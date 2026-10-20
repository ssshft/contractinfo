#include "weex/WeexInfo.h"
#include "base/InfoHelper.h"
#include "base/JsonField.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

WeexInfo::WeexInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void WeexInfo::syncExchangeInfo() {
    getSymbolList();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ============================================================================
// GET https://api-contract.weex.com/capi/v3/market/exchangeInfo
//
// {"assets":[{"asset":"USDT","marginAvailable":true}],
//  "rateLimits":[...],
//  "symbols":[{"symbol":"BTCUSDT","displaySymbol":"BTCUSDT","baseAsset":"BTC",
//              "quoteAsset":"USDT","marginAsset":"USDT","contractType":"PERPETUAL",
//              "underlyingType":"COIN","underlyingSubType":["POW"],
//              "pricePrecision":1,"quantityPrecision":4,"baseAssetPrecision":3,
//              "quotePrecision":8,"contractVal":0.0001,
//              "minOrderSize":0.0001,"maxOrderSize":2000, ...}]}
//
// 下单量单位判定（这是本文件最容易搞错的一处）:
//   DOGEUSDT  contractVal=100  minOrderSize=100  quantityPrecision=-2
//   BTCUSDT   contractVal=0.0001 minOrderSize=0.0001 quantityPrecision=4
//   若把 minOrderSize 当"张"，BTC 的最小下单 = 0.0001 张 × 0.0001 BTC = 1e-8 BTC
//   （约 0.001 USDT），不可能。若当"币"，0.0001 BTC ≈ 10 USDT、100 DOGE ≈ 9 USDT，
//   两个都落在各家所常见的最小名义 5~10 USDT 上 ⇒ **下单量单位是币，面值取 1**。
//   contractVal 在这套接口里与 minOrderSize 高度重合（1039 条里 1037 条相等），
//   当成"合约乘数"用会差 100 倍，不要用。
// ============================================================================
void WeexInfo::getSymbolList() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("api-contract.weex.com", "/capi/v3/market/exchangeInfo", body, status)) {
            return;
        }
        if (status != 200) {
            LOG_ERROR("WeexInfo::getSymbolList http status {}", status);
            return;
        }

        simdjson::dom::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.parse(padded);
        if (doc.error()) {
            LOG_ERROR("WeexInfo::getSymbolList parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        simdjson::dom::object root;
        if (doc.get_object().get(root) != simdjson::SUCCESS) {
            LOG_ERROR("WeexInfo: response is not an object");
            return;
        }

        simdjson::dom::array arr;
        if (!jf::array_at(root, "symbols", arr)) {
            LOG_ERROR("WeexInfo: 'symbols' missing or not array");
            return;
        }

        int kept = 0, skipType = 0, skipBad = 0;
        for (auto row : arr) {
            simdjson::dom::object o;
            if (row.get_object().get(o) != simdjson::SUCCESS) {
                continue;
            }

            if (jf::str_or(o, {"contractType"}) != "PERPETUAL") {
                ++skipType;
                continue;
            }

            std::string symbol  = jf::str_or(o, {"symbol"});
            std::string baseRaw = jf::str_or(o, {"baseAsset"});
            std::string quote   = jf::str_or(o, {"quoteAsset"});
            std::string margin  = jf::str_or(o, {"marginAsset"});
            if (symbol.empty() || baseRaw.empty() || quote.empty()) {
                ++skipBad;
                continue;
            }

            int64_t pricePrec = 0, qtyPrec = 0;
            jf::i64(o, {"pricePrecision"}, pricePrec);
            jf::i64(o, {"quantityPrecision"}, qtyPrec);

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseRaw, base, magnifyNumber, reduceNumber);

            std::string quoteUpper = crypto::to_upper(quote);
            info.exchangeTypeEnum = WEEX;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteUpper);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.value = 1.0;   // 下单量单位是币，见文件头注释
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            const std::string settle = margin.empty() ? quoteUpper : crypto::to_upper(margin);
            if (settle == "USDT" || settle == "USDC" || settle == "USD") {
                info.calcType = 0;
                info.instTypeEnum = (settle == "USDC") ? USDC_SWAP : USDT_SWAP;
                crypto::copy_sv_to_char_array(info.margin, settle);
            }
            else {
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
                crypto::copy_sv_to_char_array(info.margin, info.base);
            }

            infohelp::priceFromStep(info, 1.0, static_cast<int>(pricePrec));
            if (qtyPrec >= 0) {
                infohelp::sizeFromStep(info, 1.0, static_cast<int>(qtyPrec));
            }
            else {
                // quantityPrecision 为负: 步长 = 10^(-qtyPrec)。sizeFromStep 会把负值钳成 0,
                // 必须显式算出来再走 sizeFromFixed。
                const double step = std::pow(10.0, static_cast<double>(-qtyPrec));
                infohelp::sizeFromFixed(info, step, 0);
            }

            double v = 0.0;
            if (jf::dbl(o, {"minOrderSize"}, v) && v > 0.0) {
                info.minSize = v;
            }
            if (jf::dbl(o, {"maxOrderSize"}, v) && v > 0.0) {
                info.maxSize = v;
            }

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("WeexInfo -> kept={} skip(type)={} skip(bad)={}", kept, skipType, skipBad);
        if (kept == 0) {
            LOG_ERROR("WeexInfo returned 0 instruments. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("WeexInfo::getSymbolList exception: {}", e.what());
    }
}
