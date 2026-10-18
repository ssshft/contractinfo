#pragma once

#include "base/BaseInfo.h"

// Bitget 合约信息 (v2 mix)
//
// 一个接口按 productType 拉全部产品线:
//   usdt-futures (834) / usdc-futures (49) / coin-futures (0, 该产品线已无合约)
//
// ⚠️ 精度字段是 "整数步长 + 小数位数" 的组合, 不是直接的 tick:
//      tickSize = priceEndStep / 10^pricePlace    (BTCUSDT: 1 / 10^1 = 0.1)
//    volumePlace 只表示数量小数位, 真正的步长在 sizeMultiplier 里。
//    把 priceEndStep 直接当 tick 会得到 1.0 (BTC 价格跳 1 美元), 差 10 倍。
class BitgetInfo : public BaseInfo {
public:
    BitgetInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    // instTypeEnum 由 productType 决定 (usdt-futures -> USDT_SWAP, usdc-futures -> USDC_SWAP)
    void getMixInfo(const std::string& productType, InstType instTypeEnum);
};
