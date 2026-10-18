#include "htx/HtxInfo.h"
#include "base/InfoHelper.h"

#include <simdjson.h>

HtxInfo::HtxInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void HtxInfo::syncExchangeInfo() {
    // USDT 本位永续
    getSwapInfo("api.hbdm.com", "/linear-swap-api/v1/swap_contract_info", /*isCoinMargined=*/false);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 币本位永续 (只有 5 个合约, 但合约面值语义完全不同, 必须单独走一遍)
    getSwapInfo("api.hbdm.com", "/swap-api/v1/swap_contract_info", /*isCoinMargined=*/true);
    std::this_thread::sleep_for(std::chrono::seconds(1));
}


// ============================================================================
// GET api.hbdm.com/linear-swap-api/v1/swap_contract_info   (USDT 本位)
// GET api.hbdm.com/swap-api/v1/swap_contract_info          (币本位)
//
// response: {"status":"ok","data":[{...}],"ts":...}
//           {"status":"error","err_code":...,"err_msg":"..."}
//
// data[i] 字段顺序 (2026-10 实测):
//   symbol, contract_code, contract_size, price_tick, delivery_date, delivery_time,
//   create_date, contract_status, adjust, price_estimated, settlement_date,
//   support_margin_mode, open_type, settlement_period, labels, tradfi_labels,
//   enable_rpi, business_type, pair, contract_type, trade_partition
//
// ⚠️ 这个接口**不返回** min/max 下单量。HTX 把它放在 per-contract 的
//    /linear-swap-api/v1/swap_order_limit 里 (一个合约一次请求, 387 次), 不适合放进
//    60s 的轮询周期。所以 minSize/maxSize/minAmount 留 0, 并在日志里明确打出来 ——
//    0 不会影响 db 建 unit (generateUnitInfo 只看合约在不在 SecurityManager 里),
//    但真实下单前的量校验需要另外补数据源。
// ============================================================================
void HtxInfo::getSwapInfo(const std::string& host, const std::string& path, bool isCoinMargined) {
    try {
        int status = 0;
        std::string body;
        if (!syncGet(host, path, body, status)) {
            return;
        }

        if (status != 200) {
            LOG_ERROR("HtxInfo::getSwapInfo error! status: {} path: {}", status, path);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("HtxInfo::getSwapInfo parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        // 顶层顺序: status -> data -> ts
        std::string_view topStatus;
        doc["status"].get(topStatus);          // 取不到就保持空, 不当作失败
        if (!topStatus.empty() && topStatus != "ok") {
            std::string_view errMsg;
            doc["err_msg"].get(errMsg);
            LOG_ERROR("HtxInfo::getSwapInfo api status={} msg={} path={}", topStatus, errMsg, path);
            return;
        }

        auto data = doc["data"].get_array();
        if (data.error() != simdjson::SUCCESS) {
            LOG_ERROR("HtxInfo::getSwapInfo: 'data' missing or not array, path: {}", path);
            return;
        }

        int kept = 0;
        int skippedStatus = 0;
        int skippedBadCode = 0;

        for (auto row : data) {
            auto sym = row.get_object();
            if (sym.error() != simdjson::SUCCESS) {
                continue;
            }

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));

            // ---- 严格按 JSON 顺序访问 (contract_code -> contract_size -> price_tick -> contract_status) ----
            // 0 symbol 不读: 它就是 base 币 ("BTC"), 从 contract_code 切更可靠
            std::string_view contractCode;
            double contractSize = 0.0;
            double priceTick = 0.0;
            int64_t contractStatus = 0;

            sym["contract_code"].get(contractCode);
            sym["contract_size"].get(contractSize);
            sym["price_tick"].get(priceTick);
            // contract_status: 1=正常, 3=待下线/交割中 (实测 387 个里有 6 个是 3)
            sym["contract_status"].get(contractStatus);

            if (contractStatus != 1) {
                ++skippedStatus;
                continue;
            }

            // contract_code 形如 "BTC-USDT" / "BTC-USD"
            std::string code(contractCode);
            std::vector<std::string> parts = crypto::split(code, '-');
            if (parts.size() != 2 || parts[0].empty() || parts[1].empty()) {
                ++skippedBadCode;
                continue;
            }

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;
            getBaseMagnifyNum(parts[0], base, magnifyNumber, reduceNumber);

            info.exchangeTypeEnum = HTX;
            crypto::copy_sv_to_char_array(info.originInstId, contractCode);
            crypto::copy_sv_to_char_array(info.base, base);
            crypto::copy_sv_to_char_array(info.quote, parts[1]);
            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);

            info.value = contractSize;
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (isCoinMargined) {
                // 币本位: 保证金就是 base 币本身, 面值是 USD
                info.instTypeEnum = C_SWAP;
                info.calcType = 1;
                crypto::copy_sv_to_char_array(info.margin, info.base);
            }
            else {
                info.instTypeEnum = USDT_SWAP;
                info.calcType = 0;
                crypto::copy_sv_to_char_array(info.margin, info.quote);
            }

            infohelp::priceFromDouble(info, priceTick);

            // HTX 的下单量单位是"张", 且不接受小数张 -> 步长恒为 1 张, 最小 1 张。
            // 1 张 = value 个 base 币 (BTC-USDT 是 0.001 BTC)。
            info.lotSize = 1.0;
            info.sizeDigits = 0;
            info.sizePow10 = 1;
            info.lotSizeInt = 1;
            info.minSize = 1.0;

            // 见函数头注释: 本接口不给下单量上下限
            info.maxSize = 0.0;
            info.minAmount = 0.0;

            infohelp::ensurePrecisionDefaults(info);
            updateInstrumentInfo(info);
            ++kept;
        }

        LOG_INFO("HtxInfo {} -> kept={} skip(contract_status)={} skip(bad_code)={} "
                 "note: minSize/maxSize unavailable from this endpoint",
                 path, kept, skippedStatus, skippedBadCode);

        if (kept == 0) {
            LOG_ERROR("HtxInfo {} returned 0 instruments. Response schema may have changed.", path);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("HtxInfo::getSwapInfo exception: {}", e.what());
    }
}
