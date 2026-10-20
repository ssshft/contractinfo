// tools/InfoCheck.cpp
// ============================================================================
// contractinfo 的「新交易所验收器」—— 拉一遍真实数据，跑一组不变量，给出 PASS/FAIL。
//
// 为什么需要它:
//   contractinfo 的错误几乎全是**静默**的。交易所给的字段名变了、精度单位理解反了、
//   面值差 100 倍、instId 拼写跟 db 里的配置对不上 —— 这些都不会抛异常，也不会打
//   错误日志，只会让下游 db 一个 unit 都起不来，或者把挂单价 quantize 成 0。
//   肉眼看 300 条记录是查不出来的，所以把"什么叫对"写成断言。
//
//   和 tools/GetInfo 的分工:
//     GetInfo    = 读侧: 通过 SecurityManager 查 SHM/Redis，看 db 实际能拿到什么
//     InfoCheck  = 写侧: 直接构造 XxxInfo 调 syncExchangeInfo()，看我们写进去了什么
//     DumpInfoShm= 裸 SHM 全量 dump（不解释语义）
//
//   规则本身在 tools/InfoCheckRules.h 里，和 IO 完全解耦 —— 那样
//   tools/selftest_rules 才能在本机（编不了整个工程）验证"该报的报、不该报的不报"。
//   本文件只负责：拉数据、跑规则、出报告、存快照、比基线。
//
// 用法:
//   ./InfoCheck                                  # 跑 etc/contract_info.json 里所有交易所
//   ./InfoCheck --exchange BITUNIX               # 只跑一家
//   ./InfoCheck --exchange BITUNIX,PHEMEX        # 跑几家
//   ./InfoCheck --exchange BITUNIX --sample 10   # 顺便打印 10 条明细
//   ./InfoCheck --config ../etc/contract_info.json
//   ./InfoCheck --out snap.json                  # 存快照（下次用 --diff 比）
//   ./InfoCheck --diff snap.json                 # 与快照比：新增/下架/精度变化
//   ./InfoCheck --from-json snap.json            # 不联网，直接对快照跑全部检查
//   ./InfoCheck --json                           # 机器可读输出
//   ./InfoCheck --db-config ../db/etc/small_swap_db.json
//                                                # 额外检查 db 配置里的对子能否查到
//   ./InfoCheck --strict                         # WARN 也算失败（CI 用）
//
// 退出码:
//   0  全部 PASS（或只有 INFO）
//   1  有 ERROR / FATAL
//   2  用法或配置文件错误（还没开始检查）
//   3  只有 WARN 但开了 --strict
//
// 检查项（编号与输出一一对应；逐条规则的详细说明见 InfoCheckRules.h）:
//   R1 enum_map       enum<->字符串 两张表完整、无空值、无重复、双向一致、无编号空洞
//   R2 config         config 可解析、needMagnifyNum 存在（缺了 BaseInfo 构造就抛异常）
//   R3 supported      exchangeList 里每个名字都在注册表里（拼错=该所静默零数据）
//   A1 fetch          拉到的条数 > 0（=0 说明接口/解析/过滤三者之一挂了）
//   B1..B10           标识字段：非空 / base!=quote / 长度不越界 / 无空白 / 不重复 /
//                     exchangeTypeEnum 与正在拉取的那家一致（防复制粘贴忘改名）
//   C1..C8            精度四件套：pow10 == 10^digits、tickInt 自洽、quantize 往返
//   D1..D3            交易约束：minSize<=maxSize、minSize>0、minAmount>=0
//   E1..E3            面值与放大：value>0、magnify*reduce==1、base 数字前缀与 magnify 一致
//   F1,F2             跨所：同合约面值比值应是 10 的整数次幂、tick 数量级不应差 >1000 倍
//   G1 db_config      db 的 small_swap_db.json 里写的 instId 在 contractinfo 里查得到
//   H1 diff           与基线快照的差异
//
// ⚠️ C6/C7 是最关键的两条。它们直接对应"挂单价被 crypto::quantize() 截成 0"这个
//    下游事故：只要 (tickSize, pricePow10, tickSizeInt) 三者不自洽，quantize(tickSize)
//    就会返回 0，而 quantize 本身不报错。
// ============================================================================

#include "InfoCheckRules.h"   // 逐条规则（纯 std，可单独自测）

#include "operation/ExchangeInfoFactory.h"
#include "base/BaseInfo.h"
#include "string_util.h"      // crypto::fast_atod —— JsonField.h 的 jf::dbl 依赖它，
                              // 但 JsonField.h 自己不 include，靠包含顺序碰运气很容易炸
#include "base/JsonField.h"
#include "config.h"
#include "data_struct.h"
#include "log_engine.h"
#include "program_util.h"     // crypto::create_directory

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/stat.h>

using namespace icrule;   // Check / Suite / CheckDef / num / nearly / kRowDefs / Row / RowRunner

