#pragma once

#include "config.h"
#include "log_engine.h"
#include "data_struct.h"
#include "redis_client.h"
#include "key_util.h"
#include "BeastRestClient.h"   // 替代 cpprest::http_client (async, keep-alive, TLS 复用)

#ifdef USE_INFO_SHM
    #include "InstrumentInfoShm.h"
#endif

#include <regex>
#include <memory>
#include <unordered_map>


class BaseInfo {
public:
    BaseInfo(RedisClient* client, Config* conf);
    BaseInfo(Config* conf);
    virtual ~BaseInfo();

    void saveData();
    void updateInstrumentInfo(const md::InstrumentInfo& info);
    std::unordered_map<std::string, md::InstrumentInfo>& getInstrumentInfo();
    void getBaseMagnifyNum(const std::string& originName, std::string& base, double& magnifyNum, double& reduceNum);
    virtual void syncExchangeInfo() = 0;

protected:
    bool syncGet(const std::string& host, const std::string& target, std::string& body_out, int& status_out);

private:
    std::unordered_map<std::string, md::InstrumentInfo> mInstrumentInfo;
    RedisClient* redisClient;
    Config* config;
    int needMagnifyNum;

    // 每个 host 的 RestClient 缓存 (lazy 创建, 复用 TCP+TLS)
    std::unordered_map<std::string, std::shared_ptr<net::RestClient>> mClients;

#ifdef USE_INFO_SHM
    // SHM 写侧句柄, ctor 里根据 config 打开 (instrumentShmName + instrumentShmCapacity)。
    // 多个 BaseInfo 子类实例共享同一段 SHM? 不。 每家交易所有自己的 BaseInfo,
    // 但 SHM 应当是全平台唯一, 所以 shmWriter_ 需要在 ContractInfoOperation 层集中维护,
    // 每个 BaseInfo 只需要一个非拥有指针指向共享 writer 即可。
    //
    // 但为简化调用路径, 我们把 SHM writer 做成 static (整个进程一份), 每个 BaseInfo
    // 用 saveData() 前会 merge 自己的 mInstrumentInfo 到全局 buffer, 全部 exchange
    // 都刷完后由 ContractInfoOperation 统一调 flushShm() 一次 atomic swap。
    //
    // 为最小改动, 目前实现是: 每个 BaseInfo 的 saveData() 各自 publish 自己 exchange 的
    // 数据到 SHM. ⚠️ 这样后一次 publish 会覆盖前一次! 所以 ContractInfoOperation 里
    // 已经收集了 mAllInfo, 应该用它做整体 publish。见 flushAllToShm 静态方法。
public:
    // 由 ContractInfoOperation 在收集完所有 exchange 的信息后调用, 一次原子写 SHM。
    // 使用 static 保证跨 BaseInfo 实例共享一个 writer。
    static void flushAllToShm(const std::unordered_map<std::string, md::InstrumentInfo>& allInfo, Config* conf);
#endif
};