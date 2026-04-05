#pragma once
#include <cpprest/http_client.h>
#include <cpprest/http_msg.h>
#include <cpprest/json.h>
#include "config.h"
#include "log_engine.h"
#include "data_struct.h"
#include "redis_client.h"
#include "key_util.h"
#include <regex>


using namespace web::http;
using namespace web::http::client;

class BaseInfo {
public:
    BaseInfo(RedisClient* client, Config* conf);
    virtual ~BaseInfo();
    void saveData();
    void updateInstrumentInfo(const md::InstrumentInfo& info);
    std::unordered_map<std::string, md::InstrumentInfo>& getInstrumentInfo();
    void getBaseMagnifyNum(const std::string& originName, std::string& base, double& magnifyNum, double& reduceNum);
    virtual void syncExchangeInfo() = 0;

private:
    std::unordered_map<std::string, md::InstrumentInfo> mInstrumentInfo;
    RedisClient* redisClient;
    Config* config;
    int needMagnifyNum;
};