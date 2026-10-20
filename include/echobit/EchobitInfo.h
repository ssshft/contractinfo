#pragma once

#include <string>

#include "base/BaseInfo.h"

// Echobit 永续合约信息
//
// 接口（实测 2026-10-20，202 条）:
//   GET https://uapi.echobit.com/uapi/contract/list      公开，无需签名
//   -> {"msg":"success","code":"0","data":[{"symbolId":"BTC-SWAP-USDT",
//        "baseTokenId":"BTC","quoteTokenId":"USDT",
//        "baseTokenPrecision":"1","quoteTokenPrecision":"0.0001",
//        "minPricePrecision":"0.1","minTradeQuantity":"1","minTradeAmount":"20",
//        "multiplier":"0.0001","showState":true,"type":4,"baseId":1,...}]}
//
// ⚠️ 四个必须知道的地方:
//
//   1) 域名是 `uapi.echobit.com`（**u**api），不是 api.echobit.com —— 后者不可达。
//      文档站是 https://echobit.gitbook.io/echobit-user-docs（gitbook，有 llms.txt）。
//
//   2) **`quoteTokenPrecision` 不是价格 tick**。BTC 是 "0.0001"，ETH 却是 "0.1"，
//      同一语义不可能差 1000 倍 —— 它是计价金额的精度。价格 tick 是
//      `minPricePrecision`（BTC 0.1 / SOL 0.01 / XRP 0.0001，与市价量级一致）。
//
//   3) `baseTokenPrecision` = "1" = **张数步长**（这个交易所里两者恰好一致，
//      但语义是"精度以步长形式给出"，所以走 sizeFromDecimal 而不是当小数位）。
//
//   4) 过滤条件要两条：`type==4`（4=合约, 1=现货）**和** `baseId==1`
//      （1=LIVE_USDT, 2=CONTRACT_SIMULATION）。测试合约的 quote 是 "TUSDT"，
//      光按 quote=="USDT" 过滤也能挡住，但两条都写上更稳。
//      `showState` 是"是否展示"，false 的 73 条是已下架/未上线的，必须跳过。
//
// 面值: `multiplier` = 一张合约代表多少标的币（BTC 0.0001 / ETH 0.01 / SOL 1.0）。
// 下单量单位 = 张，最小 1 张（minTradeQuantity）。
class EchobitInfo : public BaseInfo {
public:
    EchobitInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    void getSymbolList();
};
