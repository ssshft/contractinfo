#pragma once

// ============================================================================
// InfoCheck 的**逐条规则**（不含 IO、不含报告格式、不依赖整个工程）
//
// 为什么单独抽出来:
//   这些断言是这个工具的全部价值。如果它们本身有 bug —— 比如对合法数据误报、
//   或者对错数据漏报 —— 那这个工具比没有更糟: 要么让人不敢信，要么让人放心地
//   上线一个错的交易所。
//
//   抽出来之后，tools/selftest_rules.cpp 可以**不编译整个 contractinfo** 就跑一遍
//   "已知正确 → 必须全 PASS" 和 "已知错误 → 必须精确命中某条检查"，本机（macOS，
//   没有 boost/simdjson/rapidjson/fmt）也能验证。
//
//   依赖只有 precision_util.h（纯 std）。这里刻意不复用 md::InstrumentInfo ——
//   那个结构体拖进 rapidjson/fmt/boost，等于把自测也锁死在部署机上。
//   InfoCheck.cpp 负责把 InstrumentInfo 翻译成 Row。
//
// 用法:
//   icrule::Suite s(icrule::kRowDefs);
//   icrule::RowRunner runner(s, needMagnifyNum, "BITUNIX");
//   for (const auto& i : rows) { runner.add(toRow(i)); }
//   runner.finish();
//   // s[icrule::C6_PXROUND].ok() / .bad / .samples
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "precision_util.h"   // crypto::quantize / crypto::POW10_INT

namespace icrule {

// ---------------------------------------------------------------------------
// 框架
// ---------------------------------------------------------------------------

constexpr int kMaxSamples = 4;   // 每项检查最多记几条失败样本，够定位就行
constexpr int kMaxDigits  = 18;  // crypto::POW10_INT 的有效下标上限

enum Sev { SEV_FATAL = 0, SEV_ERROR, SEV_WARN, SEV_INFO };

inline const char* sevName(Sev s) {
    switch (s) {
        case SEV_FATAL: return "FATAL";
        case SEV_ERROR: return "ERROR";
        case SEV_WARN:  return "WARN";
        default:        return "INFO";
    }
}

struct CheckDef {
    const char* id;
    const char* title;
    Sev         sev;
};

struct Check {
    int total = 0;    // 参与检查的条目数
    int bad   = 0;    // 不通过的条目数
    std::string note; // PASS 时的补充说明（条数分布之类）
    std::vector<std::string> samples;

    void seen() { ++total; }
    void seenMany(int n) { total += n; }

    void fail(std::string s) {
        ++bad;
        if (static_cast<int>(samples.size()) < kMaxSamples) {
            samples.push_back(std::move(s));
        }
    }

    // 只计数、不留样本。用于"明细已经单独打印过"的检查（如 diff），
    // 避免同一份内容在报告里出现两遍。
    void count(int n) { bad += n; }

