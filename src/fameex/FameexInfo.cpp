#include "fameex/FameexInfo.h"
#include "base/InfoHelper.h"
#include "base/JsonField.h"

#include <chrono>
#include <cstring>
#include <thread>

FameexInfo::FameexInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void FameexInfo::syncExchangeInfo() {
    getSymbolList();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

// ============================================================================
// GET https://futuresopenapi.fameex.com/fapi/v1/contracts
//
// [{"symbol":"E-DOGE-USDT","pricePrecision":5,"side":1,"maxMarketVolume":100000,
//   "multiplier":10.0,"minOrderVolume":0,"maxMarketMoney":100000.0,"type":"E",
//   "maxLimitVolume":10000,"maxValidOrder":100,"multiplierCoin":"DOGE",
//   "minOrderMoney":10.0,"maxLimitMoney":10000.0,"contractId":150,"status":1}]
//
// 语义:
//   multiplier      = 面值（1 张 = multiplier 个 multiplierCoin）
//   pricePrecision  = 价格小数位
//   maxLimitVolume  = 限价单最大张数；maxMarketVolume = 市价单最大张数
//   minOrderMoney   = 最小下单金额（USDT），minOrderVolume 实测全是 0，不可用
//   status          = 1 正常 / 0 不可交易（255 条里 35 条是 0）
// ============================================================================
void FameexInfo::getSymbolList() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("futuresopenapi.fameex.com", "/fapi/v1/contracts", body, status)) {
            return;
        }
        if (status != 200) {
            LOG_ERROR("FameexInfo::getSymbolList http status {}", status);
            return;
        }

        simdjson::dom::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.parse(padded);
        if (doc.error()) {
            LOG_ERROR("FameexInfo::getSymbolList parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        simdjson::dom::array arr;
        if (doc.get_array().get(arr) != simdjson::SUCCESS) {
            LOG_ERROR("FameexInfo: top-level is not an array (合约接口顶层直接是数组)");
            return;
        }

        int kept = 0, skipStatus = 0, skipBad = 0;
        for (auto row : arr) {
            simdjson::dom::object o;
            if (row.get_object().get(o) != simdjson::SUCCESS) {
                continue;
            }

            int64_t st = 1;
            if (jf::i64(o, {"status"}, st) && st != 1) {
                ++skipStatus;
                continue;
            }

            std::string symbol = jf::str_or(o, {"symbol"});
            // "E-BTC-USDT" -> ["E","BTC","USDT"]
            const size_t p1 = symbol.find('-');
            const size_t p2 = (p1 == std::string::npos) ? std::string::npos : symbol.find('-', p1 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos) {
                ++skipBad;
                continue;
            }
            std::string baseRaw = symbol.substr(p1 + 1, p2 - p1 - 1);
            std::string quote   = symbol.substr(p2 + 1);
            if (baseRaw.empty() || quote.empty()) {
                ++skipBad;
                continue;
            }

            int64_t pricePrec = 0;
            jf::i64(o, {"pricePrecision"}, pricePrec);
            double multiplier = 0.0, minVol = 0.0, maxLimit = 0.0, minMoney = 0.0;
            jf::dbl(o, {"multiplier"}, multiplier);
            jf::dbl(o, {"minOrderVolume"}, minVol);
            jf::dbl(o, {"maxLimitVolume"}, maxLimit);
            jf::dbl(o, {"minOrderMoney"}, minMoney);

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(baseRaw, base, magnifyNumber, reduceNumber);

            std::string quoteUpper = crypto::to_upper(quote);
            info.exchangeTypeEnum = FAMEEX;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteUpper);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.value = (multiplier > 0.0) ? multiplier : 1.0;
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            info.calcType = 0;
            info.instTypeEnum = (quoteUpper == "USDC") ? USDC_SWAP : USDT_SWAP;
            crypto::copy_sv_to_char_array(info.margin, info.quote);

            infohelp::priceFromStep(info, 1.0, static_cast<int>(pricePrec));
            // 没有数量精度字段: 单位是"张", 步长 1
            infohelp::sizeFromFixed(info, 1.0, 0);

            if (minVol > 0.0) {
                info.minSize = minVol;
            }
            if (maxLimit > 0.0) {
                info.maxSize = maxLimit;
            }
            if (minMoney > 0.0) {
                info.minAmount = minMoney;
            }

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("FameexInfo -> kept={} skip(status)={} skip(bad)={}", kept, skipStatus, skipBad);
        if (kept == 0) {
            LOG_ERROR("FameexInfo returned 0 instruments. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("FameexInfo::getSymbolList exception: {}", e.what());
    }
}