namespace {

// ============================================================================
// 其余三组检查的定义（这三组需要 IO 或全局信息，所以留在这里）
// ============================================================================

enum Rk { R1_ENUMMAP, R2_CONFIG, R3_SUPPORTED, R_N };
const std::vector<CheckDef> kRegDefs = {
    {"R1", "enum_map",  SEV_FATAL},
    {"R2", "config",    SEV_FATAL},
    {"R3", "supported", SEV_FATAL},
};

enum Fk { F1_XVALUE, F2_XTICK, F_N };
const std::vector<CheckDef> kCrossDefs = {
    {"F1", "xValue", SEV_WARN},
    {"F2", "xTick",  SEV_INFO},
};

enum Gk { G1_DBCFG, G_N };
const std::vector<CheckDef> kDbDefs = {
    {"G1", "dbConfig", SEV_ERROR},
};

enum Hk { H1_DIFF, H_N };
const std::vector<CheckDef> kDiffDefs = {
    {"H1", "diff", SEV_WARN},
};

// ============================================================================
// 小工具
// ============================================================================

// 定长 char[] 的安全长度：不依赖 NUL 结尾，避免字段没被清零时 strlen 越界读。
size_t blen(const char* p, size_t cap) {
    size_t n = 0;
    while (n < cap && p[n] != '\0') {
        ++n;
    }
    return n;
}

// enum -> 名字。⚠️ 刻意不用 map[e]，那会往 map 里插空串掩盖问题。
std::string exchName(ExchangeType e) {
    auto it = ExchangeTypeEnum2StrMap.find(e);
    if (it == ExchangeTypeEnum2StrMap.end() || it->second.empty()) {
        return "<unmapped:" + std::to_string(static_cast<int>(e)) + ">";
    }
    return it->second;
}

std::string instTypeName(InstType t) {
    auto it = InstTypeEnum2StrMap.find(t);
    if (it == InstTypeEnum2StrMap.end() || it->second.empty()) {
        return "<unmapped:" + std::to_string(static_cast<int>(t)) + ">";
    }
    return it->second;
}

// md::InstrumentInfo -> icrule::Row（规则层不依赖 data_struct.h）
Row toRow(const md::InstrumentInfo& i) {
    Row r;
    r.instId.assign(i.instId, blen(i.instId, sizeof(i.instId)));
    r.originInstId.assign(i.originInstId, blen(i.originInstId, sizeof(i.originInstId)));
    r.base.assign(i.base, blen(i.base, sizeof(i.base)));
    r.quote.assign(i.quote, blen(i.quote, sizeof(i.quote)));
    r.margin.assign(i.margin, blen(i.margin, sizeof(i.margin)));

    r.capInstId = sizeof(i.instId);
    r.capOrigin = sizeof(i.originInstId);
    r.capBase   = sizeof(i.base);
    r.capQuote  = sizeof(i.quote);
    r.capMargin = sizeof(i.margin);

    r.instType = instTypeName(i.instTypeEnum);
    r.exchange = exchName(i.exchangeTypeEnum);
    r.isSpot   = (i.instTypeEnum == SPOT);

    r.value         = i.value;
    r.tickSize      = i.tickSize;
    r.lotSize       = i.lotSize;
    r.minSize       = i.minSize;
    r.maxSize       = i.maxSize;
    r.minAmount     = i.minAmount;
    r.magnifyNumber = i.magnifyNumber;
    r.reduceNumber  = i.reduceNumber;

    r.priceDigits = i.priceDigits;
    r.sizeDigits  = i.sizeDigits;
    r.pricePow10  = i.pricePow10;
    r.sizePow10   = i.sizePow10;
    r.tickSizeInt = i.tickSizeInt;
    r.lotSizeInt  = i.lotSizeInt;
    return r;
}

// ============================================================================
// 注册表检查（全局跑一次）
// ============================================================================

template <typename EnumT>
void checkEnumMap(Suite& S, size_t idx,
                  const std::unordered_map<EnumT, std::string>& e2s,
                  const std::unordered_map<std::string, EnumT>& s2e,
                  const char* label) {
    Check& c = S[idx];
    c.note += std::string(label) + " enum2str=" + std::to_string(e2s.size()) +
              " str2enum=" + std::to_string(s2e.size());

    c.seen();
    if (e2s.size() != s2e.size()) {
        c.fail(std::string(label) + " 两张表条数不一致: enum2str=" +
               std::to_string(e2s.size()) + " str2enum=" + std::to_string(s2e.size()));
    }

    std::unordered_set<std::string> seenVal;
    int maxKey = -1;
    for (const auto& kv : e2s) {
        int k = static_cast<int>(kv.first);
        if (k > maxKey) maxKey = k;

        c.seen();
        if (kv.second.empty()) {
            c.fail(std::string(label) + " 枚举值 " + std::to_string(k) + " 映射到空串");
            continue;
        }
        c.seen();
        if (!seenVal.insert(kv.second).second) {
            c.fail(std::string(label) + " 名字重复: '" + kv.second + "' 被多个枚举值共用");
        }
        auto rit = s2e.find(kv.second);
        c.seen();
        if (rit == s2e.end()) {
            c.fail(std::string(label) + " str2enum 缺 '" + kv.second + "'");
        }
        else if (static_cast<int>(rit->second) != k) {
            c.fail(std::string(label) + " 往返不一致: " + std::to_string(k) + " -> '" +
                   kv.second + "' -> " + std::to_string(static_cast<int>(rit->second)));
        }
    }

    // 编号空洞：enum 是从 0 连续定义的，中间缺一个 key 说明有人加了枚举却没加映射。
    // ⚠️ 盲区：如果漏的是**最后一个**枚举值，这里看不出来（maxKey 也变小了）。
    //    那种情况靠 A1（该所拉不到数据）暴露。
    for (int k = 0; k <= maxKey; ++k) {
        c.seen();
        if (e2s.find(static_cast<EnumT>(k)) == e2s.end()) {
            c.fail(std::string(label) + " 编号空洞: 枚举值 " + std::to_string(k) + " 没有映射");
        }
    }
}

// ============================================================================
// 跨所检查（全部交易所跑完之后一次）
// ============================================================================

void checkCrossVenue(const std::vector<std::pair<std::string, std::vector<md::InstrumentInfo>>>& all,
                     Suite& S) {
    // key = "BASE|QUOTE|INSTTYPE"
    struct Agg {
        std::vector<std::pair<std::string, double>> value;  // (exchange, value)
        std::vector<std::pair<std::string, double>> tick;
    };
    std::map<std::string, Agg> groups;

    for (const auto& ex : all) {
        for (const auto& i : ex.second) {
            std::string b(i.base, blen(i.base, sizeof(i.base)));
            std::string q(i.quote, blen(i.quote, sizeof(i.quote)));
            if (b.empty() || q.empty()) {
                continue;
            }
            std::string key = b + "|" + q + "|" + instTypeName(i.instTypeEnum);
            auto& g = groups[key];
            g.value.emplace_back(ex.first, i.value);
            g.tick.emplace_back(ex.first, i.tickSize);
        }
    }

    for (const auto& kv : groups) {
        const Agg& g = kv.second;

        // F1: 面值比值应是 10 的整数次幂（"跨所面值差 100 倍"是常态，差 11 倍是 bug）
        if (g.value.size() >= 2) {
            double mn = g.value[0].second, mx = mn;
            for (const auto& p : g.value) {
                if (p.second < mn) mn = p.second;
                if (p.second > mx) mx = p.second;
            }
            S[F1_XVALUE].seen();
            if (mn > 0.0 && mx > 0.0) {
                double ratio = mx / mn;
                double lg = std::log10(ratio);
                double dev = std::fabs(lg - std::round(lg));
                if (dev > 0.02) {   // 偏离 10^k 超过约 5%
                    std::string s = kv.first + " 面值比 " + num(ratio) + " 不是 10 的整数次幂:";
                    for (const auto& p : g.value) {
                        s += " " + p.first + "=" + num(p.second);
                    }
                    S[F1_XVALUE].fail(s);
                }
            }
        }

        // F2: tick 数量级差异（>1000 倍）值得看一眼
        if (g.tick.size() >= 2) {
            double mn = 0, mx = 0;
            bool first = true;
            for (const auto& p : g.tick) {
                if (!(p.second > 0.0)) continue;
                if (first) { mn = mx = p.second; first = false; }
                else {
                    if (p.second < mn) mn = p.second;
                    if (p.second > mx) mx = p.second;
                }
            }
            S[F2_XTICK].seen();
            if (mn > 0.0 && mx / mn > 1000.0) {
                std::string s = kv.first + " tick 相差 " + num(mx / mn) + " 倍:";
                for (const auto& p : g.tick) {
                    s += " " + p.first + "=" + num(p.second);
                }
                S[F2_XTICK].fail(s);
            }
        }
    }

    S[F1_XVALUE].note = std::to_string(groups.size()) + " 个 (base,quote,instType) 分组";
}

// ============================================================================
// db 配置交叉检查
// ============================================================================

// 读 db/etc/small_swap_db.json，把每个交易所块里的 instId 拿来在 contractinfo 的结果里查。
// 查不到 = db 起不来这个 unit，而且 db 侧只会打一行 "cannot find originInstId"。
void checkDbConfig(const std::string& path,
                   const std::vector<std::pair<std::string, std::vector<md::InstrumentInfo>>>& all,
                   const std::unordered_set<std::string>& scope,
                   Suite& S) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        S[G1_DBCFG].seen();
        S[G1_DBCFG].fail("打不开 " + path);
        return;
    }
    std::string body;
    {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
            body.append(buf, n);
        }
        std::fclose(fp);
    }

    simdjson::dom::parser parser;
    simdjson::padded_string padded(body);
    auto doc = parser.parse(padded);
    if (doc.error()) {
        S[G1_DBCFG].seen();
        S[G1_DBCFG].fail(std::string("解析失败: ") + simdjson::error_message(doc.error()));
        return;
    }
    simdjson::dom::object root;
    if (doc.get_object().get(root) != simdjson::SUCCESS) {
        S[G1_DBCFG].seen();
        S[G1_DBCFG].fail("顶层不是 object");
        return;
    }

    // 每家交易所的 instId / originInstId 全名表
    std::unordered_map<std::string, std::unordered_set<std::string>> known;
    for (const auto& ex : all) {
        auto& set = known[ex.first];
        set.reserve(ex.second.size() * 2 + 8);
        for (const auto& i : ex.second) {
            set.insert(std::string(i.instId, blen(i.instId, sizeof(i.instId))));
            set.insert(std::string(i.originInstId, blen(i.originInstId, sizeof(i.originInstId))));
        }
    }

    int checkedExch = 0, checkedInst = 0;
    for (auto field : root) {
        std::string name(field.key);
        if (name == "tag" || name == "log" || name == "redis") {
            continue;
        }
        // 只检查本次实际跑了的交易所 —— 否则 --exchange BITUNIX 会把其他 17 家
        // 全报成"contractinfo 里完全没有数据"，纯属假报警。
        if (scope.find(name) == scope.end()) {
            continue;
        }
        simdjson::dom::object eo;
        if (field.value.get_object().get(eo) != simdjson::SUCCESS) {
            continue;
        }
        simdjson::dom::array ids;
        if (!jf::array_at(eo, "instId", ids)) {
            continue;
        }
        ++checkedExch;

        auto kit = known.find(name);
        for (auto v : ids) {
            std::string_view sv;
            if (v.get_string().get(sv) != simdjson::SUCCESS) {
                continue;
            }
            ++checkedInst;
            S[G1_DBCFG].seen();
            if (kit == known.end()) {
                S[G1_DBCFG].fail("db 配置里的 " + name + " 在 contractinfo 里完全没有数据");
                break;
            }
            if (kit->second.find(std::string(sv)) == kit->second.end()) {
                S[G1_DBCFG].fail(name + " 的配置对子 '" + std::string(sv) +
                                 "' 在 contractinfo 里查不到（instId / originInstId 都不匹配）");
            }
        }
    }
    S[G1_DBCFG].note = std::to_string(checkedExch) + " 家 / " + std::to_string(checkedInst) + " 个对子";
}