    bool ok() const { return bad == 0; }
};

// 一组检查。下标与对应的 CheckDef 表一一对应。
struct Suite {
    std::vector<Check> ck;
    explicit Suite(const std::vector<CheckDef>& defs) : ck(defs.size()) {}
    Check& operator[](size_t i) { return ck[i]; }
    const Check& operator[](size_t i) const { return ck[i]; }
};

// ---------------------------------------------------------------------------
// 逐条检查的编号与定义
// ---------------------------------------------------------------------------

enum RowCk {
    A1_FETCH,
    B1_INSTID, B2_ORIGIN, B3_BASE, B4_QUOTE, B5_DISTINCT,
    B6_MARGIN, B7_CAPCHAR, B8_WHITESPACE, B9_UNIQUE, B10_ENUMMATCH,
    C1_POW10, C2_TICKINT, C3_LOTINT, C4_TICKPOS, C5_LOTPOS,
    C6_PXROUND, C7_SZROUND, C8_PXGRID,
    D1_MINMAX, D2_MINPOS, D3_AMOUNT,
    E1_VALUE, E2_MAGNIFY, E3_BASEPFX,
    A_N
};

inline const std::vector<CheckDef> kRowDefs = {
    {"A1",  "fetch",      SEV_FATAL},
    {"B1",  "instId",     SEV_ERROR},
    {"B2",  "origin",     SEV_ERROR},
    {"B3",  "base",       SEV_ERROR},
    {"B4",  "quote",      SEV_ERROR},
    {"B5",  "distinct",   SEV_WARN},
    {"B6",  "margin",     SEV_WARN},
    {"B7",  "capchar",    SEV_ERROR},
    {"B8",  "whitespace", SEV_ERROR},
    {"B9",  "unique",     SEV_ERROR},
    {"B10", "enumMatch",  SEV_FATAL},
    {"C1",  "pow10",      SEV_ERROR},
    {"C2",  "tickInt",    SEV_ERROR},
    {"C3",  "lotInt",     SEV_ERROR},
    {"C4",  "tickPos",    SEV_FATAL},
    {"C5",  "lotPos",     SEV_FATAL},
    {"C6",  "pxRound",    SEV_FATAL},
    {"C7",  "szRound",    SEV_FATAL},
    {"C8",  "pxGrid",     SEV_ERROR},
    {"D1",  "minMax",     SEV_ERROR},
    // minSize==0 在很多交易所是合法的（"没有最小量限制"），下游用 lotSize 兜底即可。
    // 所以只做提示，不算失败 —— 否则第一次跑就会满屏 WARN，把真正的问题淹掉。
    {"D2",  "minPos",     SEV_INFO},
    {"D3",  "amount",     SEV_WARN},
    {"E1",  "value",      SEV_ERROR},
    {"E2",  "magnify",    SEV_ERROR},
    {"E3",  "basePfx",    SEV_WARN},
};

// ---------------------------------------------------------------------------
// 记录（md::InstrumentInfo 的检查用投影）
// ---------------------------------------------------------------------------

struct Row {
    std::string instId, originInstId, base, quote, margin;

    // 定长 char[] 的容量。等于容量说明字段被写满 —— strncpy 不保证 NUL 结尾，
    // 下游 strcpy 会越界读。默认值与 md::InstrumentInfo 一致。
    size_t capInstId = 32, capOrigin = 32, capBase = 16, capQuote = 16, capMargin = 16;

    std::string instType;   // instType 的名字，只用于样本文案
    std::string exchange;   // 实际写进 exchangeTypeEnum 的交易所名（B10 用）
    bool isSpot = false;    // 现货允许 margin 为空

    double value = 0, tickSize = 0, lotSize = 0;
    double minSize = 0, maxSize = 0, minAmount = 0;
    double magnifyNumber = 0, reduceNumber = 0;

