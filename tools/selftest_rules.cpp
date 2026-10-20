// tools/selftest_rules.cpp
// ============================================================================
// InfoCheckRules.h 的自测：**不需要编译整个 contractinfo**，本机就能跑。
//
// 为什么要有它:
//   InfoCheck 的价值全在那些断言上。断言本身错了 —— 对合法数据误报，或者对错数据
//   漏报 —— 比没有工具更糟：要么没人敢信它，要么放心地放一个错的交易所上线。
//   而 contractinfo 在本机（macOS）编不了（缺 boost/rapidjson/simdjson/fmt），
//   所以把规则抽成纯 std 的头文件，在这里单独验。
//
// 验证两件事:
//   1. 合法数据（含各家真实出现过的精度组合）→ 所有检查必须 0 命中
//   2. 每条规则各造一个"最小破坏" → 对应检查必须命中
//
// 编译运行（本机即可）:
//   /usr/bin/clang++ -std=c++17 -O1 -Wall -Wextra -I../include selftest_rules.cpp \
//       -o /tmp/selftest_rules && /tmp/selftest_rules
//
// 退出码: 0 全过；1 有不符合预期的用例。
// ============================================================================

#include "InfoCheckRules.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace icrule;

namespace {

int g_pass = 0;
int g_fail = 0;

// 一条"完全正确"的记录：BTC-USDT 永续，tick 0.1，1 张 = 1 BTC
Row good() {
    Row r;
    r.instId       = "BTC-USDT";
    r.originInstId = "BTCUSDT";
    r.base         = "BTC";
    r.quote        = "USDT";
    r.margin       = "USDT";
    r.instType     = "USDT_SWAP";
    r.exchange     = "BITUNIX";
    r.isSpot       = false;

    r.value         = 1.0;
    r.tickSize      = 0.1;
    r.priceDigits   = 1;
    r.pricePow10    = 10;
    r.tickSizeInt   = 1;
    r.lotSize       = 1.0;
    r.sizeDigits    = 0;
    r.sizePow10     = 1;
    r.lotSizeInt    = 1;

    r.minSize       = 1.0;
    r.maxSize       = 1200.0;
    r.minAmount     = 0.0;
    r.magnifyNumber = 1.0;
    r.reduceNumber  = 1.0;
    return r;
}

bool fired(const Suite& s, RowCk c) {
    return s[c].bad > 0;
}

void reportFired(const Suite& s) {
    for (size_t k = 0; k < kRowDefs.size(); ++k) {
        if (s[k].bad == 0) continue;
        std::printf("        命中 %-3s %-11s %d 条: %s\n",
                    kRowDefs[k].id, kRowDefs[k].title, s[k].bad,
                    s[k].samples.empty() ? "" : s[k].samples[0].c_str());
    }
}

// 跑一组记录，要求 expect 里每一项都命中，且 requireClean 为真时其余全部干净。
void runCase(const char* name,
             const std::vector<Row>& rows,
             const std::vector<RowCk>& expect,
             int needMagnifyNum = 1,
             const std::string& exch = "BITUNIX",
             bool requireClean = false) {
    Suite s(kRowDefs);
    {
        RowRunner runner(s, needMagnifyNum, exch);
        for (const auto& r : rows) {
            runner.add(r);
        }
        runner.finish();
    }

    bool ok = true;
    std::string missing;
    for (RowCk c : expect) {
        if (!fired(s, c)) {
            ok = false;
            missing += std::string(kRowDefs[c].id) + " ";
        }
    }

    std::string extra;
    if (requireClean) {
        for (size_t k = 0; k < kRowDefs.size(); ++k) {
            if (s[k].bad == 0) continue;
            bool wanted = false;
            for (RowCk c : expect) {
                if (static_cast<size_t>(c) == k) { wanted = true; break; }
            }
            if (!wanted) {
                ok = false;
                extra += std::string(kRowDefs[k].id) + " ";
            }
        }
    }

    if (ok) {
        ++g_pass;
        std::printf("  [ OK ] %s\n", name);
    }
    else {
        ++g_fail;
        std::printf("  [FAIL] %s\n", name);
        if (!missing.empty()) {
            std::printf("        应该命中但没有: %s\n", missing.c_str());
        }
        if (!extra.empty()) {
            std::printf("        不该命中却命中了: %s\n", extra.c_str());
        }
        reportFired(s);
    }
}

}  // namespace