// ============================================================================
// 快照 JSON 输出
// ============================================================================

void jsonStr(std::string& out, const char* s, size_t cap) {
    out += '"';
    size_t n = blen(s, cap);
    for (size_t k = 0; k < n; ++k) {
        char c = s[k];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        }
        else if (static_cast<unsigned char>(c) < 0x20) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c));
            out += b;
        }
        else {
            out += c;
        }
    }
    out += '"';
}

void jsonStr(std::string& out, const std::string& s) {
    jsonStr(out, s.c_str(), s.size());
}

void emitSuite(std::string& out, const std::vector<CheckDef>& defs, const Suite& S) {
    out += '[';
    for (size_t k = 0; k < defs.size(); ++k) {
        if (k) out += ',';
        out += "{\"id\":";
        jsonStr(out, std::string(defs[k].id));
        out += ",\"sev\":";
        jsonStr(out, std::string(sevName(defs[k].sev)));
        out += ",\"bad\":" + std::to_string(S[k].bad);
        out += ",\"total\":" + std::to_string(S[k].total);
        out += '}';
    }
    out += ']';
}

void emitInstrument(std::string& out, const md::InstrumentInfo& i) {
    char b[64];
    out += '{';
    out += "\"instType\":";
    jsonStr(out, instTypeName(i.instTypeEnum));
    out += ",\"origin\":";
    jsonStr(out, i.originInstId, sizeof(i.originInstId));
    out += ",\"base\":";
    jsonStr(out, i.base, sizeof(i.base));
    out += ",\"quote\":";
    jsonStr(out, i.quote, sizeof(i.quote));
    out += ",\"margin\":";
    jsonStr(out, i.margin, sizeof(i.margin));

#define EMIT_D(field)                                   \
    std::snprintf(b, sizeof(b), "%.12g", i.field);      \
    out += ",\"" #field "\":";                          \
    out += b;
#define EMIT_I(field)                                   \
    std::snprintf(b, sizeof(b), "%lld",                 \
                  static_cast<long long>(i.field));     \
    out += ",\"" #field "\":";                          \
    out += b;

    EMIT_D(value)
    EMIT_D(tickSize)
    EMIT_D(lotSize)
    EMIT_D(minSize)
    EMIT_D(maxSize)
    EMIT_D(minAmount)
    EMIT_D(magnifyNumber)
    EMIT_D(reduceNumber)
    EMIT_I(priceDigits)
    EMIT_I(sizeDigits)
    EMIT_I(pricePow10)
    EMIT_I(sizePow10)
    EMIT_I(tickSizeInt)
    EMIT_I(lotSizeInt)
#undef EMIT_D
#undef EMIT_I
    out += '}';
}

