#include "echobit/EchobitInfo.h"
#include "base/InfoHelper.h"
#include "base/JsonField.h"

#include <chrono>
#include <cstring>
#include <thread>

EchobitInfo::EchobitInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void EchobitInfo::syncExchangeInfo() {
    getSymbolList();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ============================================================================
// GET uapi.echobit.com/uapi/contract/list
//
// 域名的 "u" 是 uapi，不是 api —— api.echobit.com 不可达。详见头文件。
// ============================================================================
void EchobitInfo::getSymbolList() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("uapi.echobit.com", "/uapi/contract/list", body, status)) {
            return;
        }
        if (status != 200) {
            LOG_ERROR("EchobitInfo::getSymbolList http status {}", status);
            return;
        }

        simdjson::dom::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.parse(padded);
        if (doc.error()) {
            LOG_ERROR("EchobitInfo::getSymbolList parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        simdjson::dom::object root;
        if (doc.get_object().get(root) != simdjson::SUCCESS) {
            LOG_ERROR("EchobitInfo: response is not an object");
            return;
        }

        // ⚠️ 这里是字符串 "0"，不是数字 0。jf::i64 两种都吃。
        int64_t code = 0;
        if (jf::i64(root, {"code"}, code) && code != 0) {
            LOG_ERROR("EchobitInfo: api code={} msg={}", code, jf::str_or(root, {"msg"}));
            return;
        }

        simdjson::dom::array arr;
        if (!jf::array_at(root, "data", arr)) {
            LOG_ERROR("EchobitInfo: 'data' missing or not array");
            return;
        }

        int kept = 0, skipType = 0, skipState = 0, skipSim = 0, skipBad = 0;
        for (auto row : arr) {
            simdjson::dom::object o;
            if (row.get_object().get(o) != simdjson::SUCCESS) {
                continue;
            }

            // type: 1 = 现货, 4 = 合约
            int64_t type = 4;
            if (jf::i64(o, {"type"}, type) && type != 4) {
                ++skipType;
                continue;
            }
            // baseId: 1 = LIVE_USDT, 2 = CONTRACT_SIMULATION
            int64_t baseId = 1;
            if (jf::i64(o, {"baseId"}, baseId) && baseId != 1) {
                ++skipSim;
                continue;
            }
            // showState: 是否对外展示（false = 已下架/未上线）
            bool show = true;
            if (jf::boolean(o, {"showState"}, show) && !show) {
                ++skipState;
                continue;
            }

            std::string symbol = jf::str_or(o, {"symbolId", "tokenId"});
            std::string baseRaw = jf::str_or(o, {"baseTokenId"});
            std::string quote   = crypto::to_upper(jf::str_or(o, {"quoteTokenId"}));
            if (symbol.empty() || baseRaw.empty() || quote.empty()) {
                ++skipBad;
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(crypto::to_upper(baseRaw), base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = ECHOBIT;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quote);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            // multiplier = 一张合约代表多少标的币
            double value = 0.0;
            if (jf::dbl(o, {"multiplier"}, value) && value > 0.0) {
                info.value = value;
            }
            else {
                info.value = 1.0;
            }
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (quote == "USD") {
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
                crypto::copy_sv_to_char_array(info.margin, info.base);
            }
            else {
                info.calcType = 0;
                info.instTypeEnum = (quote == "USDC") ? USDC_SWAP : USDT_SWAP;
                crypto::copy_sv_to_char_array(info.margin, info.quote);
            }

            // 价格 tick = minPricePrecision（**不是** quoteTokenPrecision）
            std::string tick = jf::str_or(o, {"minPricePrecision"});
            if (!tick.empty()) {
                infohelp::priceFromDecimal(info, tick);
            }

            // 张数步长 = baseTokenPrecision（步长形式，不是小数位）
            std::string step = jf::str_or(o, {"baseTokenPrecision"});
            if (!step.empty()) {
                infohelp::sizeFromDecimal(info, step);
            }

            double v = 0.0;
            if (jf::dbl(o, {"minTradeQuantity"}, v) && v > 0.0) {
                info.minSize = v;
            }
            if (jf::dbl(o, {"minTradeAmount"}, v) && v > 0.0) {
                info.minAmount = v;
            }

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("EchobitInfo -> kept={} skip(type)={} skip(sim)={} skip(state)={} skip(bad)={}",
                 kept, skipType, skipSim, skipState, skipBad);
        if (kept == 0) {
            LOG_ERROR("EchobitInfo returned 0 instruments. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("EchobitInfo::getSymbolList exception: {}", e.what());
    }
}
