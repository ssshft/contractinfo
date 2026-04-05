#pragma once
#include "base/BaseInfo.h"
#include <unordered_map>

class ContractInfoOperation {
public:
    ContractInfoOperation();
    bool preStart(Config* config);
    void run();
    void updateExchangeInfo();

private:
    RedisClient* redisClient;
    std::unordered_map<std::string, BaseInfo*> mExchInfo;
};