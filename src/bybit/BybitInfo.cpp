#include "bybit/BybitInfo.h"

BybitInfo::BybitInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {

}

void BybitInfo::syncExchangeInfo() {
    std::vector<std::string> vInstType = {"spot", "linear", "inverse"};
    for (size_t i = 0; i < vInstType.size(); ++i) {
        getInfo(vInstType[i]);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void BybitInfo::getInfo(const std::string& instType) {
    try {
        std::string url = "https://api.bybit.com/v5/market/instruments-info?category=" + instType + "&limit=1000";
        http_client restClient(url);
        http_request request(methods::GET);

        http_response response = restClient.request(request).get();
        auto code = response.status_code();
        if (code == status_codes::OK) {
            const std::string& v = response.extract_string().get();
            rapidjson::Document d;
            rapidjson::Value& rawData = d.Parse<rapidjson::kParseNumbersAsStringsFlag>(v.c_str());
            if (rawData.HasMember("result") && rawData["result"].HasMember("list")) {
                const rapidjson::Value& symbols = rawData["result"]["list"];
                for (rapidjson::SizeType i = 0; i < symbols.Size(); ++i) {
                    md::InstrumentInfo info;
                    memset(&info, 0, sizeof(md::InstrumentInfo));

                    std::string symbol = symbols[i]["symbol"].GetString();
                    std::string base = "";
                    double magnifyNumber = 1.0;
                    double reduceNumber = 1.0;
                    std:;string baseCoin = crypto::to_upper(symbols[i]["baseCoin"].GetString());
                    getBaseMagnifyNum(baseCoin, base, magnifyNumber, reduceNumber);

                    info.exchangeTypeEnum = BYBIT;
                    
                    strncpy(info.originInstId, symbol.c_str(), INSTID_SIZE);
                    strncpy(info.base, base.c_str(), INSTID_SIZE);
                    strncpy(info.quote, crypto::to_upper(symbols[i]["quoteCoin"].GetString()).c_str(), INSTID_SIZE);
                    info.value = 1;
                    info.magnifyNumber = magnifyNumber;
                    info.reduceNumber = reduceNumber;
                    info.tickSize = std::stod(symbols[i]["priceFilter"]["tickSize"].GetString());
                    info.minSize = std::stod(symbols[i]["lotSizeFilter"]["minOrderQty"].GetString());

                    if (!crypto::str_cmp(symbols[i]["lotSizeFilter"]["maxOrderQty"].GetString(), "")) {
                        info.maxSize = std::stod(symbols[i]["lotSizeFilter"]["maxOrderQty"].GetString());
                    }
                    
                    std::string contractType = "";
                    if (symbols[i].HasMember("contractType")) {
                        contractType = symbols[i]["contractType"].GetString();
                    }

                    if (crypto::str_cmp(instType.c_str(), "spot")) {
                        info.instTypeEnum = SPOT;
                        strncpy(info.margin, crypto::to_upper(symbols[i]["quoteCoin"].GetString()).c_str(), INSTID_SIZE);
                        fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                        info.lotSize = std::stod(symbols[i]["lotSizeFilter"]["basePrecision"].GetString());
                        info.minAmount = std::stod(symbols[i]["lotSizeFilter"]["minOrderAmt"].GetString());
                    }
                    else if (crypto::str_cmp(instType.c_str(), "linear") || crypto::str_cmp(instType.c_str(), "inverse")) {
                        strncpy(info.margin, crypto::to_upper(symbols[i]["settleCoin"].GetString()).c_str(), INSTID_SIZE);
                        if (crypto::str_cmp(contractType.c_str(), "LinearPerpetual")) {
                            info.instTypeEnum = USDT_SWAP;
                            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                        }
                        else if (crypto::str_cmp(contractType.c_str(), "LinearFutures")) {
                            info.instTypeEnum = USDT_FUTURES;
                            std::vector<std::string> v = crypto::split(info.originInstId, "-");
                            fmt::format_to(info.instId, "{}-{}-{}", info.base, info.quote, v[v.size() - 1]);
                        }
                        else if (crypto::str_cmp(contractType.c_str(), "InversePerpetual")) {
                            info.instTypeEnum = C_SWAP;
                            fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                        }
                        else if (crypto::str_cmp(contractType.c_str(), "InverseFutures")) {
                            info.instTypeEnum = C_FUTURES;
                            std::vector<std::string> v = crypto::split(info.originInstId, "-");
                            fmt::format_to(info.instId, "{}-{}-{}", info.base, info.quote, v[v.size() - 1]);
                        }

                        if (!crypto::str_cmp(symbols[i]["lotSizeFilter"]["minNotionalValue"].GetString(), "")) {
                            info.minAmount = std::stod(symbols[i]["lotSizeFilter"]["minNotionalValue"].GetString());
                        }
                        else {
                            info.minAmount = info.minSize;
                        }
                        info.lotSize = std::stod(symbols[i]["lotSizeFilter"]["qtyStep"].GetString());
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