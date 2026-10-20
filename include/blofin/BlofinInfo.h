#pragma once

#include <string>

#include "base/BaseInfo.h"

// BloFin 永续合约信息（OKX 同源接口风格）
//
//   GET https://openapi.blofin.com/market/instruments?instType=SWAP
//
// ⚠️ **本文件未做实测** —— openapi.blofin.com / api.blofin.com 对本站全部返回 451
//    （地域封锁，WAF 页面），拿不到任何真实响应。字段来自 ccxt 的
//    ts/src/blofin.ts（fetchMarkets / parseMarket），**不是抓包核对过的**。
//
// 因为没实测，这里对唯一不确定的一处做了兜底：**路径前缀**。
//   ccxt 里 endpoint 键是 `market/instruments`（不带版本前缀），
//   但 BloFin 是 OKX 血统，很多部署版本实际是 `/api/v5/market/instruments`。
//   两条都试，第一条能解析出 data 数组的记进 path_，之后复用。
//
// 上线前必须在部署机确认日志 `BlofinInfo -> kept=N path=...` 且 N > 0、
// instId 形如 "BTC-USDT"。若两条都 0，说明字段名变了，回 ccxt 对一遍。
class BlofinInfo : public BaseInfo {
public:
    BlofinInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSymbolList();

private:
    std::string path_;   // 首次成功的那条路径
};