// ============================================================================
// 快照 JSON 读入（--from-json / --diff）
// ============================================================================

struct Snap {
    std::string instType;
    std::string origin, base, quote, margin;
    double value = 0, tick = 0, lot = 0, minSize = 0, maxSize = 0, minAmount = 0;
    double magnify = 0, reduce = 0;
    int64_t pd = 0, sd = 0, px10 = 0, sz10 = 0, tickInt = 0, lotInt = 0;
};

using ExchSnap = std::map<std::string, Snap>;   // instId -> Snap

bool loadSnapshot(const std::string& path,
                  std::map<std::string, ExchSnap>& out,
                  std::string& err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        err = "打不开 " + path;
        return false;
    }
    std::string body;
    {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
            body.append(buf, n);
        }
        std::fclose(fp);
    }

    simdjson::dom::parser parser;
    simdjson::padded_string padded(body);
    auto doc = parser.parse(padded);
    if (doc.error()) {
        err = std::string("解析失败: ") + simdjson::error_message(doc.error());
        return false;
    }
    simdjson::dom::object root;
    if (doc.get_object().get(root) != simdjson::SUCCESS) {
        err = "顶层不是 object";
        return false;
    }
    simdjson::dom::object exchs;
    if (!jf::object_at(root, "exchanges", exchs)) {
        err = "缺 exchanges 段（不是 InfoCheck 生成的快照？）";
        return false;
    }

    for (auto ef : exchs) {
        std::string ename(ef.key);
        simdjson::dom::object eo;
        if (ef.value.get_object().get(eo) != simdjson::SUCCESS) {
            continue;
        }
        simdjson::dom::object ins;
        if (!jf::object_at(eo, "instruments", ins)) {
            continue;
        }
        ExchSnap& dst = out[ename];
        for (auto f : ins) {
            std::string iid(f.key);
            simdjson::dom::object io;
            if (f.value.get_object().get(io) != simdjson::SUCCESS) {
                continue;
            }
            Snap s;
            s.instType = jf::str_or(io, {"instType"});
            s.origin   = jf::str_or(io, {"origin"});
            s.base     = jf::str_or(io, {"base"});
            s.quote    = jf::str_or(io, {"quote"});
            s.margin   = jf::str_or(io, {"margin"});
            jf::dbl(io, {"value"}, s.value);
            jf::dbl(io, {"tickSize"}, s.tick);
            jf::dbl(io, {"lotSize"}, s.lot);
            jf::dbl(io, {"minSize"}, s.minSize);
            jf::dbl(io, {"maxSize"}, s.maxSize);
            jf::dbl(io, {"minAmount"}, s.minAmount);
            jf::dbl(io, {"magnifyNumber"}, s.magnify);
            jf::dbl(io, {"reduceNumber"}, s.reduce);
            jf::i64(io, {"priceDigits"}, s.pd);
            jf::i64(io, {"sizeDigits"}, s.sd);
            jf::i64(io, {"pricePow10"}, s.px10);
            jf::i64(io, {"sizePow10"}, s.sz10);
            jf::i64(io, {"tickSizeInt"}, s.tickInt);
            jf::i64(io, {"lotSizeInt"}, s.lotInt);
            dst[iid] = std::move(s);
        }
    }
    return true;
}