    int     priceDigits = 0, sizeDigits = 0;
    int64_t pricePow10 = 0, sizePow10 = 0;
    int64_t tickSizeInt = 0, lotSizeInt = 0;
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

inline std::string num(double v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.10g", v);
    return b;
}

inline std::string num(int64_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(v));
    return b;
}

inline bool nearly(double a, double b, double rel) {
    double d = std::fabs(a - b);
    double m = std::max(std::fabs(a), std::fabs(b));
    if (m < 1e-12) {
        return d < 1e-12;
    }
    return d <= rel * m;
}

inline bool hasSpace(const std::string& s) {
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 逐条跑规则
//
// 累积式（而不是纯函数）是因为 A1 要看总数、B9 要跨行去重。
// ---------------------------------------------------------------------------

class RowRunner {
public:
    RowRunner(Suite& s, int needMagnifyNum, std::string expectExch)
        : S_(s), needMagnify_(needMagnifyNum), expectExch_(std::move(expectExch)) {}

    void add(const Row& r) {
        const std::string& iid = r.instId;
        byType_[r.instType]++;

        // ---- B 组：标识字段 ----
        S_[B1_INSTID].seen();
        if (iid.empty()) S_[B1_INSTID].fail("instId 为空 (origin=" + r.originInstId + ")");

        // ⭐ 新增交易所最常见的复制粘贴事故：从别家文件抄过来，忘了改
        //    info.exchangeTypeEnum = XXX。后果是数据被挂到另一家交易所名下，
        //    db 按自己的名字查不到，而 contractinfo 侧一切正常、零报错。
        S_[B10_ENUMMATCH].seen();
        if (r.exchange != expectExch_) {
            S_[B10_ENUMMATCH].fail(iid + " exchangeTypeEnum=" + r.exchange +
                                   " 但正在拉取的是 " + expectExch_);
        }

        S_[B2_ORIGIN].seen();
        if (r.originInstId.empty()) S_[B2_ORIGIN].fail("originInstId 为空 (instId=" + iid + ")");

        S_[B3_BASE].seen();
        if (r.base.empty()) S_[B3_BASE].fail(iid + " base 为空");

        S_[B4_QUOTE].seen();
        if (r.quote.empty()) S_[B4_QUOTE].fail(iid + " quote 为空");

        S_[B5_DISTINCT].seen();
        if (!r.base.empty() && r.base == r.quote) {
            S_[B5_DISTINCT].fail(iid + " base == quote == " + r.base);
        }

        // 衍生品必须有 margin（结算币）；现货可以空。
        S_[B6_MARGIN].seen();
        if (r.margin.empty() && !r.isSpot) {
            S_[B6_MARGIN].fail(iid + " (" + r.instType + ") margin 为空");
        }

        // 长度上限：等于容量说明写满了
        S_[B7_CAPCHAR].seen();
        if (r.instId.size() >= r.capInstId || r.originInstId.size() >= r.capOrigin ||
            r.base.size() >= r.capBase || r.quote.size() >= r.capQuote ||
            r.margin.size() >= r.capMargin) {
            S_[B7_CAPCHAR].fail(iid + " 字段写满 char[]（instId " + std::to_string(r.instId.size()) +
                                "/" + std::to_string(r.capInstId) + "  origin " +
                                std::to_string(r.originInstId.size()) + "/" +
                                std::to_string(r.capOrigin) + "）");
        }

        S_[B8_WHITESPACE].seen();
        if (hasSpace(r.instId) || hasSpace(r.originInstId) ||
            hasSpace(r.base) || hasSpace(r.quote)) {
            S_[B8_WHITESPACE].fail("[" + iid + "] 标识字段含空白字符（db 的 symbol 匹配会全失败）");
        }

        S_[B9_UNIQUE].seen();
        if (!iid.empty() && !seenInst_.insert(r.instType + "|" + iid).second) {
            S_[B9_UNIQUE].fail(r.instType + " 下 instId 重复: " + iid);
        }
        if (!r.originInstId.empty() &&
            !seenOrigin_.insert(r.instType + "|" + r.originInstId).second) {
            S_[B9_UNIQUE].fail(r.instType + " 下 originInstId 重复: " + r.originInstId);
        }

        // ---- C 组：精度四件套 ----
        S_[C1_POW10].seen();
        if (r.priceDigits < 0 || r.priceDigits > kMaxDigits) {
            S_[C1_POW10].fail(iid + " priceDigits=" + std::to_string(r.priceDigits) + " 越界");
        }
        else if (crypto::POW10_INT[r.priceDigits] != r.pricePow10) {
            S_[C1_POW10].fail(iid + " priceDigits=" + std::to_string(r.priceDigits) +
                              " 但 pricePow10=" + num(r.pricePow10) + "（应为 " +
                              num(crypto::POW10_INT[r.priceDigits]) + "）");
        }

        S_[C2_TICKINT].seen();
        if (r.pricePow10 <= 0) {
            S_[C2_TICKINT].fail(iid + " pricePow10<=0");
        }
        else {
            int64_t expect = static_cast<int64_t>(
                std::llround(r.tickSize * static_cast<double>(r.pricePow10)));
            if (expect <= 0) {
                S_[C2_TICKINT].fail(iid + " tickSize=" + num(r.tickSize) +
                                    " 太小，tickSize*pricePow10 舍入为 0");
            }
            else if (expect != r.tickSizeInt) {
                S_[C2_TICKINT].fail(iid + " tickSizeInt=" + num(r.tickSizeInt) +
                                    " 但 tickSize*pricePow10=" + num(expect));
            }
        }

        S_[C3_LOTINT].seen();
        if (r.sizePow10 <= 0) {
            S_[C3_LOTINT].fail(iid + " sizePow10<=0");
        }
        else {
            int64_t expect = static_cast<int64_t>(
                std::llround(r.lotSize * static_cast<double>(r.sizePow10)));
            if (expect <= 0) {
                S_[C3_LOTINT].fail(iid + " lotSize=" + num(r.lotSize) +
                                   " 太小，lotSize*sizePow10 舍入为 0");
            }
            else if (expect != r.lotSizeInt) {
                S_[C3_LOTINT].fail(iid + " lotSizeInt=" + num(r.lotSizeInt) +
                                   " 但 lotSize*sizePow10=" + num(expect));
            }
        }

        S_[C4_TICKPOS].seen();
        if (!(r.tickSize > 0.0)) S_[C4_TICKPOS].fail(iid + " tickSize=" + num(r.tickSize));

        S_[C5_LOTPOS].seen();
        if (!(r.lotSize > 0.0)) S_[C5_LOTPOS].fail(iid + " lotSize=" + num(r.lotSize));

        // ⭐ 核心：quantize(tick) 必须回到 tick 本身。返回 0 就是"挂单价被截成 0"。
        S_[C6_PXROUND].seen();
        {
            double q = crypto::quantize(r.tickSize, r.pricePow10, r.tickSizeInt);
            if (!(q > 0.0)) {
                S_[C6_PXROUND].fail(iid + " quantize(tick=" + num(r.tickSize) + ") = " + num(q) +
                                    "  ← 挂单价会被截成 0  (pow10=" + num(r.pricePow10) +
                                    " tickInt=" + num(r.tickSizeInt) + ")");
            }
            else if (!nearly(q, r.tickSize, 1e-9)) {
                S_[C6_PXROUND].fail(iid + " quantize(tick=" + num(r.tickSize) + ") = " + num(q) +
                                    "  ≠ tick  (pow10=" + num(r.pricePow10) +
                                    " tickInt=" + num(r.tickSizeInt) + ")");
            }
        }

        S_[C7_SZROUND].seen();
        {
            double q = crypto::quantize(r.lotSize, r.sizePow10, r.lotSizeInt);
            if (!(q > 0.0)) {
                S_[C7_SZROUND].fail(iid + " quantize(lot=" + num(r.lotSize) + ") = " + num(q) +
                                    "  ← 下单量会被截成 0");
            }
            else if (!nearly(q, r.lotSize, 1e-9)) {
                S_[C7_SZROUND].fail(iid + " quantize(lot=" + num(r.lotSize) + ") = " +
                                    num(q) + " ≠ lot");
            }
        }

        // 网格点必须落在 tick 的整数倍上（k=1/7/12345 覆盖小、中、大三种量级）
        S_[C8_PXGRID].seen();
        {
            static const double kMul[] = {1.0, 7.0, 12345.0};
            for (double m : kMul) {
                double p = r.tickSize * m;
                double q = crypto::quantize(p, r.pricePow10, r.tickSizeInt);
                if (!(q > 0.0)) {
                    S_[C8_PXGRID].fail(iid + " quantize(" + num(m) + "*tick=" + num(p) +
                                       ") = " + num(q));
                    break;
                }
                if (!nearly(q, p, 1e-9)) {
                    S_[C8_PXGRID].fail(iid + " quantize(" + num(m) + "*tick=" + num(p) +
                                       ") = " + num(q) + " 偏离网格");
                    break;
                }
            }
        }

        // ---- D 组：交易约束 ----
        S_[D1_MINMAX].seen();
        if (r.minSize > 0.0 && r.maxSize > 0.0 && r.minSize > r.maxSize) {
            S_[D1_MINMAX].fail(iid + " minSize=" + num(r.minSize) +
                               " > maxSize=" + num(r.maxSize));
        }

        S_[D2_MINPOS].seen();
        if (!(r.minSize > 0.0)) {
            S_[D2_MINPOS].fail(iid + " minSize=" + num(r.minSize) + "（下游需用 lotSize 兜底）");
        }

        S_[D3_AMOUNT].seen();
        if (r.minAmount < 0.0) {
            S_[D3_AMOUNT].fail(iid + " minAmount=" + num(r.minAmount) + " < 0");
        }

        // ---- E 组：面值与放大 ----
        S_[E1_VALUE].seen();
        if (!(r.value > 0.0)) {
            S_[E1_VALUE].fail(iid + " value=" + num(r.value) + "（面值必须 > 0）");
        }

        S_[E2_MAGNIFY].seen();
        if (!(r.magnifyNumber > 0.0) || !(r.reduceNumber > 0.0)) {
            S_[E2_MAGNIFY].fail(iid + " magnify=" + num(r.magnifyNumber) +
                                " reduce=" + num(r.reduceNumber) + "（两者都必须 > 0）");
        }
        else if (!nearly(r.magnifyNumber * r.reduceNumber, 1.0, 1e-9)) {
            S_[E2_MAGNIFY].fail(iid + " magnify=" + num(r.magnifyNumber) + " * reduce=" +
                                num(r.reduceNumber) + " = " +
                                num(r.magnifyNumber * r.reduceNumber) + " ≠ 1");
        }

        // base 带数字前缀（1000CHEEMS / 10000SATS）：
        //   needMagnifyNum!=0 时 getBaseMagnifyNum() 应该把前缀剥掉并令 magnify=该数字；
        //   needMagnifyNum==0 时按设计**不剥**，这里就不检查（否则是假报警）。
        if (needMagnify_ != 0) {
            size_t k = 0;
            while (k < r.base.size() && r.base[k] >= '0' && r.base[k] <= '9') ++k;
            if (k > 0 && k < r.base.size()) {
                double n = std::atof(r.base.substr(0, k).c_str());
                if (n > 100.0) {
                    S_[E3_BASEPFX].seen();
                    if (!nearly(r.magnifyNumber, n, 1e-9)) {
                        S_[E3_BASEPFX].fail(iid + " base=" + r.base + " 应剥离出 magnify=" +
                                            num(n) + "，实际 magnify=" + num(r.magnifyNumber));
                    }
                }
            }
        }
    }

    // 收尾：A1 的条数与 instType 分布
    void finish() {
        S_[A1_FETCH].seen();
        int total = 0;
        for (const auto& kv : byType_) {
            total += kv.second;
        }
        if (total == 0) {
            S_[A1_FETCH].fail("拉取结果为空 —— 接口/解析/过滤三者之一失败，见日志");
        }

        std::string dist;
        for (const auto& kv : byType_) {
            if (!dist.empty()) dist += " ";
            dist += kv.first + "=" + std::to_string(kv.second);
        }
        S_[A1_FETCH].note = std::to_string(total) + " 条" + (dist.empty() ? "" : "  (" + dist + ")");
    }

private:
    Suite& S_;
    int needMagnify_;
    std::string expectExch_;
    std::unordered_set<std::string> seenInst_;
    std::unordered_set<std::string> seenOrigin_;
    std::map<std::string, int> byType_;
};

}  // namespace icrule
