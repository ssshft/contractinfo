#pragma once

#include <string>

#include "base/BaseInfo.h"

// Hotcoin 永续合约信息
//
// 接口（实测 2026-10-20，628 条）:
//   GET https://api-ct.hotcoin.fit/api/v1/perpetual/public     公开，无需签名
//   -> {"code":200,"data":[{"code":"sophusdt","base":"usdt","quote":"usdt",
//        "underlying":"soph","indexBase":"soph","direction":0,
//        "unitAmount":1000.0,"minTradeUnit":1.0,"minTradeDigit":8,
//        "marketPriceDigit":6,"minQuoteDigit":6,"env":0,"maxLever":100,...}]}
//
// ⚠️ 三个必须知道的地方（都用"直觉写法"会静默出错）:
//
//   1) **合约不在 api.hotcoinfin.com 上**。那是现货，`/v1/contract/*` 一律返回
//      {"code":10170,"msg":"API未开放"}——不是权限问题，是根本没这个路由。
//      合约是独立域 `api-ct.hotcoin.fit`，路径前缀 `/api/v1/perpetual/`。
//
//   2) **`base` 不是标的币，是保证金/计价币**。线性合约里 `base` 和 `quote` 都是
//      "usdt"，拿 base 建 instId 会得到 "USDT-USDT" 这种垃圾键，而且所有合约都会
//      互相覆盖（同一个 key）。真正的标的是 `underlying`（老数据可能只有 `indexBase`）。
//
//   3) **`minTradeDigit`=8 是标的币的小数位，不是下单张数的步长**。下单量单位是
//      "张"，张数步长看 `minTradeUnit`（实测 628 条全部 1.0）。把 minTradeDigit
//      当成数量精度会让 lotSize 变成 1e-8 张，等于没有步长约束。
//
// 面值: `unitAmount` = 一张合约代表多少标的币（soph=1000、qcom=0.01、btc 量级 0.0001）。
// 过滤: `env` 1 = 测试环境，0 = 正式（本次快照 628 条全为 0）。
// 结算: `direction` 0 = 线性(U本位) / 1 = 反向(币本位，此时 quote="usd")。
class HotcoinInfo : public BaseInfo {
public:
    HotcoinInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSymbolList();
};