// 把快照还原成 InstrumentInfo，供 --from-json 复用全部检查逻辑。
bool snapToInfo(const std::string& exch, const std::string& iid, const Snap& s,
                md::InstrumentInfo& out) {
    std::memset(&out, 0, sizeof(out));

    auto exIt = ExchangeTypeStr2EnumMap.find(exch);
    if (exIt == ExchangeTypeStr2EnumMap.end()) {
        return false;
    }
    auto itIt = InstTypeStr2EnumMap.find(s.instType);
    if (itIt == InstTypeStr2EnumMap.end()) {
        return false;
    }
    out.exchangeTypeEnum = exIt->second;
    out.instTypeEnum     = itIt->second;

    auto cp = [](char* dst, size_t cap, const std::string& src) {
        std::snprintf(dst, cap, "%s", src.c_str());
    };
    cp(out.instId, sizeof(out.instId), iid);
    cp(out.originInstId, sizeof(out.originInstId), s.origin);
    cp(out.base, sizeof(out.base), s.base);
    cp(out.quote, sizeof(out.quote), s.quote);
    cp(out.margin, sizeof(out.margin), s.margin);

    out.value         = s.value;
    out.tickSize      = s.tick;
    out.lotSize       = s.lot;
    out.minSize       = s.minSize;
    out.maxSize       = s.maxSize;
    out.minAmount     = s.minAmount;
    out.magnifyNumber = s.magnify;
    out.reduceNumber  = s.reduce;
    out.priceDigits   = static_cast<int>(s.pd);
    out.sizeDigits    = static_cast<int>(s.sd);
    out.pricePow10    = s.px10;
    out.sizePow10     = s.sz10;
    out.tickSizeInt   = s.tickInt;
    out.lotSizeInt    = s.lotInt;
    return true;
}

// ============================================================================
// 报告输出
// ============================================================================

struct Totals {
    int pass = 0, fail = 0, warn = 0, info = 0;
    int worst = -1;   // 最严重的级别
    void add(const CheckDef& d, bool ok) {
        if (ok) {
            ++pass;
        }
        else if (d.sev == SEV_WARN) {
            ++warn;
        }
        else if (d.sev == SEV_INFO) {
            ++info;
        }
        else {
            ++fail;
        }
        if (!ok && (worst < 0 || static_cast<int>(d.sev) < worst)) {
            worst = static_cast<int>(d.sev);
        }
    }
};

Totals printSuite(const std::string& header, const std::vector<CheckDef>& defs,
                  const Suite& S, bool quietInfo) {
    if (!header.empty()) {
        std::printf("\n%s\n", header.c_str());
    }
    Totals t;
    for (size_t k = 0; k < defs.size(); ++k) {
        const Check& c = S[k];
        const bool ok = c.ok();
        t.add(defs[k], ok);
        if (quietInfo && ok && defs[k].sev == SEV_INFO) {
            continue;
        }
        std::printf("  [%s] %-3s %-11s %-4s",
                    ok ? "PASS" : "FAIL",
                    defs[k].id,
                    defs[k].title,
                    ok ? "" : sevName(defs[k].sev));
        if (!ok) {
            std::printf(" %d/%d 条\n", c.bad, c.total);
            for (const auto& s : c.samples) {
                std::printf("        %s\n", s.c_str());
            }
            if (c.bad > static_cast<int>(c.samples.size())) {
                std::printf("        ... 还有 %d 条\n", c.bad - static_cast<int>(c.samples.size()));
            }
        }
        else if (!c.note.empty()) {
            std::printf(" %s\n", c.note.c_str());
        }
        else if (c.total > 0) {
            std::printf(" %d 条\n", c.total);
        }
        else {
            std::printf("\n");
        }
    }
    return t;
}

void printInstrument(const md::InstrumentInfo& i) {
    std::printf("    %-22s origin=%-20s %-5s/%-5s margin=%-5s\n",
                i.instId, i.originInstId, i.base, i.quote, i.margin);
    std::printf("        value=%s tick=%s (d=%d pow10=%s int=%s)  lot=%s (d=%d pow10=%s int=%s)\n",
                num(i.value).c_str(),
                num(i.tickSize).c_str(), i.priceDigits, num(i.pricePow10).c_str(),
                num(i.tickSizeInt).c_str(),
                num(i.lotSize).c_str(), i.sizeDigits, num(i.sizePow10).c_str(),
                num(i.lotSizeInt).c_str());
    std::printf("        minSize=%s maxSize=%s minAmount=%s magnify=%s reduce=%s calcType=%d\n",
                num(i.minSize).c_str(), num(i.maxSize).c_str(), num(i.minAmount).c_str(),
                num(i.magnifyNumber).c_str(), num(i.reduceNumber).c_str(), i.calcType);
}

// ============================================================================
// 参数
// ============================================================================

struct Opts {
    std::string config     = "etc/contract_info.json";
    std::string dbConfig;                       // 空 = 不做 G1
    std::string logDir     = "logs";
    std::string outPath;                        // --out
    std::string diffPath;                       // --diff
    std::string fromJson;                       // --from-json
    std::vector<std::string> exchanges;         // 空 = 用 config 的 exchangeList
    bool json              = false;
    bool strict            = false;
    bool quietInfo         = false;
    int  sample            = 0;
};

