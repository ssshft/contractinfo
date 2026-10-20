#pragma once

// ============================================================================
// 交易所 Info 实例的**唯一**注册表
//
// 为什么要有这个文件:
//   以前 ContractInfoOperation::preStart() 里是一条 18 段 if/else 链, 每加一家
//   交易所就要在那条链上插一段。工具 (InfoCheck / GetInfo) 想"对某一家跑一遍
//   真实解析" 就只能再抄一份链 —— 两份链必然漂移: 生产里加了一家, 工具里没有,
//   于是"新交易所验证"这件事本身就漏掉了最新那家。
//
//   这里把链收敛成一张表 (ExchangeInfoFactory.cpp 的 kEntries), 生产路径和所有
//   tools 都从这张表取。加一家交易所 = 加一行, 两处同时生效。
//
// 用法:
//   // 1) 创建一个交易所的 Info (调用方拥有返回的指针, 记得 delete)
//   BaseInfo* info = exchfac::create("BITUNIX", nullptr, Config::instance());
//   if (!info) { /* 这家还没实现 */ }
//   info->syncExchangeInfo();                       // 同步拉一轮
//   auto& rows = info->getInstrumentInfo();         // key = "EXCH.INSTTYPE.INSTID"
//   delete info;
//
//   // 2) 判断某家是否已实现 (不构造实例)
//   if (!exchfac::isSupported("FAMEEX")) { ... }
//
//   // 3) 列出所有已实现的交易所 (顺序 = 注册表顺序)
//   for (const auto& id : exchfac::supported()) { ... }
//
// ⚠️ 两条约定:
//   1. redisClient 可以传 nullptr。BaseInfo 只在 saveData() 里用它, 且带 null 判断;
//      编译了 -DUSE_INFO_SHM 时 saveData() 是 no-op。工具不需要 Redis 就能跑解析。
//   2. create() 对未实现的交易所**返回 nullptr, 不抛异常、不打日志** —— 由调用方
//      决定是报错 (生产) 还是跳过 (工具)。
// ============================================================================

#include <string>
#include <vector>

class BaseInfo;
class RedisClient;
class Config;

namespace exchfac {

// 创建一家交易所的 BaseInfo 实例。未实现返回 nullptr。
// 返回值由调用方 delete。
BaseInfo* create(const std::string& exchId, RedisClient* client, Config* conf);

// 该交易所是否已实现 (等价于 create() != nullptr, 但不构造实例)。
bool isSupported(const std::string& exchId);

// 所有已实现的交易所 id, 顺序与注册表一致 (即 preStart 里原来的 if/else 顺序)。
const std::vector<std::string>& supported();

// 把 supported() 用分隔符拼成一个字符串, 用于日志/报错文案。
// 例: "BINANCE, OKX, BYBIT, ..."
std::string supportedJoined(const char* sep = ", ");

}  // namespace exchfac
