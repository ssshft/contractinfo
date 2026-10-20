#include "blofin/BlofinInfo.h"
#include "base/InfoHelper.h"
#include "base/JsonField.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace {

// 路径候选。ccxt 里 endpoint 键是 `market/instruments`（无版本前缀），
// 但 BloFin 是 OKX 血统，实际部署常见 `/api/v5/market/instruments`。
// 本地被 451 挡住，没法确认是哪一条，所以两条都试。
const char* kPathCandidates[] = {
    "/market/instruments?instType=SWAP",
    "/api/v5/market/instruments?instType=SWAP",
};

}  // namespace

BlofinInfo::BlofinInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void BlofinInfo::syncExchangeInfo() {
    getSymbolList();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ============================================================================
// GET openapi.blofin.com{/market/instruments | /api/v5/market/instruments}?instType=SWAP
//
// {"code":"0","msg":"","data":[{
//   "instId":"BTC-USDT","instType":"SWAP","baseCurrency":"BTC","quoteCurrency":"USDT",
//   "settleCurrency":"USDT","contractValue":"0.0001","tickSize":"0.1","lotSize":"0.0001",
//   "minSize":"0.0001","maxLimitSize":"1000","contractType":"linear","state":"live",
//   "maxLeverage":"100"}]}
//
// 字段来源 ccxt parseMarket。数值字段 ccxt 里是 safeString/safeNumber 混用，
// 这里统一用 jf::dbl 兼容 "0.0001" 与 0.0001。
//
// 过滤: instType=="SWAP"、state=="live"（suspend/preopen/test 都不要）。
//       两个字段缺失时**不跳过** —— 抓不到响应，宁可多收也不要因为字段名变了全丢。
// ============================================================================
void BlofinInfo::getSymbolList() {
    try {
        std::string body;
        int status = 0;
        bool got = false;

        // 先试上次成功的那条，失败再回退候选表
        if (!path_.empty()) {
            if (syncGet("openapi.blofin.com", path_, body, status) && status == 200 && body.size() >= 200) {
                got = true;
            }
            else {
                LOG_WARN("BlofinInfo: 之前成功的路径 {} 这次失败 (status={}), 回退候选表", path_, status);
                path_.clear();
            }
        }
        if (!got) {
            for (const char* p : kPathCandidates) {
                body.clear();
                status = 0;
                if (!syncGet("openapi.blofin.com", p, body, status)) {
                    continue;
                }
                if (status == 451) {
                    LOG_ERROR("BlofinInfo: {} 返回 451 (地域封锁)，部署机若同区域则无法使用", p);
                    continue;
                }
                if (status != 200 || body.size() < 200) {
                    continue;
                }
                path_ = p;
                got = true;
                break;
            }
        }
        if (!got) {
            LOG_ERROR("BlofinInfo: 所有候选路径都拿不到数据 (451=地域封锁)");
            return;
        }

        simdjson::dom::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.parse(padded);
        if (doc.error()) {
            LOG_ERROR("BlofinInfo::getSymbolList parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        simdjson::dom::object root;
        if (doc.get_object().get(root) != simdjson::SUCCESS) {
            LOG_ERROR("BlofinInfo: response is not an object");
            return;
        }

        std::string code = jf::str_or(root, {"code"});
        if (!code.empty() && code != "0") {
            LOG_ERROR("BlofinInfo: api code={} msg={}", code, jf::str_or(root, {"msg"}));
            return;
        }

        simdjson::dom::array arr;
        if (!jf::array_at(root, "data", arr)) {
            LOG_ERROR("BlofinInfo: 'data' missing or not array (path={})", path_);
            return;
        }

        int kept = 0, skipType = 0, skipState = 0, skipBad = 0;
        for (auto row : arr) {
            simdjson::dom::object o;
            if (row.get_object().get(o) != simdjson::SUCCESS) {
                continue;
            }

            const std::string instType = jf::str_or(o, {"instType"});
            if (!instType.empty() && crypto::to_upper(instType) != "SWAP") {
                ++skipType;
                continue;
            }
            const std::string st = jf::str_or(o, {"state"});
            if (!st.empty() && st != "live") {
                ++skipState;
                continue;
            }

            std::string instId = jf::str_or(o, {"instId"});
            std::string baseRaw = jf::str_or(o, {"baseCurrency"});
            std::string quote   = jf::str_or(o, {"quoteCurrency"});
            std::string settle  = jf::str_or(o, {"settleCurrency"});
            std::string tickS   = jf::str_or(o, {"tickSize"});
            std::string lotS    = jf::str_or(o, {"lotSize"});
            std::string ctype   = jf::str_or(o, {"contractType"});
            if (instId.empty() || baseRaw.empty() || quote.empty()) {
                ++skipBad;
                continue;
            }

            double contractValue = 0.0, minSize = 0.0, maxLimit = 0.0;
            jf::dbl(o, {"contractValue"}, contractValue);
            jf::dbl(o, {"minSize"}, minSize);
            jf::dbl(o, {"maxLimitSize"}, maxLimit);

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseRaw, base, magnifyNumber, reduceNumber);

            std::string quoteUpper = crypto::to_upper(quote);
            std::string settleUpper = settle.empty() ? quoteUpper : crypto::to_upper(settle);
            info.exchangeTypeEnum = BLOFIN;
            crypto::copy_sv_to_char_array(info.originInstId, instId);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteUpper);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.value = (contractValue > 0.0) ? contractValue : 1.0;
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (ctype == "inverse" || settleUpper == "USD" ||
                (settleUpper != "USDT" && settleUpper != "USDC")) {
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
                crypto::copy_sv_to_char_array(info.margin, info.base);
            }
            else {
                info.calcType = 0;
                info.instTypeEnum = (settleUpper == "USDC") ? USDC_SWAP : USDT_SWAP;
                crypto::copy_sv_to_char_array(info.margin, settleUpper);
            }

            infohelp::priceFromDecimal(info, tickS);
            infohelp::sizeFromDecimal(info, lotS);
            if (minSize > 0.0) {
                info.minSize = minSize;
            }
            if (maxLimit > 0.0) {
                info.maxSize = maxLimit;
            }

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("BlofinInfo -> kept={} path={} skip(type)={} skip(state)={} skip(bad)={}",
                 kept, path_, skipType, skipState, skipBad);
        if (kept == 0) {
            LOG_ERROR("BlofinInfo returned 0 instruments (path={}). Schema not verified live - check endpoint/fields.",
                      path_);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("BlofinInfo::getSymbolList exception: {}", e.what());
    }
}