void usage(const char* prog) {
    std::fprintf(stderr,
        "用法: %s [选项]\n"
        "\n"
        "  --config <path>       contractinfo 配置，默认 etc/contract_info.json\n"
        "  --exchange <A,B>      只跑这些交易所（逗号分隔，可重复）\n"
        "  --sample <N>          每家最多打印 N 条明细（默认 0）\n"
        "  --json                机器可读输出\n"
        "  --out <path>          把快照写成 JSON 文件（可用作 --diff 基线）\n"
        "  --diff <path>         与基线快照比对\n"
        "  --from-json <path>    不联网，直接对快照跑全部检查\n"
        "  --db-config <path>    额外检查 db 的 small_swap_db.json 能否查到\n"
        "  --log-dir <path>      解析器日志目录，默认 logs\n"
        "  --strict              WARN 也算失败\n"
        "  --quiet-info          不打印通过的 INFO 项\n"
        "  -h, --help\n", prog);
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool parseArgs(int argc, char** argv, Opts& o) {
    auto next = [&](int& i, const char* opt) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "缺少 %s 的值\n", opt);
            return nullptr;
        }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config")         { auto v = next(i, "--config");    if (!v) return false; o.config = v; }
        else if (a == "--db-config") { auto v = next(i, "--db-config"); if (!v) return false; o.dbConfig = v; }
        else if (a == "--log-dir")   { auto v = next(i, "--log-dir");   if (!v) return false; o.logDir = v; }
        else if (a == "--out")       { auto v = next(i, "--out");       if (!v) return false; o.outPath = v; }
        else if (a == "--diff")      { auto v = next(i, "--diff");      if (!v) return false; o.diffPath = v; }
        else if (a == "--from-json") { auto v = next(i, "--from-json"); if (!v) return false; o.fromJson = v; }
        else if (a == "--sample")    { auto v = next(i, "--sample");    if (!v) return false; o.sample = std::atoi(v); }
        else if (a == "--exchange") {
            auto v = next(i, "--exchange");
            if (!v) return false;
            std::string s = v;
            size_t p = 0;
            while (p <= s.size()) {
                size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                std::string one = trim(s.substr(p, q - p));
                if (!one.empty()) o.exchanges.push_back(one);
                p = q + 1;
            }
        }
        else if (a == "--json")       { o.json = true; }
        else if (a == "--strict")     { o.strict = true; }
        else if (a == "--quiet-info") { o.quietInfo = true; }
        else if (a == "-h" || a == "--help") { usage(argv[0]); return false; }
        else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

