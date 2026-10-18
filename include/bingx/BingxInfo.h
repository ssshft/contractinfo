#pragma once

#include "base/BaseInfo.h"

// BingX 合约信息
//
// ⚠️ `size` 字段**不是**合约面值, 是数量步长 (恒等于 10^-quantityPrecision)。
//    BingX 永续的下单量单位就是 base 币, 所以 value = 1。
//    把 size 当 value 会让 BTC 的名义价值变成 0.0001 倍。
//
// ⚠️ status 有两种: 1 = 加密货币永续 (979 个), 25 = 外汇/合成品 (277 个, 如 NCFXEUR2JPY-USDT)。
//    本工程只做加密币价差, 只保留 status==1。
class BingxInfo : public BaseInfo {
public:
    BingxInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSwapContracts();
};
