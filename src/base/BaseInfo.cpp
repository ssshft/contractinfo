#include "base/BaseInfo.h"
#include <future>


BaseInfo::BaseInfo(RedisClient* client, Config* conf) {
    redisClient = client;
    config = conf;
    std::string magnifNumStr = "";
    config->get_string("needMagnifyNum", magnifNumStr);
    needMagnifyNum = std::stoi(magnifNumStr);
}

BaseInfo::~BaseInfo() {
    // mClients 的 shared_ptr 会依次析构, RestClient::dtor 内会 stop worker + drain pending
}

void BaseInfo::saveData() {
#ifdef USE_INFO_SHM
    // SHM 模式: 单个 BaseInfo 不 publish, 由 ContractInfoOperation 收集所有 exchange
    // 数据后统一调 flushAllToShm() 一次原子写。
    // 这里只做本地保存 (mInstrumentInfo 已经在 updateInstrumentInfo 里存好), no-op。
    (void)0;
#else
    // Redis 模式: 逐条写 (旧逻辑)
    for (auto iter = mInstrumentInfo.begin(); iter != mInstrumentInfo.end(); ++iter) {
        auto& info = iter->second;
        std::string redisKey = crypto::get_instrumentInfo_channel_key(
            ExchangeTypeEnum2StrMap[info.exchangeTypeEnum],
            InstTypeEnum2StrMap[info.instTypeEnum],
            info.instId);
        std::string infoJsonStr = info.getJsonStr();
        redisClient->set(redisKey, infoJsonStr);
    }
#endif
}

void BaseInfo::updateInstrumentInfo(const md::InstrumentInfo& info) {
    std::string key = fmt::format("{}.{}.{}", ExchangeTypeEnum2StrMap[info.exchangeTypeEnum], InstTypeEnum2StrMap[info.instTypeEnum], info.instId);
    mInstrumentInfo[key] = info;
}

std::unordered_map<std::string, md::InstrumentInfo>& BaseInfo::getInstrumentInfo() {
    return mInstrumentInfo;
}

void BaseInfo::getBaseMagnifyNum(const std::string& originName, std::string& base, double& magnifyNum, double& reduceNum) {
    magnifyNum = 1.0;
    reduceNum = 1.0;
    base = originName;

    if (needMagnifyNum == 0) {
        return;
    }

    std::regex numRegex(R"(^(\d+)([a-zA-Z]+)$)");
    std::smatch match;
    std::string result = base;

    if (std::regex_match(base, match, numRegex)) {
        std::string numStr = match[1].str();
        std::string letterStr = match[2].str();

        double num = std::stod(numStr);

        if (num > 100) {
            magnifyNum = num;
            reduceNum = 1.0 / num;
            base = letterStr;
        }
    }
}


// ============================================================================
// syncGet: BeastRestClient 的同步包装 (基于 promise/future)
// ============================================================================
bool BaseInfo::syncGet(const std::string& host, const std::string& target,
                       std::string& body_out, int& status_out) {
    auto it = mClients.find(host);
    if (it == mClients.end()) {
        net::RestClientConfig cfg;
        cfg.host                        = host;
        cfg.port                        = 443;
        cfg.use_tls                     = true;
        cfg.verify_peer                 = false;
        cfg.max_connections             = 1;
        cfg.parallel_establish_threads  = 1;
        cfg.request_queue_capacity      = 16;
        cfg.request_pool_size           = 8;
        cfg.request_timeout_ms          = 30'000;

        try {
            auto client = std::make_shared<net::RestClient>(cfg);
            it = mClients.emplace(host, std::move(client)).first;
        }
        catch (const std::exception& e) {
            LOG_ERROR("syncGet: create RestClient for {} failed: {}", host, e.what());
            return false;
        }
    }

    auto promise = std::make_shared<std::promise<std::pair<boost::system::error_code, net::HttpResponse>>>();
    auto future  = promise->get_future();

    it->second->async_request(
        boost::beast::http::verb::get,
        target,
        std::string(),
        std::string(),
        [promise](boost::system::error_code ec, net::HttpResponse resp) {
            promise->set_value(std::make_pair(ec, std::move(resp)));
        });

    if (future.wait_for(std::chrono::seconds(35)) != std::future_status::ready) {
        LOG_ERROR("syncGet {} {} timeout (>35s, RestClient worker maybe stuck?)", host, target);
        return false;
    }

    auto result = future.get();
    boost::system::error_code ec = result.first;
    net::HttpResponse resp       = std::move(result.second);

    if (ec) {
        LOG_ERROR("syncGet {} {} failed: {}", host, target, ec.message());
        return false;
    }
    status_out = resp.status_code;
    body_out   = std::move(resp.body);
    return true;
}


// ============================================================================
// SHM 写: 由 ContractInfoOperation 汇总完所有 exchange 后调, 一次原子交换
// ============================================================================
#ifdef USE_INFO_SHM
void BaseInfo::flushAllToShm(const std::unordered_map<std::string, md::InstrumentInfo>& allInfo,
                             Config* conf) {
    // 静态 writer, 整个进程共享一个 mmap 段
    static sm::shm::Writer sWriter;
    static bool sOpened = false;

    if (!sOpened) {
        std::string name = sm::shm::kDefaultShmName;
        conf->get_string("instrumentShmName", name);

        std::string capStr;
        uint32_t capacity = sm::shm::kDefaultCapacity;
        if (conf->get_string("instrumentShmCapacity", capStr) && !capStr.empty()) {
            try { capacity = static_cast<uint32_t>(std::stoul(capStr)); }
            catch (...) { capacity = sm::shm::kDefaultCapacity; }
        }

        if (!sWriter.open(name, capacity)) {
            LOG_ERROR("[shm] failed to open/create instrument info SHM '{}' capacity={}",
                      name, capacity);
            return;
        }
        LOG_INFO("[shm] instrument info SHM ready: name='{}' capacity={}", name, capacity);
        sOpened = true;
    }

    // 把 map 打成 vector; 顺序无所谓, reader 会重建 hash 索引
    std::vector<md::InstrumentInfo> vec;
    vec.reserve(allInfo.size());
    for (auto& kv : allInfo) {
        vec.push_back(kv.second);
    }

    if (vec.size() > sWriter.capacity()) {
        LOG_ERROR("[shm] instrument count {} exceeds SHM capacity {}. "
                  "Increase 'instrumentShmCapacity' in config and restart contractinfo.",
                  vec.size(), sWriter.capacity());
        // Writer::publish 会自动截断到 capacity, 但下游会缺条目, 必须告警
    }

    if (!sWriter.publish(vec)) {
        LOG_ERROR("[shm] publish failed, count={}", vec.size());
        return;
    }
    LOG_INFO("[shm] published {} instruments", vec.size());
}
#endif