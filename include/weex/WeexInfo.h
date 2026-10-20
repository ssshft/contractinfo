#pragma once

#include "base/BaseInfo.h"

// WEEX 永续合约信息（Bitget 同源，v3 接口）
//
//   GET https://api-contract.weex.com/capi/v3/market/exchangeInfo
//   {"assets":[...],"rateLimits":[...],"symbols":[{...}]}
//
// 实测 2026-10-20: symbols 1040 条（PERPETUAL 563 条，其余是 TRADIFI_PERPETUAL
// 代币化股票），marginAsset 全是 USDT。
//
// ⚠️ 三个坑：
//   1. v2 路径（/api/v2/mix/market/contracts）已经下线，返回
//      "The v2 service has been discontinued"；正确路径是 /capi/v3/market/exchangeInfo。
//   2. quantityPrecision **可能是负数**（DOGEUSDT = -2），表示数量步长 = 10^2 = 100。
//      infohelp::sizeFromStep 会把负 digits 钳成 0，所以负数必须走 sizeFromFixed。
//   3. 没有 status / enableTrade 字段 —— 这个接口不告诉你哪些合约能下单，只能全收。
//   4. contractType 除了 PERPETUAL 还有 TRADIFI_PERPETUAL（代币化股票），必须过滤。
class WeexInfo : public BaseInfo {
public:
    WeexInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSymbolList();
};
