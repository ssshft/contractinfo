#include "operation/ContractInfoOperation.h"
#include "operation/ExchangeInfoFactory.h"
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
            const std::string& exchId = exchVec[i];

            // 交易所的创建收敛到 exchfac 这一张注册表里, 见
            // include/operation/ExchangeInfoFactory.h。以前这里是 18 段 if/else,
            // 加一家所要在生产和每个工具里各改一遍, 两份链必然漂移。
            BaseInfo* info = exchfac::create(exchId, redisClient, config);
            if (info) {
                mExchInfo[exchId] = info;
            }
            else {
                // ⚠️ 以前这里没有 else: exchangeList 里写错一个名字 (或写了尚未实现的所)
                // 会被静默忽略, 进程照常跑, 只是那家所一条合约都没有。必须打日志。
                // 名单从注册表现取, 不再手写 —— 手写的那份必然漏掉最新加的所。
                LOG_WARN("ContractInfoOperation: unknown exchangeId '{}' in exchangeList, ignored. "
                         "Implemented: {}", exchId, exchfac::supportedJoined("/"));
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