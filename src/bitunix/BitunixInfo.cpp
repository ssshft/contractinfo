#include "bitunix/BitunixInfo.h"
#include "base/InfoHelper.h"
#include "base/JsonField.h"

#include <chrono>
#include <cstring>
#include <thread>

BitunixInfo::BitunixInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void BitunixInfo::syncExchangeInfo() {
    getSymbolList();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ============================================================================
// GET fapi.bitunix.com/api/v1/futures/market/trading_pairs
//
// {"code":0,"data":[{"symbol":"BTCUSDT","base":"BTC","quote":"USDT",
//   "minTradeVolume":"0.0001","minBuyPriceOffset":"-0.5","maxSellPriceOffset":"100",
//   "maxLimitOrderVolume":"1200","maxMarketOrderVolume":"120","basePrecision":4,
//   "quotePrecision":1,"maxLeverage":200,...,"symbolStatus":"OPEN",
//   "isApiSupported":true,...}],"msg":""}
//
// 语义 (实测 2026-10-20, 781 条):
//   basePrecision  = **数量**小数位  (DOGEUSDT=0 -> 步长 1 张, minTradeVolume=53)
//   quotePrecision = **价格**小数位  (BTCUSDT=1 -> tick 0.1)
//   minTradeVolume / maxLimitOrderVolume 的下单量单位 = 标的本身, 所以面值取 1
//   quote 有三种: USDT / USDC / USD(币本位, 结算币 = base)
//   过滤: symbolStatus=="OPEN" (PREVIEW 是尚未开放) 且 isApiSupported==true
// ============================================================================
void BitunixInfo::getSymbolList() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("fapi.bitunix.com", "/api/v1/futures/market/trading_pairs", body, status)) {
            return;
        }
        if (status != 200) {
            LOG_ERROR("BitunixInfo::getSymbolList http status {}", status);
            return;
        }

        simdjson::dom::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.parse(padded);
        if (doc.error()) {
            LOG_ERROR("BitunixInfo::getSymbolList parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        simdjson::dom::object root;
        if (doc.get_object().get(root) != simdjson::SUCCESS) {
            LOG_ERROR("BitunixInfo: response is not an object");
            return;
        }

        int64_t code = 0;
        if (jf::i64(root, {"code"}, code) && code != 0) {
            LOG_ERROR("BitunixInfo: api code={} msg={}", code, jf::str_or(root, {"msg"}));
            return;
        }

        simdjson::dom::array arr;
        if (!jf::array_at(root, "data", arr)) {
            LOG_ERROR("BitunixInfo: 'data' missing or not array");
            return;
        }

        int kept = 0, skipStatus = 0, skipApi = 0, skipBad = 0;
        for (auto row : arr) {
            simdjson::dom::object o;
            if (row.get_object().get(o) != simdjson::SUCCESS) {
                continue;
            }

            std::string symbol  = jf::str_or(o, {"symbol"});
            std::string baseRaw = jf::str_or(o, {"base"});
            std::string quote   = jf::str_or(o, {"quote"});
            if (symbol.empty() || baseRaw.empty() || quote.empty()) {
                ++skipBad;
                continue;
            }

            std::string st = jf::str_or(o, {"symbolStatus"});
            if (!st.empty() && st != "OPEN") {
                ++skipStatus;
                continue;
            }
            bool apiOk = true;
            if (jf::boolean(o, {"isApiSupported"}, apiOk) && !apiOk) {
                ++skipApi;
                continue;
            }

            int64_t qtyPrec = 0, pricePrec = 0;
            jf::i64(o, {"basePrecision"}, qtyPrec);    // 数量小数位
            jf::i64(o, {"quotePrecision"}, pricePrec); // 价格小数位

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseRaw, base, magnifyNumber, reduceNumber);

            std::string quoteUpper = crypto::to_upper(quote);
            info.exchangeTypeEnum = BITUNIX;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteUpper);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.value = 1.0;   // 下单量单位就是标的本身
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (quoteUpper == "USD") {
                // 币本位: 结算币是 base
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
                crypto::copy_sv_to_char_array(info.margin, info.base);
            }
            else {
                info.calcType = 0;
                info.instTypeEnum = (quoteUpper == "USDC") ? USDC_SWAP : USDT_SWAP;
                crypto::copy_sv_to_char_array(info.margin, info.quote);
            }

            infohelp::priceFromStep(info, 1.0, static_cast<int>(pricePrec));
            infohelp::sizeFromStep(info, 1.0, static_cast<int>(qtyPrec));

            double v = 0.0;
            if (jf::dbl(o, {"minTradeVolume"}, v) && v > 0.0) {
                info.minSize = v;
            }
            if (jf::dbl(o, {"maxLimitOrderVolume"}, v) && v > 0.0) {
                info.maxSize = v;
            }

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("BitunixInfo -> kept={} skip(status)={} skip(api)={} skip(bad)={}",
                 kept, skipStatus, skipApi, skipBad);
        if (kept == 0) {
            LOG_ERROR("BitunixInfo returned 0 instruments. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("BitunixInfo::getSymbolList exception: {}", e.what());
    }
}
