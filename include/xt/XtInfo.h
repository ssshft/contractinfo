#pragma once

#include "base/BaseInfo.h"

// XT.com 合约信息
//
// ⚠️ 期货走 fapi.xt.com, 而且路径是 **/future/market/v1/public/symbol/list**
//    (不是 fapi/market/... , 那个路径返回 200 + 空壳 {"result":{"openapiDocs":...}},
//     很容易被误判成"接口正常但没有合约")。
//
// ⚠️ 这个接口把 1183 个合约全返回了, 其中只有 1129 个是永续, 且
//    tradeSwitch/openSwitch/isOpenApi 三个开关都可能是 false —— 必须过滤,
//    否则会订到一批下不了单的合约。
class XtInfo : public BaseInfo {
public:
    XtInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSymbolList();
};
