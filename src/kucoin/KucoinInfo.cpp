#include "kucoin/KucoinInfo.h"
#include "base/InfoHelper.h"

#include <simdjson.h>

namespace {

// KuCoin 沿用了 BitMEX/老交易所的 XBT 写法。目前只有 BTC 一个别名,
// 但写成函数而不是 if 是为了以后出现别的别名时有地方加。
inline std::string normalizeKucoinBase(std::string_view base) {
    if (base == "XBT") {
        return "BTC";
    }
    return std::string(base);
}

} // anonymous namespace


KucoinInfo::KucoinInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void KucoinInfo::syncExchangeInfo() {
    getActiveContracts();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}


// ============================================================================
// GET api-futures.kucoin.com/api/v1/contracts/active
//
// response: {"code":"200000","data":[{...}]}   code != "200000" 即失败
//
// data[i] 字段顺序 (2026-10 实测, 共 86 个字段):
//   symbol, displaySymbol, rootSymbol, type, firstOpenDate, expireDate, settleDate,
//   baseCurrency, displayBaseCurrency, quoteCurrency, settleCurrency,
//   maxOrderQty, marketMaxOrderQty, maxPrice, lotSize, tickSize, indexPriceTickSize,
//   multiplier, initialMargin, maintainMargin, ..., status, ...
//
// 语义:
//   symbol      = "XBTUSDTM" 这种拼接串, 不要拿来切 base/quote (M 后缀会被当成币种)
//   multiplier  = 1 张合约的面值; **反向合约为 -1**, 表示 1 张 = 1 USD
//   lotSize     = 数量步长(张),  tickSize = 价格步长
//   expireDate  = 永续为 null, 交割/季度合约才是时间戳
//   本接口**不给 minOrderQty**, 只有 maxOrderQty
//
// 过滤:
//   - 只要永续 (expireDate 为 null)。季度合约的 instId 需要加到期后缀, 而 KuCoin 的
//     symbol 里那段 (XBTMZ26) 不是标准的 YYMMDD, 猜错会造出永远对不上的 instId,
//     还会和同币种的永续撞键 (都落到 "XBT-USD")。本工程只做永续, 直接跳过并计数。
//   - status 必须 "Open"
// ============================================================================
void KucoinInfo::getActiveContracts() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("api-futures.kucoin.com", "/api/v1/contracts/active", body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("KucoinInfo::getActiveContracts error! status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("KucoinInfo::getActiveContracts parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        // 顶层顺序: code -> data
        std::string_view code;
        doc["code"].get(code);
        if (!code.empty() && code != "200000") {
            LOG_ERROR("KucoinInfo::getActiveContracts api code={}", code);
            return;
        }

        auto data = doc["data"].get_array();
        if (data.error() != simdjson::SUCCESS) {
            LOG_ERROR("KucoinInfo::getActiveContracts: 'data' missing or not array");
            return;
        }

        int kept = 0;
        int skippedDated = 0;
        int skippedStatus = 0;

        for (auto row : data) {
            auto sym = row.get_object();
            if (sym.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // ---- 严格按 JSON 顺序访问 ----
            std::string_view symbol, baseCurrency, quoteCurrency, settleCurrency, statusStr;
            int64_t expireDate = 0;
            double maxOrderQty = 0.0, lotSize = 0.0, tickSize = 0.0, multiplier = 0.0;

            sym["symbol"].get(symbol);                  // 0
            // 5: 永续为 null, get() 失败即可, expireDate 保持 0
            sym["expireDate"].get(expireDate);
            sym["baseCurrency"].get(baseCurrency);      // 7
            sym["quoteCurrency"].get(quoteCurrency);    // 9
            sym["settleCurrency"].get(settleCurrency);  // 10
            sym["maxOrderQty"].get(maxOrderQty);        // 11
            sym["lotSize"].get(lotSize);                // 14
            sym["tickSize"].get(tickSize);              // 15
            sym["multiplier"].get(multiplier);          // 17
            sym["status"].get(statusStr);               // 38

            // 只要永续。expireDate 对永续是 null, get() 失败后保持 0;
            // 再用 symbol 末位兜底一次 —— KuCoin 永续都以 M 结尾 (XBTUSDTM/ETHUSDM/XBTUSDCM),
            // 季度合约以年份数字结尾 (XBTMZ26)。实测 702 个里只有 1 个季度合约, 两条判据一致。
            // 双重判据是为了不依赖 "get() 失败后 value 是否已被消费" 这一 ondemand 细节。
            bool isDated = (expireDate != 0);
            if (!isDated && !symbol.empty()) {
                const char last = symbol.back();
                if (last >= '0' && last <= '9') {
                    isDated = true;
                }
            }
            if (isDated) {
                ++skippedDated;
                continue;
            }
            if (statusStr != "Open") {
                ++skippedStatus;
                continue;
            }

            std::string baseRaw = normalizeKucoinBase(baseCurrency);
            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseRaw, base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = KUCOIN;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteCurrency);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            const bool isInverse = (quoteCurrency == "USD");   // string_view 直接比, str_cmp 不接受 string_view
            if (isInverse) {
                // 反向合约: 1 张 = 1 USD, 保证金是 base 币
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
                info.value = 1.0;
                crypto::copy_sv_to_char_array(info.margin, info.base);
            }
            else {
                info.value = (multiplier > 0.0) ? multiplier : 1.0;
                info.calcType = 0;
                crypto::copy_sv_to_char_array(info.margin, settleCurrency);
                if (settleCurrency == "USDC") {
                    info.instTypeEnum = USDC_SWAP;
                }
                else {
                    info.instTypeEnum = USDT_SWAP;
                }
            }

            infohelp::priceFromDouble(info, tickSize);
            infohelp::sizeFromDouble(info, lotSize);

            // KuCoin 只给 maxOrderQty, 不给最小下单量。最小单位就是 1 张 (lotSize),
            // 用它兜底比留 0 安全 —— 0 会被下游当成"无下限"。
            info.minSize = (lotSize > 0.0) ? lotSize : 1.0;
            info.maxSize = maxOrderQty;

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("KucoinInfo -> kept={} skip(dated)={} skip(status)={}", kept, skippedDated, skippedStatus);

        if (kept == 0) {
            LOG_ERROR("KucoinInfo returned 0 instruments. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("KucoinInfo::getActiveContracts exception: {}", e.what());
    }
}
