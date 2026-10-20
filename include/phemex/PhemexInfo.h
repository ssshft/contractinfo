#pragma once

#include "base/BaseInfo.h"

// Phemex 永续合约信息
//
//   GET https://api.phemex.com/public/products
//   {"code":0,"msg":"","data":{"currencies":[...],"products":[...],
//                              "perpProductsV2":[...], ...}}
//
// 实测 2026-10-20: products 1177 条（type=="Perpetual" 159 条，全部 quote=USD 的
// **币本位**；其中 status=="Listed" 只有 8 条，其余已下架）、perpProductsV2 891 条
// （USDT / USDC 本位，Listed 137 条）⇒ 实际入库 145 条。
//
// ⚠️ 两套结构不一样，必须分开解析：
//     products[]         tickSize / lotSize / contractSize / maxOrderQty 是 **double**
//     perpProductsV2[]   tickSize / qtyStepSize / maxOrderQtyRq 是 **字符串**
//
// ⚠️ **V1 的标的币在 `settleCurrency` 里，不在 symbol 里**。
//    币本位合约的名字带一个 `c` 前缀表示 coin-margined：
//      BTCUSD   settle="BTC"  -> BTC
//      cETHUSD  settle="ETH"  -> ETH      （剥 symbol 会得到 "cETH"，跨所配对直接对不上）
//    所以 V1 一律用 settleCurrency 当 base，不要用 symbol 剥 quote。
//
// ⚠️ V1 没有 minOrderQty 字段，一张起订 ⇒ minSize 取 lotSize。
//    V2 也没有 minOrderQty，只有 minOrderValueRv（USDT 金额）⇒ minSize 留 0、
//    minAmount = minOrderValueRv，下游用量校验靠金额这一条。
class PhemexInfo : public BaseInfo {
public:
    PhemexInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSymbolList();
};