int main() {
    std::printf("=== InfoCheckRules 自测 ===\n\n");

    // ------------------------------------------------------------------
    // 1. 合法数据必须零命中
    // ------------------------------------------------------------------
    std::printf("-- 合法数据 --\n");

    {
        std::vector<Row> rows = {good()};
        runCase("单条 BTC-USDT 永续", rows, {}, 1, "BITUNIX", /*requireClean=*/true);
    }

    {
        // 各家真实出现过的精度组合，混在一起跑，确认没有互相干扰
        std::vector<Row> rows;

        Row a = good();                                   // BTC: tick 0.1, lot 1
        rows.push_back(a);

        Row b = good();                                   // DOGE(WEEX): tick 1e-5, lot 1
        b.instId = "DOGE-USDT"; b.originInstId = "DOGEUSDT"; b.base = "DOGE";
        b.tickSize = 0.00001; b.priceDigits = 5; b.pricePow10 = 100000; b.tickSizeInt = 1;
        b.lotSize = 1; b.sizeDigits = 0; b.sizePow10 = 1; b.lotSizeInt = 1;
        b.minSize = 100; b.maxSize = 2000;
        rows.push_back(b);

        Row c = good();                                   // 小数数量步长: lot 0.001
        c.instId = "ETH-USDT"; c.originInstId = "ETHUSDT"; c.base = "ETH";
        c.tickSize = 0.01; c.priceDigits = 2; c.pricePow10 = 100; c.tickSizeInt = 1;
        c.lotSize = 0.001; c.sizeDigits = 3; c.sizePow10 = 1000; c.lotSizeInt = 1;
        c.minSize = 0.001;
        rows.push_back(c);

        Row d = good();                                   // 币本位: margin = BTC, tick 0.5
        d.instId = "BTC-USD"; d.originInstId = "BTCUSD"; d.quote = "USD"; d.margin = "BTC";
        d.instType = "C_SWAP";
        d.tickSize = 0.5; d.priceDigits = 1; d.pricePow10 = 10; d.tickSizeInt = 5;
        d.lotSize = 1; d.sizeDigits = 0; d.sizePow10 = 1; d.lotSizeInt = 1;
        rows.push_back(d);

        Row e = good();                                   // 1000CHEEMS: base 已剥离, magnify=1000
        e.instId = "1000CHEEMS-USDT"; e.originInstId = "1000CHEEMSUSDT";
        e.base = "CHEEMS";
        e.value = 1000;
        e.tickSize = 0.0000001; e.priceDigits = 7; e.pricePow10 = 10000000; e.tickSizeInt = 1;
        e.lotSize = 1; e.sizeDigits = 0; e.sizePow10 = 1; e.lotSizeInt = 1;
        e.magnifyNumber = 1000; e.reduceNumber = 0.001;
        rows.push_back(e);

        Row f = good();                                   // 现货: margin 可以空
        f.instId = "SOL-USDT"; f.originInstId = "SOLUSDT"; f.base = "SOL";
        f.instType = "SPOT"; f.margin = ""; f.isSpot = true;
        f.tickSize = 0.001; f.priceDigits = 3; f.pricePow10 = 1000; f.tickSizeInt = 1;
        f.lotSize = 0.01; f.sizeDigits = 2; f.sizePow10 = 100; f.lotSizeInt = 1;
        f.minSize = 0.01;
        rows.push_back(f);

        runCase("六家精度组合混合", rows, {}, 1, "BITUNIX", /*requireClean=*/true);
    }

    {
        // needMagnifyNum=0 时 base 保留数字前缀是**设计行为**，不能报 E3
        Row r = good();
        r.base = "1000CHEEMS";
        r.magnifyNumber = 1.0;
        r.reduceNumber = 1.0;
        std::vector<Row> rows = {r};
        runCase("needMagnifyNum=0 保留数字前缀不报警", rows, {}, 0, "BITUNIX", /*requireClean=*/true);
    }

    // ------------------------------------------------------------------
    // 2. 每条规则各造一个最小破坏
    // ------------------------------------------------------------------
    std::printf("\n-- 单点破坏 --\n");

    {   // A1: 空列表
        Suite s(kRowDefs);
        RowRunner runner(s, 1, "BITUNIX");
        runner.finish();
        if (s[A1_FETCH].bad == 1) { ++g_pass; std::printf("  [ OK ] A1 空列表\n"); }
        else { ++g_fail; std::printf("  [FAIL] A1 空列表\n"); }
    }

    { Row r = good(); r.instId = "";            runCase("B1 instId 空", {r}, {B1_INSTID}); }
    { Row r = good(); r.originInstId = "";      runCase("B2 origin 空", {r}, {B2_ORIGIN}); }
    { Row r = good(); r.base = "";              runCase("B3 base 空", {r}, {B3_BASE}); }
    { Row r = good(); r.quote = "";             runCase("B4 quote 空", {r}, {B4_QUOTE}); }
    { Row r = good(); r.quote = "BTC";          runCase("B5 base==quote", {r}, {B5_DISTINCT}); }
    { Row r = good(); r.margin = "";            runCase("B6 衍生品 margin 空", {r}, {B6_MARGIN}); }

    {   // B7: 写满 char[32]
        Row r = good();
        r.instId = std::string(32, 'X');
        runCase("B7 instId 写满 char[32]", {r}, {B7_CAPCHAR});
    }
    {   // B7 反例：31 字符应该通过
        Row r = good();
        r.instId = std::string(31, 'X');
        runCase("B7 31 字符不应报（边界）", {r}, {}, 1, "BITUNIX", /*requireClean=*/true);
    }

    { Row r = good(); r.instId = " BTC-USDT";   runCase("B8 instId 带空格", {r}, {B8_WHITESPACE}); }

    {   // B9: 两条同 instId
        Row a = good();
        Row b = good();
        b.originInstId = "OTHER";
        runCase("B9 instId 重复", {a, b}, {B9_UNIQUE});
    }

    {   // B10: 复制粘贴忘改交易所
        Row r = good();
        r.exchange = "HOTCOIN";
        runCase("B10 exchangeTypeEnum 填成别家", {r}, {B10_ENUMMATCH});
    }

    {   // C1: digits 与 pow10 不一致
        Row r = good();
        r.priceDigits = 2;   // pow10 仍是 10
        runCase("C1 pow10 != 10^digits", {r}, {C1_POW10});
    }

    {   // C2: tickSizeInt 算错
        Row r = good();
        r.tickSizeInt = 100;
        runCase("C2 tickSizeInt 算错", {r}, {C2_TICKINT, C6_PXROUND});
    }

    {   // C3: lotSizeInt 算错
        Row r = good();
        r.lotSizeInt = 1000;
        runCase("C3 lotSizeInt 算错", {r}, {C3_LOTINT});
    }

    { Row r = good(); r.tickSize = 0;           runCase("C4 tickSize=0", {r}, {C4_TICKPOS}); }
    { Row r = good(); r.lotSize = 0;            runCase("C5 lotSize=0", {r}, {C5_LOTPOS}); }

    {   // ⭐ 最典型的生产事故：把「小数位数」当成「步长」，tickSizeInt 差了 1000 倍
        //    实际数据: tick=0.001, digits=3, pow10=1000, 但 tickSizeInt 被写成 100
        //    => quantize(0.001) = llround(0.001*1000)=1, ticks=1/100=0, => 0
        Row r = good();
        r.tickSize = 0.001; r.priceDigits = 3; r.pricePow10 = 1000; r.tickSizeInt = 100;
        runCase("C6 quantize(tick) 被截成 0", {r}, {C2_TICKINT, C6_PXROUND});
    }

    {   // C7: 数量侧同样的事故
        Row r = good();
        r.lotSize = 0.01; r.sizeDigits = 2; r.sizePow10 = 100; r.lotSizeInt = 10;
        runCase("C7 quantize(lot) 被截成 0", {r}, {C3_LOTINT, C7_SZROUND});
    }

    {   // C8: tick 不是 pow10 网格的整数倍（0.3/10 = 3 其实合法；用 0.15 造非法）
        Row r = good();
        r.tickSize = 0.15; r.priceDigits = 2; r.pricePow10 = 100; r.tickSizeInt = 15;
        // 0.15 在 pow10=100 下是 15 个最小单位，quantize 仍能精确表示 => 不该报
        runCase("C8 tick 为最小单位整数倍时不报", {r}, {}, 1, "BITUNIX", /*requireClean=*/true);
    }

    { Row r = good(); r.minSize = 2000; r.maxSize = 1200; runCase("D1 minSize>maxSize", {r}, {D1_MINMAX}); }
    { Row r = good(); r.minSize = 0;            runCase("D2 minSize=0（提示级）", {r}, {D2_MINPOS}); }
    { Row r = good(); r.minAmount = -1;         runCase("D3 minAmount<0", {r}, {D3_AMOUNT}); }
    { Row r = good(); r.value = 0;              runCase("E1 value=0", {r}, {E1_VALUE}); }

    {   // E2: magnify * reduce != 1
        Row r = good();
        r.magnifyNumber = 2; r.reduceNumber = 0.4;
        runCase("E2 magnify*reduce != 1", {r}, {E2_MAGNIFY});
    }

    {   // E3: base 有 1000 前缀但 magnify 没跟着
        Row r = good();
        r.base = "1000CHEEMS";
        runCase("E3 base 前缀与 magnify 不一致", {r}, {E3_BASEPFX});
    }

    // ------------------------------------------------------------------
    std::printf("\n=== 合计 %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
