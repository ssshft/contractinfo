#include "base/BaseInfo.h"



BaseInfo::BaseInfo(RedisClient* client, Config* conf) {
    redisClient = client;
    config = conf;
    std::string magnifNumStr = "";
    config->get_string("needMagnifyNum", magnifNumStr); 
    needMagnifyNum = std::stoi(magnifNumStr);
}

BaseInfo::~BaseInfo() {
    
}

void BaseInfo::saveData() {
    for (auto iter = mInstrumentInfo.begin(); iter != mInstrumentInfo.end(); ++iter) {
        auto& info = iter->second;
        std::string redisKey = crypto::get_instrumentInfo_channel_key(ExchangeTypeEnum2StrMap[info.exchangeTypeEnum], InstTypeEnum2StrMap[info.instTypeEnum], info.instId);
        std::string infoJsonStr = info.getJsonStr();
        redisClient->set(redisKey, infoJsonStr);
    }
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

    // 查找以数字开头、字母结尾的字符串
    std::regex numRegex(R"(^(\d+)([a-zA-Z]+)$)");
    std::smatch match;
    std::string result = base;

    if (std::regex_match(base, match, numRegex)) {
        // match[1] 是数字部分
        // match[2] 是字母部分
        std::string numStr = match[1].str();
        std::string letterStr = match[2].str();
        
        double num = std::stod(numStr);
        
        if (num > 100) {
            magnifyNum = num;
            reduceNum = 1.0 / num;
            base = letterStr;  // 将base设置为字母部分
        }
    }
}