bool fileExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

}  // namespace

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
    Opts o;
    if (!parseArgs(argc, argv, o)) {
        return 2;
    }

    // 解析器内部的 LOG_ERROR 走 fmtlog。工具必须自己初始化，否则失败路径的行为
    // 未定义 —— 而失败路径恰恰是我们最想看到日志的地方。
    std::string logDir = o.logDir;
    if (!crypto::create_directory(logDir)) {
        logDir = ".";
    }
    log_maintain("InfoCheck", logDir, "debug");

    // ---------------- 1. 配置 ----------------
    Suite reg(kRegDefs);

    if (!fileExists(o.config)) {
        std::fprintf(stderr, "配置文件不存在: %s\n", o.config.c_str());
        return 2;
    }
    // Config::load() 内部 exit(-1)，所以只能事先检查存在性。
    Config* cfg = Config::instance();
    cfg->load(o.config.c_str());

    // BaseInfo 构造函数会 get_string("needMagnifyNum") 然后 stoi —— 缺 key 就抛
    // std::invalid_argument，进程直接 terminate。所以先查。
    std::string magnifyStr;
    int needMagnifyNum = 1;
    reg[R2_CONFIG].seen();
    if (!cfg->get_string("needMagnifyNum", magnifyStr)) {
        reg[R2_CONFIG].fail("配置缺 needMagnifyNum（BaseInfo 构造会抛 std::invalid_argument）");
    }
    else {
        try {
            needMagnifyNum = std::stoi(magnifyStr);
        }
        catch (...) {
            reg[R2_CONFIG].fail("needMagnifyNum='" + magnifyStr + "' 不是整数");
        }
    }
    reg[R2_CONFIG].note = "needMagnifyNum=" + std::to_string(needMagnifyNum);

    // 枚举表
    checkEnumMap<ExchangeType>(reg, R1_ENUMMAP, ExchangeTypeEnum2StrMap, ExchangeTypeStr2EnumMap, "ExchangeType");
    {
        // InstType 与 ExchangeType 合并在同一个 R1 里报（避免刷屏），失败样本会带前缀
        Suite tmp(kRegDefs);
        checkEnumMap<InstType>(tmp, R1_ENUMMAP, InstTypeEnum2StrMap, InstTypeStr2EnumMap, "InstType");
        for (auto& s : tmp[R1_ENUMMAP].samples) {
            reg[R1_ENUMMAP].fail(s);
        }
        reg[R1_ENUMMAP].total += tmp[R1_ENUMMAP].total;
        reg[R1_ENUMMAP].note += "  " + tmp[R1_ENUMMAP].note;
    }

    // 要跑哪些交易所
    std::vector<std::string> targets = o.exchanges;
    if (targets.empty()) {
        std::string k = "exchangeList";
        if (!cfg->has_exchange(k)) {
            reg[R3_SUPPORTED].seen();
            reg[R3_SUPPORTED].fail("配置缺 exchangeList");
        }
        else {
            std::vector<std::string> list;
            cfg->get_exchange_list("exchangeList", list);
            targets = list;
        }
    }

    // R3: 每个名字都要能创建
    for (const auto& name : targets) {
        reg[R3_SUPPORTED].seen();
        if (!exchfac::isSupported(name)) {
            reg[R3_SUPPORTED].fail("'" + name + "' 不在交易所注册表里（拼写错误，或还没实现）");
        }
    }
    reg[R3_SUPPORTED].note = std::to_string(exchfac::supported().size()) + " 家已实现";

    // ---------------- 2. 取数据 + 跑逐条规则 ----------------
    struct ExchData {
        std::string name;
        std::vector<md::InstrumentInfo> rows;
        Suite s;
        explicit ExchData(const std::vector<CheckDef>& d) : s(d) {}
    };
    std::vector<ExchData> data;
    data.reserve(targets.size());

    std::map<std::string, ExchSnap> baseline;   // --diff / --from-json 用
    bool haveBaseline = false;

    const std::string snapPath = !o.fromJson.empty() ? o.fromJson : o.diffPath;
    if (!snapPath.empty()) {
        std::string err;
        if (!loadSnapshot(snapPath, baseline, err)) {
            std::fprintf(stderr, "读快照失败 (%s): %s\n", snapPath.c_str(), err.c_str());
            return 2;
        }
        haveBaseline = true;
    }

    const bool offline = !o.fromJson.empty();

    for (const auto& name : targets) {
        if (!exchfac::isSupported(name)) {
            continue;   // R3 已经报过了
        }
        ExchData ed(kRowDefs);
        ed.name = name;

        if (offline) {
            auto it = baseline.find(name);
            if (it == baseline.end()) {
                ed.s[A1_FETCH].seen();
                ed.s[A1_FETCH].fail("快照里没有这家交易所");
            }
            else {
                ed.rows.reserve(it->second.size());
                for (const auto& kv : it->second) {
                    md::InstrumentInfo info;
                    if (snapToInfo(name, kv.first, kv.second, info)) {
                        ed.rows.push_back(info);
                    }
                }
            }
        }
        else {
            std::printf("[%s] 拉取中 ...\n", name.c_str());
            std::fflush(stdout);
            std::unique_ptr<BaseInfo> info(exchfac::create(name, nullptr, cfg));
            if (!info) {
                ed.s[A1_FETCH].seen();
                ed.s[A1_FETCH].fail("create 失败");
            }
            else {
                info->syncExchangeInfo();
                fmtlog::poll();   // 把解析器的日志刷出去，别丢
                auto& m = info->getInstrumentInfo();
                ed.rows.reserve(m.size());
                for (auto& kv : m) {
                    ed.rows.push_back(kv.second);
                }
            }
        }

        {
            RowRunner runner(ed.s, needMagnifyNum, name);
            for (const auto& i : ed.rows) {
                runner.add(toRow(i));
            }
            runner.finish();
        }
        if (needMagnifyNum == 0) {
            ed.s[E3_BASEPFX].note = "needMagnifyNum=0，按设计不剥离 base 数字前缀，跳过";
        }
        data.push_back(std::move(ed));
    }

    // ---------------- 3. 跨所 + db 配置 ----------------
    std::vector<std::pair<std::string, std::vector<md::InstrumentInfo>>> view;
    view.reserve(data.size());
    for (auto& d : data) {
        view.emplace_back(d.name, d.rows);
    }

    Suite cross(kCrossDefs);
    if (view.size() >= 2) {
        checkCrossVenue(view, cross);
    }

    Suite dbc(kDbDefs);
    std::string dbPath = o.dbConfig;
    if (dbPath.empty() && fileExists("../db/etc/small_swap_db.json")) {
        dbPath = "../db/etc/small_swap_db.json";
    }
    if (!dbPath.empty() && !offline) {
        std::unordered_set<std::string> scope(targets.begin(), targets.end());
        checkDbConfig(dbPath, view, scope, dbc);
    }

    // ---------------- 4. 与基线比对 ----------------
    // 延迟到"打印报告"时才算，否则明细会跑到总标题上面去。
    Suite diff(kDiffDefs);
    auto runDiff = [&]() {
        if (!haveBaseline || !o.fromJson.empty()) {
            return;   // --from-json 时基线就是数据源，不自己跟自己比
        }
        // diff 的明细本身是多行的、结构化的，单独打印比塞进 Check 的样本列表清楚。
        // H1 只负责计数（比较了多少条、变了多少条），供汇总和退出码使用。
        std::printf("\n---- 与基线比对 (%s) ----\n", snapPath.c_str());
        std::string summary;

        for (auto& d : data) {
            auto bit = baseline.find(d.name);
            if (bit == baseline.end()) {
                std::printf("  %-10s 基线里没有这家交易所（新增）\n", d.name.c_str());
                diff[H1_DIFF].seen();
                diff[H1_DIFF].count(1);
                summary += d.name + " 新增; ";
                continue;
            }
            ExchSnap cur;
            for (const auto& i : d.rows) {
                Snap s;
                s.instType = instTypeName(i.instTypeEnum);
                s.origin.assign(i.originInstId, blen(i.originInstId, sizeof(i.originInstId)));
                s.tick    = i.tickSize;
                s.lot     = i.lotSize;
                s.value   = i.value;
                s.magnify = i.magnifyNumber;
                s.minSize = i.minSize;
                s.maxSize = i.maxSize;
                cur[std::string(i.instId, blen(i.instId, sizeof(i.instId)))] = std::move(s);
            }

            const ExchSnap& base = bit->second;
            int delisted = 0, listed = 0, changed = 0;

            for (const auto& kv : base) {
                diff[H1_DIFF].seen();
                if (cur.find(kv.first) == cur.end()) {
                    ++delisted;
                    diff[H1_DIFF].count(1);
                    std::printf("  - 下架  %-10s %-22s (origin=%s)\n",
                                d.name.c_str(), kv.first.c_str(), kv.second.origin.c_str());
                }
            }
            for (const auto& kv : cur) {
                auto oit = base.find(kv.first);
                if (oit == base.end()) {
                    ++listed;
                    diff[H1_DIFF].seen();
                    diff[H1_DIFF].count(1);
                    std::printf("  + 新增  %-10s %-22s\n", d.name.c_str(), kv.first.c_str());
                    continue;
                }
                const Snap& b = oit->second;
                const Snap& c = kv.second;
                auto chg = [&](const char* field, double bv, double cv) {
                    if (!nearly(bv, cv, 1e-9)) {
                        ++changed;
                        diff[H1_DIFF].count(1);
                        std::printf("  ~ %-9s %-10s %-22s %s -> %s\n",
                                    field, d.name.c_str(), kv.first.c_str(),
                                    num(bv).c_str(), num(cv).c_str());
                    }
                };
                chg("tickSize", b.tick, c.tick);
                chg("lotSize", b.lot, c.lot);
                chg("value", b.value, c.value);
                chg("magnify", b.magnify, c.magnify);
                chg("minSize", b.minSize, c.minSize);
                chg("maxSize", b.maxSize, c.maxSize);
            }

            std::printf("  %-10s %zu -> %zu 条", d.name.c_str(), base.size(), cur.size());
            if (delisted || listed || changed) {
                std::printf("   下架 %d / 新增 %d / 变更 %d", delisted, listed, changed);
            }
            else {
                std::printf("   无变化");
            }
            std::printf("\n");

            summary += d.name + " " + std::to_string(base.size()) + "->" +
                       std::to_string(cur.size()) + " (下架" + std::to_string(delisted) +
                       "/新增" + std::to_string(listed) + "/变更" + std::to_string(changed) + "); ";
        }
        diff[H1_DIFF].note = summary;
    };

    // ---------------- 5. 报告 ----------------
    std::string json;
    if (o.json || !o.outPath.empty()) {
        json += "{\n  \"tool\":\"InfoCheck\",\n  \"version\":1,\n";
        json += "  \"generated_at\":" + std::to_string(static_cast<long long>(std::time(nullptr))) + ",\n";
        json += "  \"config\":";
        jsonStr(json, o.config);
        json += ",\n  \"needMagnifyNum\":" + std::to_string(needMagnifyNum) + ",\n";
        json += "  \"registry\":{\"checks\":";
        emitSuite(json, kRegDefs, reg);
        json += "},\n  \"exchanges\":{\n";

        bool firstEx = true;
        for (auto& d : data) {
            if (!firstEx) json += ",\n";
            firstEx = false;

            std::map<std::string, int> byType;
            for (const auto& i : d.rows) {
                byType[instTypeName(i.instTypeEnum)]++;
            }
            json += "    ";
            jsonStr(json, d.name);
            json += ":{\"count\":" + std::to_string(d.rows.size()) + ",\"by_instType\":{";
            bool f2 = true;
            for (const auto& kv : byType) {
                if (!f2) json += ',';
                f2 = false;
                jsonStr(json, kv.first);
                json += ":" + std::to_string(kv.second);
            }
            json += "},\n     \"checks\":";
            emitSuite(json, kRowDefs, d.s);
            json += ",\n     \"instruments\":{";
            bool f3 = true;
            for (const auto& i : d.rows) {
                if (!f3) json += ',';
                f3 = false;
                json += "\n       ";
                jsonStr(json, std::string(i.instId, blen(i.instId, sizeof(i.instId))));
                json += ":";
                emitInstrument(json, i);
            }
            json += "\n     }}";
        }
        json += "\n  }\n}\n";

        if (!o.outPath.empty()) {
            FILE* fp = std::fopen(o.outPath.c_str(), "wb");
            if (!fp) {
                std::fprintf(stderr, "写不了 %s\n", o.outPath.c_str());
            }
            else {
                std::fwrite(json.data(), 1, json.size(), fp);
                std::fclose(fp);
            }
        }
    }

    if (o.json) {
        std::fputs(json.c_str(), stdout);
        return 0;
    }

    std::printf("\n================ contractinfo InfoCheck ================\n");
    std::printf("config      : %s\n", o.config.c_str());
    std::printf("needMagnify : %d\n", needMagnifyNum);
    std::printf("mode        : %s\n", offline ? "from-json (离线)" : "live");
    if (!dbPath.empty() && !offline) {
        std::printf("db config   : %s\n", dbPath.c_str());
    }
    std::printf("log dir     : %s\n", logDir.c_str());

    Totals all;
    auto merge = [&all](const Totals& t) {
        all.pass += t.pass;
        all.fail += t.fail;
        all.warn += t.warn;
        all.info += t.info;
        if (t.worst >= 0 && (all.worst < 0 || t.worst < all.worst)) {
            all.worst = t.worst;
        }
    };

    merge(printSuite("---- 注册表 ----", kRegDefs, reg, o.quietInfo));

    for (auto& d : data) {
        std::string hdr = "---- " + d.name + " ----";
        merge(printSuite(hdr, kRowDefs, d.s, o.quietInfo));

        if (o.sample > 0 && !d.rows.empty()) {
            std::printf("\n  明细（前 %d 条）:\n",
                        std::min<int>(o.sample, static_cast<int>(d.rows.size())));
            for (int k = 0; k < o.sample && k < static_cast<int>(d.rows.size()); ++k) {
                printInstrument(d.rows[k]);
            }
        }
    }

    if (view.size() >= 2) {
        merge(printSuite("---- 跨所一致性 ----", kCrossDefs, cross, o.quietInfo));
    }
    if (!dbPath.empty() && !offline) {
        merge(printSuite("---- db 配置交叉检查 ----", kDbDefs, dbc, o.quietInfo));
    }
    if (haveBaseline && o.fromJson.empty()) {
        runDiff();   // 先打印逐条明细，再让 H1 出汇总行
        merge(printSuite("---- 与基线比对（汇总） ----", kDiffDefs, diff, o.quietInfo));
    }

    std::printf("\n========================================================\n");
    std::printf("PASS %d   FAIL %d   WARN %d   INFO %d\n", all.pass, all.fail, all.warn, all.info);
    if (!o.outPath.empty()) {
        std::printf("快照已写入: %s\n", o.outPath.c_str());
    }
    std::printf("解析器日志: %s/InfoCheck_*.log\n", logDir.c_str());

    fmtlog::poll();

    if (all.fail > 0) {
        std::printf("结论: 不通过（有 FATAL/ERROR）\n");
        return 1;
    }
    if (all.warn > 0) {
        std::printf("结论: %s（有 %d 项 WARN）\n", o.strict ? "不通过" : "通过，但有 WARN", all.warn);
        return o.strict ? 3 : 0;
    }
    std::printf("结论: 通过\n");
    return 0;
}
