#pragma once

#include "base/BaseInfo.h"

// FameEX 永续合约信息
//
//   GET https://futuresopenapi.fameex.com/fapi/v1/contracts
//   顶层直接是数组（不是 {"code":..,"data":[..]}）—— 写成 doc["data"] 会拿到空。
//
// 实测 2026-10-20: 255 条，全部 -USDT 计价，status=1 的 220 条。
//
// ⚠️ 基址是 futuresopenapi.fameex.com，**不是** api.fameex.com（后者对任意路径返回
//    403 {"msg":"Forbidden"}）。另外 sapi/v1/* 那条是现货，合约在 fapi/v1/contracts。
// ⚠️ symbol 形如 "E-BTC-USDT"（E- 前缀），要按 '-' 切成三段取 base/quote。
// ⚠️ 没有数量精度字段，下单量单位是"张"，步长 1；面值在 multiplier 里。
class FameexInfo : public BaseInfo {
public:
    FameexInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSymbolList();
};
