#include "base/BaseInfo.h"

class GateioInfo : public BaseInfo {
public:
    GateioInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSpotInfo();
    void getFuturesInfo(std::string settle);
    void getDeliveryInfo(std::string settle);
};