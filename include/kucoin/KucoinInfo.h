#pragma once

#include "base/BaseInfo.h"

// KuCoin 合约信息
//
// ⚠️ 期货走独立域名 api-futures.kucoin.com, 不是现货的 api.kucoin.com。
//
// ⚠️ 两个坑:
//   1. baseCurrency 对 BTC 返回 "XBT" (4 个合约), 不是 "BTC"。不做映射的话
//      instId 会变成 "XBT-USDT", 和 binance 的 "BTC-USDT" 永远配不上对。
//   2. 反向合约的 multiplier 是 **-1**, 不是面值。含义是"1 张 = 1 USD",
//      value 必须取 1, 直接乘 -1 会让名义价值变成负数。
class KucoinInfo : public BaseInfo {
public:
    KucoinInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getActiveContracts();
};
