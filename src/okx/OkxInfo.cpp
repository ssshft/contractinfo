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

        const bool isSpot = crypto::str_cmp(instType.c_str(), "SPOT");
        const bool isSwap = crypto::str_cmp(instType.c_str(), "SWAP");
        const bool isFutures = crypto::str_cmp(instType.c_str(), "FUTURES");

        for (auto sym_val : data) {
            auto sym = sym_val.get_object();
            if (sym.error() != simdjson::SUCCESS) {
                continue;
            }

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
                sym["baseCcy"].get(baseCcy_sv);
                sym["instId"].get(instId_sv);
                read_inst_id_code(sym.value(), instIdCode);
                sym["lotSz"].get(lotSz_sv);
                sym["maxLmtSz"].get(maxLmtSz_sv);
                sym["minSz"].get(minSz_sv);
                sym["quoteCcy"].get(quoteCcy_sv);
                sym["tickSz"].get(tickSz_sv);
            } else {
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

            info.tickSize = crypto::fast_atod(tickSz_sv);
            int d = crypto::countDecimalDigits(tickSz_sv);
            if (d > 18) {
                d = 18;
            }
            info.priceDigits = d;
            info.pricePow10 = crypto::POW10_INT[d];
            info.tickSizeInt = static_cast<int64_t>(std::llround(info.tickSize * static_cast<double>(info.pricePow10)));
            if (info.tickSizeInt <= 0) {
                info.tickSizeInt = 1;
            }

            info.lotSize = crypto::fast_atod(lotSz_sv);
            int lotD = crypto::countDecimalDigits(lotSz_sv);
            if (lotD > 18) {
                lotD = 18;
            }
            info.sizeDigits = lotD;
            info.sizePow10 = crypto::POW10_INT[lotD];
            info.lotSizeInt = static_cast<int64_t>(std::llround(info.lotSize * static_cast<double>(info.sizePow10)));
            if (info.lotSizeInt <= 0) {
                info.lotSizeInt = 1;
            }

            info.minSize = crypto::fast_atod(minSz_sv);
            info.maxSize = crypto::fast_atod(maxLmtSz_sv);
            info.minAmount = info.minSize;

            std::string base;
            double magnifyNumber = 1.0, reduceNumber = 1.0;

            if (isSpot) {
                info.instTypeEnum = SPOT;
                std::string baseCcy = crypto::to_upper(std::string(baseCcy_sv));
                getBaseMagnifyNum(baseCcy, base, magnifyNumber, reduceNumber);
                std::string quoteCcy = crypto::to_upper(std::string(quoteCcy_sv));
                crypto::copy_sv_to_char_array(info.base, base);
                crypto::copy_sv_to_char_array(info.quote, quoteCcy);
                crypto::copy_sv_to_char_array(info.margin, quoteCcy);
                fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                info.value = 1;
                info.calcType = 0;
            }
            else if (isSwap || isFutures) {
                // OKX 合约 instId 形如 "BTC-USDT-SWAP" / "BTC-USDT-241227", 从中拆出 base/quote
                std::vector<std::string> vv = crypto::split(std::string(instId_sv), "-");
                if (vv.size() < 2) {
                    continue;   // 异常数据跳过
                }
                std::string baseCcy = crypto::to_upper(vv[0]);
                getBaseMagnifyNum(baseCcy, base, magnifyNumber, reduceNumber);
                std::string quoteCcy = crypto::to_upper(vv[1]);
                std::string settle = crypto::to_upper(std::string(settleCcy_sv));

                crypto::copy_sv_to_char_array(info.base, base);
                crypto::copy_sv_to_char_array(info.quote, quoteCcy);
                crypto::copy_sv_to_char_array(info.margin, settle);
                info.value = crypto::fast_atod(ctVal_sv);

                if (isSwap) {
                    fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                    if (ctType_sv == "linear") {
                        info.instTypeEnum = USDT_SWAP;
                        info.calcType = 0;
                    }
                    else if (ctType_sv == "inverse") {
                        info.instTypeEnum = C_SWAP;
                        info.calcType = 1;
                    }
                } else {
                    // FUTURES: instId 里已经带交割日期, 直接沿用作为 instId 更直观
                    crypto::copy_sv_to_char_array(info.instId, instId_sv);
                    if (ctType_sv == "linear") {
                        info.instTypeEnum = USDT_FUTURES;
                        info.calcType = 0;
                    }
                    else if (ctType_sv == "inverse") {
                        info.instTypeEnum = C_FUTURES;
                        info.calcType = 1;
                    }
                }
            }
            info.magnifyNumber = magnifyNumber;
            info.reduceNumber = reduceNumber;

            if (info.pricePow10 <= 0) { 
                info.priceDigits = 0; 
                info.pricePow10 = 1; 
                info.tickSizeInt = 1; 
            }

            if (info.sizePow10 <= 0) { 
                info.sizeDigits = 0; 
                info.sizePow10 = 1; 
                info.lotSizeInt = 1; 
            }

            updateInstrumentInfo(info);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("OkxInfo::getInfo exception: {}", e.what());
    }
}