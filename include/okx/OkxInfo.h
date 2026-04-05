#include "base/BaseInfo.h"

class OkxInfo : public BaseInfo {
public:
    OkxInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();
    void getInfo(const std::string& instType);
};