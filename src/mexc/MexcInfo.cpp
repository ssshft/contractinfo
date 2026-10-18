#include "mexc/MexcInfo.h"
#include "base/InfoHelper.h"

#include <simdjson.h>

MexcInfo::MexcInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void MexcInfo::syncExchangeInfo() {
    getContractDetail();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}


// ============================================================================
// GET api.mexc.com/api/v1/contract/detail
//
// response: {"success":true,"code":0,"data":[{...}]}
//   失败时 success=false + message, data 缺失 —— 只看 http 200 会漏掉。
//
// data[i] 字段顺序 (2026-10 实测, 共 80 个字段):
//   symbol, displayName, displayNameEn, positionOpenType, baseCoin, quoteCoin,
//   baseCoinName, quoteCoinName, futureType, settleCoin, contractSize,
//   minLeverage, maxLeverage, countryConfigContractMaxLeverage,
//   priceScale, volScale, amountScale, priceUnit, volUnit, minVol, maxVol,
//   bidLimitPriceRate, askLimitPriceRate, takerFeeRate, makerFeeRate, ...,
//   state, ..., apiAllowed, ...
//
// 语义:
//   contractSize = 1 张合约对应的 base 币数量 (面值/乘数)
//   priceUnit    = 价格最小变动,  priceScale = 其小数位数
//   volUnit      = 数量步长(张),     volScale   = 其小数位数
//   minVol/maxVol= 最小/最大下单张数
//   state        = 0 正常; futureType = 1 永续
//   apiAllowed   = false 的 19 个合约不开放 API, 必须剔掉, 否则下单必然被拒
// ============================================================================
void MexcInfo::getContractDetail() {
    try {
        int status = 0;
        std::string body;
        if (!syncGet("api.mexc.com", "/api/v1/contract/detail", body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("MexcInfo::getContractDetail error! status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("MexcInfo::getContractDetail parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        // 顶层顺序: success -> code -> data
        bool success = false;
        doc["success"].get(success);
        if (!success) {
            int64_t code = 0;
            doc["code"].get(code);
            LOG_ERROR("MexcInfo::getContractDetail api success=false code={}", code);
            return;
        }

        auto data = doc["data"].get_array();
        if (data.error() != simdjson::SUCCESS) {
            LOG_ERROR("MexcInfo::getContractDetail: 'data' missing or not array");
            return;
        }

        int kept = 0;
        int skippedType = 0;
        int skippedState = 0;
        int skippedApi = 0;

        for (auto row : data) {
            auto sym = row.get_object();
            if (sym.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // ---- 严格按 JSON 顺序访问 ----
            std::string_view symbol, baseCoin, quoteCoin, settleCoin;
            double contractSize = 0.0, priceUnit = 0.0, volUnit = 0.0;
            double minVol = 0.0, maxVol = 0.0;
            int64_t futureType = 0, priceScale = 0, volScale = 0, state = 0;
            bool apiAllowed = false;

            sym["symbol"].get(symbol);            // 0
            sym["baseCoin"].get(baseCoin);        // 4
            sym["quoteCoin"].get(quoteCoin);      // 5
            sym["futureType"].get(futureType);    // 8
            sym["settleCoin"].get(settleCoin);    // 9
            sym["contractSize"].get(contractSize);// 10
            sym["priceScale"].get(priceScale);    // 14
            sym["volScale"].get(volScale);        // 15
            sym["priceUnit"].get(priceUnit);      // 17
            sym["volUnit"].get(volUnit);          // 18
            sym["minVol"].get(minVol);            // 19
            sym["maxVol"].get(maxVol);            // 20
            sym["state"].get(state);              // 35
            sym["apiAllowed"].get(apiAllowed);    // 50

            if (futureType != 1) {
                ++skippedType;
                continue;
            }
            if (state != 0) {
                ++skippedState;
                continue;
            }
            if (!apiAllowed) {
                ++skippedApi;
                continue;
            }

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            std::string baseStr(baseCoin);
            getBaseMagnifyNum(baseStr, base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = MEXC;
            crypto::copy_sv_to_char_array(info.originInstId, symbol);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, quoteCoin);
            crypto::copy_sv_to_char_array(info.margin, settleCoin);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.value = contractSize;   // 下单量单位是"张"
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (settleCoin == "USDT") {
                info.instTypeEnum = USDT_SWAP;
                info.calcType = 0;
            }
            else if (settleCoin == "USDC") {
                info.instTypeEnum = USDC_SWAP;
                info.calcType = 0;
            }
            else {
                // 币本位: 保证金 = base 币
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
                crypto::copy_sv_to_char_array(info.margin, info.base);
            }

            // ⚠️ MEXC 的 priceUnit / volUnit **本身就是** tick / 步长,
            //    priceScale / volScale 只是它的小数位数。用 FromFixed 而不是 FromStep
            //    (FromStep 会再除一次 10^n, PEPE 的 tick 会变成 1e-20)。
            infohelp::priceFromFixed(info, priceUnit, static_cast<int>(priceScale));
            infohelp::sizeFromFixed(info, volUnit, static_cast<int>(volScale));

            info.minSize = minVol;
            info.maxSize = maxVol;
            // MEXC 不给最小名义价值; 用 张数 * 面值 * 价格 才能算, 价格这里没有 -> 留 0

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("MexcInfo -> kept={} skip(futureType)={} skip(state)={} skip(apiAllowed=false)={}",
                 kept, skippedType, skippedState, skippedApi);

        if (kept == 0) {
            LOG_ERROR("MexcInfo returned 0 instruments. Response schema may have changed.");
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("MexcInfo::getContractDetail exception: {}", e.what());
    }
}
