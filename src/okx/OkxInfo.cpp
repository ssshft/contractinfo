#include "okx/OkxInfo.h"

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
        std::string url = "https://www.okx.com/api/v5/public/instruments?instType=" + instType;
        http_client restClient(url);
        http_request request(methods::GET);

        http_response response = restClient.request(request).get();
        auto code = response.status_code();
        if (code == status_codes::OK) {
            const std::string& v = response.extract_string().get();
            rapidjson::Document d;
            rapidjson::Value& rawData = d.Parse<rapidjson::kParseNumbersAsStringsFlag>(v.c_str());
            if (rawData.HasMember("data")) {
                const rapidjson::Value& symbols = rawData["data"];
                for (rapidjson::SizeType i = 0; i < symbols.Size(); ++i) {
                    md::InstrumentInfo info;
                    memset(&info, 0, sizeof(md::InstrumentInfo));

                    std::string symbol = symbols[i]["instId"].GetString();
                    std::string base = "";
                    double magnifyNumber = 1.0;
                    double reduceNumber = 1.0;
                
                    info.exchangeTypeEnum = OKX;
                    
                    strncpy(info.originInstId, symbol.c_str(), INSTID_SIZE);
                    info.tickSize = std::stod(symbols[i]["tickSz"].GetString());
                    info.lotSize = std::stod(symbols[i]["lotSz"].GetString());
                    info.minSize = std::stod(symbols[i]["minSz"].GetString());
                    info.maxSize = std::stod(symbols[i]["maxLmtSz"].GetString());
                    info.minAmount = info.minSize;

                    if (crypto::str_cmp(instType.c_str(), "SPOT")) {
                        info.instTypeEnum = SPOT;
                        std:;string baseCcy = crypto::to_upper(symbols[i]["baseCcy"].GetString());
                        getBaseMagnifyNum(baseCcy, base, magnifyNumber, reduceNumber);

                        strncpy(info.base, base.c_str(), INSTID_SIZE);
                        strncpy(info.quote, crypto::to_upper(symbols[i]["quoteCcy"].GetString()).c_str(), INSTID_SIZE);
                        strncpy(info.margin, crypto::to_upper(symbols[i]["quoteCcy"].GetString()).c_str(), INSTID_SIZE);
                        fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                        info.value = 1;
                        info.magnifyNumber = magnifyNumber;
                        info.reduceNumber = reduceNumber;
                    }
                    else if (crypto::str_cmp(instType.c_str(), "SWAP")) {
                        std::vector<std::string> v = crypto::split(info.originInstId, "-");
                        std::string baseCcy = crypto::to_upper(v[0]);
                        getBaseMagnifyNum(baseCcy, base, magnifyNumber, reduceNumber);

                        strncpy(info.base, base.c_str(), INSTID_SIZE);
                        strncpy(info.quote, crypto::to_upper(v[1]).c_str(), INSTID_SIZE);
                        strncpy(info.margin, crypto::to_upper(symbols[i]["settleCcy"].GetString()).c_str(), INSTID_SIZE);
                        fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                        info.value = std::stod(symbols[i]["ctVal"].GetString());
                        info.magnifyNumber = magnifyNumber;
                        info.reduceNumber = reduceNumber;

                        std::string ctType = symbols[i]["ctType"].GetString();
                        if (crypto::str_cmp(ctType.c_str(), "linear")) {
                            info.instTypeEnum = USDT_SWAP;
                        }
                        else if (crypto::str_cmp(ctType.c_str(), "inverse")) {
                            info.instTypeEnum = C_SWAP;
                        }
                    }
                    else if (crypto::str_cmp(instType.c_str(), "FUTURES")) {
                        std::vector<std::string> v = crypto::split(info.originInstId, "-");
                        std::string baseCcy = crypto::to_upper(v[0]);
                        getBaseMagnifyNum(baseCcy, base, magnifyNumber, reduceNumber);

                        strncpy(info.base, base.c_str(), INSTID_SIZE);
                        strncpy(info.quote, crypto::to_upper(v[1]).c_str(), INSTID_SIZE);
                        strncpy(info.margin, crypto::to_upper(symbols[i]["settleCcy"].GetString()).c_str(), INSTID_SIZE);
                        strncpy(info.instId, info.originInstId, INSTID_SIZE);
                        info.value = std::stod(symbols[i]["ctVal"].GetString());
                        info.magnifyNumber = magnifyNumber;
                        info.reduceNumber = reduceNumber;

                        std::string ctType = symbols[i]["ctType"].GetString();
                        if (crypto::str_cmp(ctType.c_str(), "linear")) {
                            info.instTypeEnum = USDT_FUTURES;
                        }
                        else if (crypto::str_cmp(ctType.c_str(), "inverse")) {
                            info.instTypeEnum = C_FUTURES;
                        }
                    }

                    updateInstrumentInfo(info);
                }
            }
        }
        else {
            LOG_ERROR("getInfo error! code: {}", code);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("{}", e.what());
    }
}