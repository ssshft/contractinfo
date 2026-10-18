#pragma once

#include "base/BaseInfo.h"

// HTX (原 Huobi) 合约信息
//
// ⚠️ 期货/合约走的是**独立域名** api.hbdm.com, 不是现货的 api.huobi.pro。
//    用错域名会拿到 404 而不是报错, 表现为 "这家所没有合约"。
//
// 两条产品线各一个接口, 字段名一致但 contract_size 的含义不同:
//   USDT 本位  /linear-swap-api/v1/swap_contract_info   contract_size = 多少个 base 币 (BTC-USDT: 0.001)
//   币本位     /swap-api/v1/swap_contract_info          contract_size = 多少 USD        (BTC-USD: 100)
class HtxInfo : public BaseInfo {
public:
    HtxInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    // isCoinMargined=true -> 币本位 (C_SWAP, calcType=1)
    void getSwapInfo(const std::string& host, const std::string& path, bool isCoinMargined);
};
