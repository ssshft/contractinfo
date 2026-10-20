#include "operation/ExchangeInfoFactory.h"

#include "binance/BinanceInfo.h"
#include "okx/OkxInfo.h"
#include "bybit/BybitInfo.h"
#include "gateio/GateioInfo.h"
#include "htx/HtxInfo.h"
#include "bitget/BitgetInfo.h"
#include "mexc/MexcInfo.h"
#include "kucoin/KucoinInfo.h"
#include "bingx/BingxInfo.h"
#include "xt/XtInfo.h"
#include "bitunix/BitunixInfo.h"
#include "phemex/PhemexInfo.h"
#include "weex/WeexInfo.h"
#include "fameex/FameexInfo.h"
#include "blofin/BlofinInfo.h"
#include "hotcoin/HotcoinInfo.h"
#include "echobit/EchobitInfo.h"
#include "mgbx/MgbxInfo.h"

namespace exchfac {

namespace {

// 构造函数签名。无捕获 lambda 可以隐式转成这个函数指针类型。
using Creator = BaseInfo* (*)(RedisClient*, Config*);

struct Entry {
    const char* name;
    Creator     fn;
};

// ---------------------------------------------------------------------------
// 交易所注册表 —— 这是**唯一**需要改的地方。
//
// 新增一家交易所时:
//   1. 写 include/xxx/XxxInfo.h + src/xxx/XxxInfo.cpp, 在 CMakeLists.txt 里加
//      aux_source_directory(... DIR_XXX_SRCS) 并放进 ContractInfoCore 的源列表
//   2. 在上面 include 一份头文件
//   3. 在下面**追加**一行 (顺序无所谓, 但保持"老所在前、新所在后"便于对照日志)
//   4. data_struct.h 的 enum ExchangeType + ExchangeTypeEnum2StrMap +
//      ExchangeTypeStr2EnumMap 三处必须同一次改完 (只追加, 不许插中间)
//   5. etc/contract_info.json 的 exchangeList 里加名字
//
// 漏掉第 4 步的症状: 复合键退化成 ".USDT_SWAP.BTC-USDT", 跨所互相撞键, 不报错。
// 漏掉第 5 步的症状: 进程照常跑, 但这家一条合约都没有 (preStart 现在会 LOG_WARN)。
// 第 4/5 步都能被 tools/InfoCheck 的 R1/R3 两项检查抓出来。
// ---------------------------------------------------------------------------
const Entry kEntries[] = {
    {"BINANCE",  +[](RedisClient* c, Config* f) -> BaseInfo* { return new BinanceInfo(c, f); }},
    {"OKX",      +[](RedisClient* c, Config* f) -> BaseInfo* { return new OkxInfo(c, f); }},
    {"BYBIT",    +[](RedisClient* c, Config* f) -> BaseInfo* { return new BybitInfo(c, f); }},
    {"GATEIO",   +[](RedisClient* c, Config* f) -> BaseInfo* { return new GateioInfo(c, f); }},
    {"HTX",      +[](RedisClient* c, Config* f) -> BaseInfo* { return new HtxInfo(c, f); }},
    {"BITGET",   +[](RedisClient* c, Config* f) -> BaseInfo* { return new BitgetInfo(c, f); }},
    {"MEXC",     +[](RedisClient* c, Config* f) -> BaseInfo* { return new MexcInfo(c, f); }},
    {"KUCOIN",   +[](RedisClient* c, Config* f) -> BaseInfo* { return new KucoinInfo(c, f); }},
    {"BINGX",    +[](RedisClient* c, Config* f) -> BaseInfo* { return new BingxInfo(c, f); }},
    {"XT",       +[](RedisClient* c, Config* f) -> BaseInfo* { return new XtInfo(c, f); }},
    {"BITUNIX",  +[](RedisClient* c, Config* f) -> BaseInfo* { return new BitunixInfo(c, f); }},
    {"PHEMEX",   +[](RedisClient* c, Config* f) -> BaseInfo* { return new PhemexInfo(c, f); }},
    {"WEEX",     +[](RedisClient* c, Config* f) -> BaseInfo* { return new WeexInfo(c, f); }},
    {"FAMEEX",   +[](RedisClient* c, Config* f) -> BaseInfo* { return new FameexInfo(c, f); }},
    {"BLOFIN",   +[](RedisClient* c, Config* f) -> BaseInfo* { return new BlofinInfo(c, f); }},
    {"HOTCOIN",  +[](RedisClient* c, Config* f) -> BaseInfo* { return new HotcoinInfo(c, f); }},
    {"ECHOBIT",  +[](RedisClient* c, Config* f) -> BaseInfo* { return new EchobitInfo(c, f); }},
    {"MGBX",     +[](RedisClient* c, Config* f) -> BaseInfo* { return new MgbxInfo(c, f); }},
};

constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

}  // namespace

BaseInfo* create(const std::string& exchId, RedisClient* client, Config* conf) {
    for (size_t i = 0; i < kEntryCount; ++i) {
        // 与原 preStart 的 crypto::str_cmp 语义完全一致: 大小写敏感的精确匹配。
        if (exchId == kEntries[i].name) {
            return kEntries[i].fn(client, conf);
        }
    }
    return nullptr;
}

bool isSupported(const std::string& exchId) {
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (exchId == kEntries[i].name) {
            return true;
        }
    }
    return false;
}

const std::vector<std::string>& supported() {
    static const std::vector<std::string> kVec = [] {
        std::vector<std::string> v;
        v.reserve(kEntryCount);
        for (size_t i = 0; i < kEntryCount; ++i) {
            v.emplace_back(kEntries[i].name);
        }
        return v;
    }();
    return kVec;
}

std::string supportedJoined(const char* sep) {
    std::string out;
    const auto& v = supported();
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) {
            out += sep;
        }
        out += v[i];
    }
    return out;
}

}  // namespace exchfac
