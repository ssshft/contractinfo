#include "base/BaseInfo.h"

class BinanceInfo : public BaseInfo {
public:
    BinanceInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSpotInfo();
    void getUFutureInfo();
    void getCFutureInfo();
};