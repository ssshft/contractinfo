#include "hotcoin/HotcoinInfo.h"
#include "base/InfoHelper.h"
#include "base/JsonField.h"

#include <chrono>
#include <cstring>
#include <thread>

HotcoinInfo::HotcoinInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void HotcoinInfo::syncExchangeInfo() {
    getSymbolList();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ============================================================================
// GET api-ct.hotcoin.fit/api/v1/perpetual/public
//
// 注意是 `api-ct.hotcoin.fit` 而不是 `api.hotcoinfin.com` —— 后者是现货域名，
// 对任何 /v1/contract/* 路径都返回 {"code":10170,"msg":"API未开放"}。
// 详见头文件。
// ============================================================================
void HotcoinInfo::getSymbolList() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("api-ct.hotcoin.fit", "/api/v1/perpetual/public", body, status)) {
            return;
        }
        if (status != 200) {
            LOG_ERROR("HotcoinInfo::getSymbolList http status {}", status);
            return;
        }

        simdjson::dom::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.parse(padded);
        if (doc.error()) {
            LOG_ERROR("HotcoinInfo::getSymbolList parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        simdjson::dom::object root;
        if (doc.get_object().get(root) != simdjson::SUCCESS) {
            LOG_ERROR("HotcoinInfo: response is not an object");
            return;
        }

        // code 是 int (200 = 成功)，但别只认 200：文档里同一个字段有时写 "200" 有时写 0
        int64_t code = 0;
        if (jf::i64(root, {"code"}, code) && code != 0 && code != 200) {
            LOG_ERROR("HotcoinInfo: api code={} msg={}", code, jf::str_or(root, {"msg"}));
            return;
        }

        simdjson::dom::array arr;
        if (!jf::array_at(root, "data", arr)) {
            LOG_ERROR("HotcoinInfo: 'data' missing or not array");
            return;
        }

        int kept = 0, skipEnv = 0, skipBad = 0;
        for (auto row : arr) {
            simdjson::dom::object o;
            if (row.get_object().get(o) != simdjson::SUCCESS) {
                continue;
            }

            // env: 0 = 正式, 1 = 测试。测试合约不该进行情表。
            int64_t env = 0;
            if (jf::i64(o, {"env"}, env) && env != 0) {
                ++skipEnv;
                continue;
            }

            std::string symbol = jf::str_or(o, {"code"});
            std::string quote  = crypto::to_upper(jf::str_or(o, {"quote"}));
            if (symbol.empty() || quote.empty()) {
                ++skipBad;
                continue;
            }

            // ⚠️ base 是保证金币（线性合约全是 "usdt"），标的是 underlying / indexBase
            std::string baseRaw = jf::str_or(o, {"underlying", "indexBase"});
            if (baseRaw.empty()) {
                // 都没有才退回从 code 里剥掉 quote 后缀（sophusdt - usdt -> soph）
                std::string q = jf::str_or(o, {"quote"});
                if (!q.empty() && symbol.size() > q.size() &&
                    symbol.compare(symbol.size() - q.size(), q.size(), q) == 0) {
                    baseRaw = symbol.substr(0, symbol.size() - q.size());
                }
                else {
                    ++skipBad;
                    continue;
                }
            }

            int64_t direction = 0, priceDigits = 0;
            jf::i64(o, {"direction"}, direction);

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(crypto::to_upper(baseRaw), base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = HOTCOIN;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quote);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            // unitAmount = 一张合约代表多少标的币
            double value = 0.0;
            if (jf::dbl(o, {"unitAmount"}, value) && value > 0.0) {
                info.value = value;
            }
            else {
                info.value = 1.0;
            }
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (direction == 1 || quote == "USD") {
                // 反向合约（币本位）：结算币就是标的本身
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
                crypto::copy_sv_to_char_array(info.margin, info.base);
            }
            else {
                info.calcType = 0;
                info.instTypeEnum = (quote == "USDC") ? USDC_SWAP : USDT_SWAP;
                crypto::copy_sv_to_char_array(info.margin, info.quote);
            }

            // 价格精度：marketPriceDigit（小数位），缺失时退回 minQuoteDigit
            if (jf::i64(o, {"marketPriceDigit"}, priceDigits) ||
                jf::i64(o, {"minQuoteDigit"}, priceDigits)) {
                infohelp::priceFromStep(info, 1.0, static_cast<int>(priceDigits));
            }

            // 数量步长：minTradeUnit 就是张数步长（实测全为 1.0），同时它也就是最小张数。
            // ⚠️ 不要用 minTradeDigit（那是标的币小数位，8）。
            double step = 0.0;
            if (jf::dbl(o, {"minTradeUnit"}, step) && step > 0.0) {
                infohelp::sizeFromFixed(info, step, 0);
                info.minSize = step;
            }

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("HotcoinInfo -> kept={} skip(env)={} skip(bad)={}", kept, skipEnv, skipBad);
        if (kept == 0) {
            LOG_ERROR("HotcoinInfo returned 0 instruments. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("HotcoinInfo::getSymbolList exception: {}", e.what());
    }
}
