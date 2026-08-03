#include "okx/OkxInfo.h"
#include <simdjson.h>


OkxInfo::OkxInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {
}

void OkxInfo::syncExchangeInfo() {
    std::vector<std::string> vInstType = {"SPOT", "SWAP", "FUTURES"};
    for (size_t i = 0; i < vInstType.size(); ++i) {
        getInfo(vInstType[i]);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}


// ============================================================================
// OKX GET /api/v5/public/instruments?instType=...
//   response: {"code":"0", "msg":"", "data":[{...},...]}
//
// data[i] 每条 instrument 的字段顺序是**字母序** (实测):
//   auctionEndTime, baseCcy, ctMult, ctType, ctVal, ctValCcy, contTdSwTime,
//   elp, expTime, ..., instId, instType, ..., listTime, lotSz, maxIcebergSz,
//   maxLmtAmt, maxLmtSz, maxMktAmt, maxMktSz, ..., minSz, ...,
//   quoteCcy, ..., settleCcy, ..., tickSz, uly, instIdCode
//
// ⚠️ 因此 simdjson ondemand 访问顺序必须按字母序:
//   baseCcy → ctType → ctVal → instId → lotSz → maxLmtSz → minSz → quoteCcy → settleCcy → tickSz
// ============================================================================
void OkxInfo::getInfo(const std::string& instType) {
    try {
        std::string path = "/api/v5/public/instruments?instType=" + instType;
        int status = 0;
        std::string body;
        if (!syncGet("www.okx.com", path, body, status)) return;
        if (status != 200) {
            LOG_ERROR("OkxInfo::getInfo error! status: {}", status);
            return;
        }

        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(body);
        auto doc = parser.iterate(padded);
        if (doc.error()) {
            LOG_ERROR("OkxInfo::getInfo parse error: {}", simdjson::error_message(doc.error()));
            return;
        }

        // 顶层字段顺序: code, msg, data (按 JSON 顺序访问, 我们只需要 data)
        auto data = doc["data"].get_array();
        if (data.error() != simdjson::SUCCESS) {
            LOG_ERROR("OkxInfo::getInfo: 'data' missing or not array");
            return;
        }

        const bool isSpot    = crypto::str_cmp(instType.c_str(), "SPOT");
        const bool isSwap    = crypto::str_cmp(instType.c_str(), "SWAP");
        const bool isFutures = crypto::str_cmp(instType.c_str(), "FUTURES");

        for (auto sym_val : data) {
            auto sym = sym_val.get_object();
            if (sym.error() != simdjson::SUCCESS) continue;

            md::InstrumentInfo info;
            memset(&info, 0, sizeof(md::InstrumentInfo));
            info.exchangeTypeEnum = OKX;

            // ---- 按字母序读所需字段 ----
            std::string_view baseCcy_sv, ctType_sv, ctVal_sv;
            std::string_view instId_sv, lotSz_sv, maxLmtSz_sv, minSz_sv;
            std::string_view quoteCcy_sv, settleCcy_sv, tickSz_sv;
            int64_t instIdCode = 0;

            auto read_inst_id_code = [](simdjson::ondemand::object& obj, int64_t& out) {
                auto f = obj["instIdCode"];
                if (f.error() != simdjson::SUCCESS) {
                    return;
                }

                bool is_null = false;
                if (f.is_null().get(is_null) == simdjson::SUCCESS && is_null) {
                    return;
                }

                (void)f.get(out);
            };

            if (isSpot) {
                // SPOT: baseCcy → instId → lotSz → maxLmtSz → minSz → quoteCcy → tickSz
                sym["baseCcy"].get(baseCcy_sv);
                sym["instId"].get(instId_sv);
                read_inst_id_code(sym.value(), instIdCode);
                sym["lotSz"].get(lotSz_sv);
                sym["maxLmtSz"].get(maxLmtSz_sv);
                sym["minSz"].get(minSz_sv);
                sym["quoteCcy"].get(quoteCcy_sv);
                sym["tickSz"].get(tickSz_sv);
            } else {
                // SWAP / FUTURES: ctType → ctVal → instId → lotSz → maxLmtSz → minSz → settleCcy → tickSz
                sym["ctType"].get(ctType_sv);
                sym["ctVal"].get(ctVal_sv);
                sym["instId"].get(instId_sv);
                read_inst_id_code(sym.value(), instIdCode);
                sym["lotSz"].get(lotSz_sv);
                sym["maxLmtSz"].get(maxLmtSz_sv);
                sym["minSz"].get(minSz_sv);
                sym["settleCcy"].get(settleCcy_sv);
                sym["tickSz"].get(tickSz_sv);
            }

            info.instIdCode = instIdCode;

            crypto::copy_sv_to_char_array(info.originInstId, instId_sv);
            info.tickSize  = crypto::fast_atod(tickSz_sv);
            info.lotSize   = crypto::fast_atod(lotSz_sv);
            info.minSize   = crypto::fast_atod(minSz_sv);
            info.maxSize   = crypto::fast_atod(maxLmtSz_sv);
            info.minAmount = info.minSize;

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;

            if (isSpot) {
                info.instTypeEnum = SPOT;
                std::string baseCcy = crypto::to_upper(std::string(baseCcy_sv));
                getBaseMagnifyNum(baseCcy, base, magnifyNumber, reduceNumber);
                std::string quoteCcy = crypto::to_upper(std::string(quoteCcy_sv));
                crypto::copy_sv_to_char_array(info.base,   base);
                crypto::copy_sv_to_char_array(info.quote,  quoteCcy);
                crypto::copy_sv_to_char_array(info.margin, quoteCcy);
                fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                info.value = 1;
            }
            else if (isSwap || isFutures) {
                // OKX 合约 instId 形如 "BTC-USDT-SWAP" / "BTC-USDT-241227", 从中拆出 base/quote
                std::vector<std::string> vv = crypto::split(std::string(instId_sv), "-");
                if (vv.size() < 2) continue;   // 异常数据跳过
                std::string baseCcy = crypto::to_upper(vv[0]);
                getBaseMagnifyNum(baseCcy, base, magnifyNumber, reduceNumber);
                std::string quoteCcy = crypto::to_upper(vv[1]);
                std::string settle   = crypto::to_upper(std::string(settleCcy_sv));

                crypto::copy_sv_to_char_array(info.base,   base);
                crypto::copy_sv_to_char_array(info.quote,  quoteCcy);
                crypto::copy_sv_to_char_array(info.margin, settle);
                info.value = crypto::fast_atod(ctVal_sv);

                if (isSwap) {
                    fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                    if (ctType_sv == "linear")  info.instTypeEnum = USDT_SWAP;
                    else if(ctType_sv == "inverse") info.instTypeEnum = C_SWAP;
                } else {
                    // FUTURES: instId 里已经带交割日期, 直接沿用作为 instId 更直观
                    crypto::copy_sv_to_char_array(info.instId, instId_sv);
                    if (ctType_sv == "linear")  info.instTypeEnum = USDT_FUTURES;
                    else if (ctType_sv == "inverse") info.instTypeEnum = C_FUTURES;
                }
            }
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber  = reduceNumber;

            updateInstrumentInfo(info);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("OkxInfo::getInfo exception: {}", e.what());
    }
}