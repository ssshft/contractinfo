#include "binance/BinanceInfo.h"

BinanceInfo::BinanceInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {

}

void BinanceInfo::syncExchangeInfo() {
    getSpotInfo();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    getUFutureInfo();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    getCFutureInfo();
    std::this_thread::sleep_for(std::chrono::seconds(1));
}

void BinanceInfo::getSpotInfo() {
    try {
        http_client restClient("https://api.binance.com/api/v3/exchangeInfo");
        http_request request(methods::GET);

        http_response response = restClient.request(request).get();
        auto code = response.status_code();
        if (code == status_codes::OK) {
            const std::string& v = response.extract_string().get();
            rapidjson::Document d;
            rapidjson::Value& rawData = d.Parse<rapidjson::kParseNumbersAsStringsFlag>(v.c_str());
            if (rawData.HasMember("symbols")) {
                const rapidjson::Value& symbols = rawData["symbols"];
                for (rapidjson::SizeType i = 0; i < symbols.Size(); ++i) {
                    md::InstrumentInfo info;
                    memset(&info, 0, sizeof(md::InstrumentInfo));

                    std::string symbol = symbols[i]["symbol"].GetString();
                    std::string baseAsset = symbols[i]["baseAsset"].GetString();
                    std::string base = "";
                    double magnifyNumber = 1.0;
                    double reduceNumber = 1.0;
                    getBaseMagnifyNum(baseAsset, base, magnifyNumber, reduceNumber);

                    info.exchangeTypeEnum = BINANCE;
                    info.instTypeEnum = SPOT;
                    strncpy(info.originInstId, symbol.c_str(), INSTID_SIZE);
                    strncpy(info.base, base.c_str(), INSTID_SIZE);
                    strncpy(info.quote, symbols[i]["quoteAsset"].GetString(), INSTID_SIZE);
                    strncpy(info.margin, symbols[i]["quoteAsset"].GetString(), INSTID_SIZE);
                    fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                    info.value = 1;
                    info.magnifyNumber = magnifyNumber;
                    info.reduceNumber = reduceNumber;

                    if (symbols[i].HasMember("filters")) {
                        const rapidjson::Value& filters = symbols[i]["filters"];
                        for (rapidjson::SizeType j = 0; j < filters.Size(); ++j) {
                            std::string filterType = filters[j]["filterType"].GetString();
                            if (crypto::str_cmp(filterType.c_str(), "PRICE_FILTER")) {
                                info.tickSize = std::stod(filters[j]["tickSize"].GetString());
                            }
                            else if (crypto::str_cmp(filterType.c_str(), "LOT_SIZE")) {
                                info.lotSize = std::stod(filters[j]["minQty"].GetString());
                                info.minSize = std::stod(filters[j]["minQty"].GetString());
                                info.maxSize = std::stod(filters[j]["maxQty"].GetString());
                            }
                            else if (crypto::str_cmp(filterType.c_str(), "NOTIONAL")) {
                                info.minAmount = std::stod(filters[j]["minNotional"].GetString());
                            }
                        }
                    }

                    updateInstrumentInfo(info);
                }
            }
        }
        else {
            LOG_ERROR("getSpotInfo error! code: {}", code);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("{}", e.what());
    }
}

void BinanceInfo::getUFutureInfo() {
    try {
        http_client restClient("https://fapi.binance.com/fapi/v1/exchangeInfo");
        http_request request(methods::GET);

        http_response response = restClient.request(request).get();
        auto code = response.status_code();
        if (code == status_codes::OK) {
            const std::string& v = response.extract_string().get();
            rapidjson::Document d;
            rapidjson::Value& rawData = d.Parse<rapidjson::kParseNumbersAsStringsFlag>(v.c_str());
            if (rawData.HasMember("symbols")) {
                const rapidjson::Value& symbols = rawData["symbols"];
                for (rapidjson::SizeType i = 0; i < symbols.Size(); ++i) {
                    md::InstrumentInfo info;
                    memset(&info, 0, sizeof(md::InstrumentInfo));

                    std::string symbol = symbols[i]["symbol"].GetString();
                    std::string baseAsset = symbols[i]["baseAsset"].GetString();
                    std::string base = "";
                    double magnifyNumber = 1.0;
                    double reduceNumber = 1.0;
                    getBaseMagnifyNum(baseAsset, base, magnifyNumber, reduceNumber);

                    info.exchangeTypeEnum = BINANCE;
                    strncpy(info.originInstId, symbol.c_str(), INSTID_SIZE);
                    strncpy(info.base, base.c_str(), INSTID_SIZE);
                    strncpy(info.quote, symbols[i]["quoteAsset"].GetString(), INSTID_SIZE);
                    strncpy(info.margin, symbols[i]["marginAsset"].GetString(), INSTID_SIZE);
                    info.value = 1;
                    info.magnifyNumber = magnifyNumber;
                    info.reduceNumber = reduceNumber;

                    std::string contractType = symbols[i]["contractType"].GetString();
                    if (contractType.find("PERPETUAL") != std::string::npos) {
                        fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                        if (crypto::str_cmp(info.margin, "USDT")) {
                            info.instTypeEnum = USDT_SWAP;
                        }
                        else if (crypto::str_cmp(info.margin, "BUSD")) {
                            info.instTypeEnum = BUSD_SWAP;
                        }
                        else if (crypto::str_cmp(info.base, info.margin)) {
                            info.instTypeEnum = C_SWAP;
                        }
                    }
                    else if (contractType.find("QUARTER") != std::string::npos) {
                        std::vector<std::string> v = crypto::split(info.originInstId, "_");
                        fmt::format_to(info.instId, "{}-{}-{}", info.base, info.quote, v[v.size() - 1]);
                        if (crypto::str_cmp(info.margin, "USDT")) {
                            info.instTypeEnum = USDT_FUTURES;
                        }
                        else if (crypto::str_cmp(info.margin, "BUSD")) {
                            info.instTypeEnum = BTC_FUTURES;
                        }
                        else if (crypto::str_cmp(info.base, info.margin)) {
                            info.instTypeEnum = C_FUTURES;
                        }
                    }

                    if (symbols[i].HasMember("filters")) {
                        const rapidjson::Value& filters = symbols[i]["filters"];
                        for (rapidjson::SizeType j = 0; j < filters.Size(); ++j) {
                            std::string filterType = filters[j]["filterType"].GetString();
                            if (crypto::str_cmp(filterType.c_str(), "PRICE_FILTER")) {
                                info.tickSize = std::stod(filters[j]["tickSize"].GetString());
                            }
                            else if (crypto::str_cmp(filterType.c_str(), "LOT_SIZE")) {
                                info.lotSize = std::stod(filters[j]["stepSize"].GetString());
                                info.minSize = std::stod(filters[j]["minQty"].GetString());
                                info.maxSize = std::stod(filters[j]["maxQty"].GetString());
                            }
                            else if (crypto::str_cmp(filterType.c_str(), "NOTIONAL")) {
                                info.minAmount = std::stod(filters[j]["notional"].GetString());
                            }
                        }
                    }

                    updateInstrumentInfo(info);
                }
            }
        }
        else {
            LOG_ERROR("getUFutureInfo error, code: {}", code);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("{}", e.what());
    }
}

void BinanceInfo::getCFutureInfo() {
    try {
        http_client restClient("https://dapi.binance.com/dapi/v1/exchangeInfo");
        http_request request(methods::GET);

        http_response response = restClient.request(request).get();
        auto code = response.status_code();
        if (code == status_codes::OK) {
            const std::string& v = response.extract_string().get();
            rapidjson::Document d;
            rapidjson::Value& rawData = d.Parse<rapidjson::kParseNumbersAsStringsFlag>(v.c_str());
            if (rawData.HasMember("symbols")) {
                const rapidjson::Value& symbols = rawData["symbols"];
                for (rapidjson::SizeType i = 0; i < symbols.Size(); ++i) {
                    md::InstrumentInfo info;
                    memset(&info, 0, sizeof(md::InstrumentInfo));

                    std::string symbol = symbols[i]["symbol"].GetString();
                    std::string baseAsset = crypto::to_upper(symbols[i]["baseAsset"].GetString());
                    std::string base = "";
                    double magnifyNumber = 1.0;
                    double reduceNumber = 1.0;
                    getBaseMagnifyNum(baseAsset, base, magnifyNumber, reduceNumber);

                    info.exchangeTypeEnum = BINANCE;
                    strncpy(info.originInstId, symbol.c_str(), INSTID_SIZE);
                    strncpy(info.base, base.c_str(), INSTID_SIZE);
                    strncpy(info.quote, crypto::to_upper(symbols[i]["quoteAsset"].GetString()).c_str(), INSTID_SIZE);
                    strncpy(info.margin, crypto::to_upper(symbols[i]["marginAsset"].GetString()).c_str(), INSTID_SIZE);
                    info.value = std::stod(symbols[i]["contractSize"].GetString());
                    info.magnifyNumber = magnifyNumber;
                    info.reduceNumber = reduceNumber;

                    std::string contractType = symbols[i]["contractType"].GetString();
                    if (contractType.find("PERPETUAL") != std::string::npos) {
                        fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                        if (crypto::str_cmp(info.margin, "USDT")) {
                            info.instTypeEnum = USDT_SWAP;
                        }
                        else if (crypto::str_cmp(info.margin, "BUSD")) {
                            info.instTypeEnum = BUSD_SWAP;
                        }
                        else if (crypto::str_cmp(info.base, info.margin)) {
                            info.instTypeEnum = C_SWAP;
                        }
                    }
                    else if (contractType.find("QUARTER") != std::string::npos) {
                        std::vector<std::string> v = crypto::split(info.originInstId, "_");
                        fmt::format_to(info.instId, "{}-{}-{}", info.base, info.quote, v[v.size() - 1]);
                        if (crypto::str_cmp(info.margin, "USDT")) {
                            info.instTypeEnum = USDT_FUTURES;
                        }
                        else if (crypto::str_cmp(info.margin, "BUSD")) {
                            info.instTypeEnum = BTC_FUTURES;
                        }
                        else if (crypto::str_cmp(info.base, info.margin)) {
                            info.instTypeEnum = C_FUTURES;
                        }
                    }

                    if (symbols[i].HasMember("filters")) {
                        const rapidjson::Value& filters = symbols[i]["filters"];
                        for (rapidjson::SizeType j = 0; j < filters.Size(); ++j) {
                            std::string filterType = filters[j]["filterType"].GetString();
                            if (crypto::str_cmp(filterType.c_str(), "PRICE_FILTER")) {
                                info.tickSize = std::stod(filters[j]["tickSize"].GetString());
                            }
                            else if (crypto::str_cmp(filterType.c_str(), "LOT_SIZE")) {
                                info.lotSize = std::stod(filters[j]["stepSize"].GetString());
                                info.minSize = std::stod(filters[j]["minQty"].GetString());
                                info.maxSize = std::stod(filters[j]["maxQty"].GetString());
                                info.minAmount = info.minSize;
                            }
                        }
                    }

                    updateInstrumentInfo(info);
                }
            }
        }
        else {
            LOG_ERROR("getCFutureInfo error, code: {}", code);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("{}", e.what());
    }
}