#include "gateio/GateioInfo.h"

GateioInfo::GateioInfo(RedisClient* client, Config* conf) : BaseInfo(client, conf) {

}

void GateioInfo::syncExchangeInfo() {
    getSpotInfo();
    std::this_thread::sleep_for(std::chrono::seconds(1));

    
    getFuturesInfo("usdt");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    getFuturesInfo("btc");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    getDeliveryInfo("usdt");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    getDeliveryInfo("btc");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
}

void GateioInfo::getSpotInfo() {
    try {
        http_client restClient("https://api.gateio.ws/api/v4/spot/currency_pairs");
        http_request request(methods::GET);

        http_response response = restClient.request(request).get();
        auto code = response.status_code();
        if (code == status_codes::OK) {
            const std::string& v = response.extract_string().get();
            rapidjson::Document d;
            rapidjson::Value& symbols = d.Parse<rapidjson::kParseNumbersAsStringsFlag>(v.c_str());
    
            for (rapidjson::SizeType i = 0; i < symbols.Size(); ++i) {
                md::InstrumentInfo info;
                memset(&info, 0, sizeof(md::InstrumentInfo));

                std::string symbol = symbols[i]["id"].GetString();
                std::string baseAsset = crypto::to_upper(symbols[i]["base"].GetString());
                std::string base = "";
                double magnifyNumber = 1.0;
                double reduceNumber = 1.0;
                getBaseMagnifyNum(baseAsset, base, magnifyNumber, reduceNumber);

                info.exchangeTypeEnum = GATEIO;
                info.instTypeEnum = SPOT;
                strncpy(info.originInstId, symbol.c_str(), INSTID_SIZE);
                strncpy(info.base, base.c_str(), INSTID_SIZE);
                strncpy(info.quote, symbols[i]["quote"].GetString(), INSTID_SIZE);
                strncpy(info.margin, symbols[i]["quote"].GetString(), INSTID_SIZE);
                fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                info.value = 1;
                info.magnifyNumber = magnifyNumber;
                info.reduceNumber = reduceNumber;

                double precision = std::stod(symbols[i]["precision"].GetString());
                double amountPrecison = std::stod(symbols[i]["amount_precision"].GetString());

                info.tickSize = std::pow(10, -precision);
                info.lotSize = std::pow(10, -amountPrecison);

                if (symbols[i].HasMember("min_base_amount")) {
                    info.minSize = std::stod(symbols[i]["min_base_amount"].GetString());
                }
     
                if (symbols[i].HasMember("max_base_amount")) {
                    info.maxSize = std::stod(symbols[i]["max_base_amount"].GetString());
                }

                if (symbols[i].HasMember("min_quote_amount")) {
                    info.minAmount = std::stod(symbols[i]["min_quote_amount"].GetString());
                }

                updateInstrumentInfo(info);
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

void GateioInfo::getFuturesInfo(std::string settle) {
    try {
        std::string url = "https://api.gateio.ws/api/v4/futures/" + settle + "/contracts";
        http_client restClient(url);
        http_request request(methods::GET);

        http_response response = restClient.request(request).get();
        auto code = response.status_code();
        if (code == status_codes::OK) {
            const std::string& v = response.extract_string().get();
            rapidjson::Document d;
            rapidjson::Value& symbols = d.Parse<rapidjson::kParseNumbersAsStringsFlag>(v.c_str());

            for (rapidjson::SizeType i = 0; i < symbols.Size(); ++i) {
                md::InstrumentInfo info;
                memset(&info, 0, sizeof(md::InstrumentInfo));

                std::string symbol = symbols[i]["name"].GetString();
                std::vector<std::string> v = crypto::split(symbol, "_");
                std::string baseAsset = crypto::to_upper(v[0]);
                std::string base = "";
                double magnifyNumber = 1.0;
                double reduceNumber = 1.0;
                getBaseMagnifyNum(baseAsset, base, magnifyNumber, reduceNumber);

                info.exchangeTypeEnum = GATEIO;
                if (crypto::str_cmp(settle, "usdt")) {
                    info.instTypeEnum = USDT_SWAP;
                }
                else if (crypto::str_cmp(settle, "btc")) {
                    info.instTypeEnum = C_SWAP;
                }
                
                strncpy(info.originInstId, symbol.c_str(), INSTID_SIZE);
                strncpy(info.base, base.c_str(), INSTID_SIZE);
                strncpy(info.quote, crypto::to_upper(v[1]).c_str(), INSTID_SIZE);
                strncpy(info.margin, crypto::to_upper(v[1]).c_str(), INSTID_SIZE);
                fmt::format_to(info.instId, "{}-{}", info.base, info.quote);
                info.value = std::stod(symbols[i]["quanto_multiplier"].GetString());
                info.magnifyNumber = magnifyNumber;
                info.reduceNumber = reduceNumber;

                info.tickSize = std::stod(symbols[i]["order_price_round"].GetString());
                info.lotSize = std::stod(symbols[i]["order_size_min"].GetString());
                info.minSize = info.lotSize;
                info.maxSize = std::stod(symbols[i]["order_size_max"].GetString());
                info.minAmount = info.lotSize;

                updateInstrumentInfo(info);
            }
        }
        else {
            LOG_ERROR("getFuturesInfo error! code: {}", code);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("{}", e.what());
    }
}

void GateioInfo::getDeliveryInfo(std::string settle) {
    try {
        std::string url = "https://api.gateio.ws/api/v4/delivery/" + settle + "/contracts";
        http_client restClient(url);
        http_request request(methods::GET);

        http_response response = restClient.request(request).get();
        auto code = response.status_code();
        if (code == status_codes::OK) {
            const std::string& v = response.extract_string().get();
            rapidjson::Document d;
            rapidjson::Value& symbols = d.Parse<rapidjson::kParseNumbersAsStringsFlag>(v.c_str());

            for (rapidjson::SizeType i = 0; i < symbols.Size(); ++i) {
                md::InstrumentInfo info;
                memset(&info, 0, sizeof(md::InstrumentInfo));

                std::string symbol = symbols[i]["name"].GetString();
                std::vector<std::string> v = crypto::split(symbol, "_");
                std::string baseAsset = crypto::to_upper(v[0]);
                std::string base = "";
                double magnifyNumber = 1.0;
                double reduceNumber = 1.0;
                getBaseMagnifyNum(baseAsset, base, magnifyNumber, reduceNumber);

                info.exchangeTypeEnum = GATEIO;
                if (crypto::str_cmp(settle, "usdt")) {
                    info.instTypeEnum = USDT_FUTURES;
                }
                else if (crypto::str_cmp(settle, "btc")) {
                    info.instTypeEnum = C_FUTURES;
                }
                
                strncpy(info.originInstId, symbol.c_str(), INSTID_SIZE);
                strncpy(info.base, base.c_str(), INSTID_SIZE);
                strncpy(info.quote, crypto::to_upper(v[1]).c_str(), INSTID_SIZE);
                strncpy(info.margin, crypto::to_upper(v[1]).c_str(), INSTID_SIZE);

                std::string thirdStr = v[2].substr(2);
                fmt::format_to(info.instId, "{}-{}-{}", info.base, info.quote, thirdStr);
                info.value = std::stod(symbols[i]["quanto_multiplier"].GetString());
                info.magnifyNumber = magnifyNumber;
                info.reduceNumber = reduceNumber;

                info.tickSize = std::stod(symbols[i]["order_price_round"].GetString());
                info.lotSize = std::stod(symbols[i]["order_size_min"].GetString());
                info.minSize = info.lotSize;
                info.maxSize = std::stod(symbols[i]["order_size_max"].GetString());
                info.minAmount = info.lotSize;

                updateInstrumentInfo(info);
            }
        }
        else {
            LOG_ERROR("getDeliveryInfo error! code: {}", code);
        }
    }
    catch (std::exception& e) {
        LOG_ERROR("{}", e.what());
    }
}