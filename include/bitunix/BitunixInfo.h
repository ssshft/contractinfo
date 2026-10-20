#pragma once

#include "base/BaseInfo.h"

// Bitunix 永续合约信息
//
//   GET https://fapi.bitunix.com/api/v1/futures/market/trading_pairs
//   {"code":0,"data":[{...}],"msg":""}
//
// 实测 2026-10-20: 781 条；quote 有 USDT / USDC / USD 三种（USD = 币本位）。
// 过滤: symbolStatus == "OPEN" && isApiSupported == true（PREVIEW 是还没开放的）
//
// ⚠️ basePrecision 是**数量**小数位、quotePrecision 是**价格**小数位 ——
//    名字和直觉相反。DOGEUSDT 的 basePrecision=0 / minTradeVolume=53，
//    即数量步长 1、最小 53 张，说明下单量单位就是标的本身（面值 1）。
class BitunixInfo : public BaseInfo {
public:
    BitunixInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSymbolList();
};
