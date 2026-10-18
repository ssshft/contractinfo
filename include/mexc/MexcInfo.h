#pragma once

#include "base/BaseInfo.h"

// MEXC 合约信息
//
// ⚠️ MEXC 的下单量单位是**张**(contract), 1 张 = contractSize 个 base 币。
//    所以 value = contractSize (BTC_USDT 是 0.0001, DOGE_USDT 是 100, PEPE_USDT 是 10000000)。
//    把 minVol 当成币数会差 100~1000 万倍。
//
// ⚠️ 精度字段是 (priceUnit, priceScale) 而不是直接的 tick 字符串:
//      tickSize = priceUnit,  priceScale = 小数位数
//    PEPE 的 priceUnit 在 JSON 里是 1e-10, 直接 fast_atod 后必须按 priceScale=10 位
//    打印成 "0.0000000001", 否则 priceDigits 会被数成 0。
class MexcInfo : public BaseInfo {
public:
    MexcInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getContractDetail();
};
