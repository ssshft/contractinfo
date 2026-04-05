#include "base/BaseInfo.h"

class BybitInfo : public BaseInfo {
public:
    BybitInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();
    void getInfo(const std::string& instType);
};