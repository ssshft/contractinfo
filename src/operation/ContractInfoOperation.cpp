#include "operation/ContractInfoOperation.h"
#include "binance/BinanceInfo.h"
#include "okx/OkxInfo.h"
#include "bybit/BybitInfo.h"
#include "gateio/GateioInfo.h"
#include "util/utils.h"


ContractInfoOperation::ContractInfoOperation() {
    redisClient = nullptr;
}

bool ContractInfoOperation::preStart(Config* config) {
    std::string host = "";
    std::string port = "";
    std::string password = "";

#ifndef USE_INFO_SHM
    if (config->get_redis_config("host", host) && config->get_redis_config("port", port) && config->get_redis_config("password", password)) {
        redisClient = new RedisClient(host.c_str(), std::stoi(port), password.c_str(), true, false);
    }
#endif

    std::vector<std::string> exchVec;
    if (config->get_exchange_list("exchangeList", exchVec)) {
        for (size_t i = 0; i < exchVec.size(); ++i) {
            std::string exchId = exchVec[i];

            if (crypto::str_cmp(exchId.c_str(), "BINANCE")) {
                mExchInfo[exchId] = new BinanceInfo(redisClient, config);
            }
            else if (crypto::str_cmp(exchId.c_str(), "OKX")) {
                mExchInfo[exchId] = new OkxInfo(redisClient, config);
            }
            else if (crypto::str_cmp(exchId.c_str(), "BYBIT")) {
                mExchInfo[exchId] = new BybitInfo(redisClient, config);
            }
            else if (crypto::str_cmp(exchId.c_str(), "GATEIO")) {
                mExchInfo[exchId] = new GateioInfo(redisClient, config);
            }
        }
    }

    if (mExchInfo.size() > 0) {
        return true;
    }
    return false;
}

void ContractInfoOperation::run() {
    LOG_INFO("ContractInfoOperation start run!");
    std::thread updateThread(&ContractInfoOperation::updateExchangeInfo, this);
    updateThread.detach();
}

void ContractInfoOperation::updateExchangeInfo() {
    while (1) {
        std::unordered_map<std::string, md::InstrumentInfo> mAllInfo;
        for (auto iter = mExchInfo.begin(); iter != mExchInfo.end(); ++iter) {
            LOG_INFO("start update {} info.", iter->first);
            iter->second->syncExchangeInfo();
            iter->second->saveData();
            std::unordered_map<std::string, md::InstrumentInfo>& mInfo = iter->second->getInstrumentInfo();
            mAllInfo.insert(mInfo.begin(), mInfo.end());
        }

    #ifdef USE_INFO_SHM
        BaseInfo::flushAllToShm(mAllInfo, Config::instance());
    #else

        const std::string& allInfoKey = crypto::get_all_instuments_key();
        const std::string& allInfoValue = mapToJsonArray(mAllInfo);
        if (redisClient) {
            redisClient->set(allInfoKey, allInfoValue);
        }
    #endif
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }

}