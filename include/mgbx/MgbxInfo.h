#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "base/BaseInfo.h"

// MGBX 永续合约信息
//
// ⚠️ 这家**没有"一次拿全部合约规格"的接口**，必须两步：
//
//   第 1 步  取交易对名单（有 symbol，但没有规格）
//     GET https://open.mgbx.com/fut/v1/public/q/tickers
//     -> {"code":0,"data":[{"s":"btc_usdt","c":"81404.2",...}, ...]}   实测 283 条
//
//   第 2 步  逐个取规格（必须带 symbol，不带就返回 {"code":-1,"msg":"invalid symbol"}）
//     GET https://open.mgbx.com/fut/v1/public/symbol/detail?symbol=btc_usdt
//     -> {"code":0,"data":{"symbol":"btc_usdt","baseCoin":"btc","quoteCoin":"usdt",
//          "contractSize":"0.0001","quantityPrecision":0,"pricePrecision":1,
//          "minStepPrice":"0.1","minQty":"1","minNotional":"1",
//          "maxNotional":"100000000","tradeSwitch":true,"state":0,
//          "underlyingType":"U_BASED",...}}
//
// 所以这里做了缓存：`seen_` 记住已取到规格的 symbol，后续每轮只补新上的合约。
// 首轮 283 个请求（keep-alive 复用连接，约 5~10 秒），之后每轮基本 0~2 个。
//
// 其它必须知道的:
//   - 文档站 apidocs.mgbx.com，但**接口域名是 open.mgbx.com**（apidoc/apidocs 只是文档）。
//   - `contractSize` 是面值：BTC 0.0001、DOGE 10、1000CHEEMS 1000。
//     注意 1000CHEEMS 的 `baseCoin` 就是 "1000cheems"（一个 bundle = 1000 CHEEMS），
//     所以 1 张 = 1000 个 bundle = 1e6 CHEEMS —— 与 getBaseMagnifyNum 的 ×1000 叠加，
//     两者是相乘关系，不是重复计算。
//   - `minStepPrice` 与 `pricePrecision` 一致（0.1 / 1 位），优先用 minStepPrice。
//   - `quantityPrecision` = 张数小数位（0 ⇒ 步长 1 张）。
//   - `minNotional` 是 USDT 金额，不要塞进 maxSize（maxSize 单位是张）。
//   - `state` 0 = 正常；`tradeSwitch` false = 暂停交易。两条都要过。
class MgbxInfo : public BaseInfo {
public:
    MgbxInfo(RedisClient* client, Config* conf);
    virtual void syncExchangeInfo();

    // 第 1 步：取 symbol 名单，返回条数（<0 = 失败）
    int fetchSymbols(std::vector<std::string>& symbols);
    // 第 2 步：取单个 symbol 的规格。
    //   返回 1  = 已写入 mInstrumentInfo
    //   返回 0  = 拿到了响应但不可交易（state!=0 / tradeSwitch=false）—— 也算"已处理"
    //   返回 -1 = 网络或解析失败，下一轮要重试
    int fetchDetail(const std::string& symbol);

    void getSymbolList();

private:
    std::unordered_set<std::string> seen_;   // 已经取到规格的 symbol（进程内缓存）
};
