#pragma once

#include "data_struct.h"
#include "precision_util.h"   // crypto::POW10_INT / countDecimalDigits
#include "string_util.h"      // crypto::fast_atod

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string_view>

// ============================================================================
// contractinfo 各交易所共用的精度字段写入助手
//
// 为什么单独抽出来:
//   InstrumentInfo 上跟精度有关的是四个互相咬合的字段
//       tickSize / priceDigits / pricePow10 / tickSizeInt   (价格侧)
//       lotSize  / sizeDigits  / sizePow10  / lotSizeInt    (数量侧)
//   它们必须自洽 —— pricePow10 必须是 10^priceDigits, tickSizeInt 必须是
//   tickSize*pricePow10 的整数表示。任何一处算错, 下游 crypto::quantize() 会把
//   挂单价格截断成 0 或者差一个数量级, 而且**不会报错**。
//
//   这段逻辑在 binance/bybit/gateio/okx 里是各自复制粘贴的。新增 6 家时再复制 6 份,
//   出错概率随份数线性上升, 所以这里收敛成一组 inline 函数。
//   (已有 4 家没有回改, 避免本次改动面过大; 后续可以逐步替换。)
//
// 交易所给的精度字段形式只有三种, 对应下面三个入口:
//   1) 十进制字符串        "0.0001"                  -> priceFromDecimal / sizeFromDecimal
//   2) 整数步长 + 小数位数  priceEndStep=1, place=1   -> priceFromStep / sizeFromStep
//   3) JSON double        0.1 (htx price_tick)       -> priceFromDouble / sizeFromDouble
// ============================================================================
namespace infohelp {

constexpr int kMaxDigits = 18;

// "0.001" / "0.25000" / "1" -> tickSize 四件套
inline void priceFromDecimal(md::InstrumentInfo& info, std::string_view tick) {
    if (tick.empty()) {
        return;
    }
    info.tickSize = crypto::fast_atod(tick);
    int d = crypto::countDecimalDigits(tick);
    if (d > kMaxDigits) {
        d = kMaxDigits;
    }
    info.priceDigits = d;
    info.pricePow10 = crypto::POW10_INT[d];
    info.tickSizeInt = static_cast<int64_t>(std::llround(info.tickSize * static_cast<double>(info.pricePow10)));
    if (info.tickSizeInt <= 0) {
        info.tickSizeInt = 1;
    }
}

inline void sizeFromDecimal(md::InstrumentInfo& info, std::string_view lot) {
    if (lot.empty()) {
        return;
    }
    info.lotSize = crypto::fast_atod(lot);
    int d = crypto::countDecimalDigits(lot);
    if (d > kMaxDigits) {
        d = kMaxDigits;
    }
    info.sizeDigits = d;
    info.sizePow10 = crypto::POW10_INT[d];
    info.lotSizeInt = static_cast<int64_t>(std::llround(info.lotSize * static_cast<double>(info.sizePow10)));
    if (info.lotSizeInt <= 0) {
        info.lotSizeInt = 1;
    }
}

// ⚠️ 下面两个入口的差别是本次实现里最容易搞错的地方, 务必分清:
//
//   priceFromStep(step, digits)   tick = step / 10^digits
//       适用于交易所给的是"整数步长 + 小数位数"的情况:
//         bitget  priceEndStep=1, pricePlace=1  -> 0.1     (不是 1.0)
//         bitget  priceEndStep=1, pricePlace=9  -> 1e-9
//         bingx   pricePrecision=3              -> step 取 1, 得 0.001
//         xt      quantityPrecision=0           -> step 取 1, 得 1
//
//   priceFromFixed(value, digits)   tick = value, 只是按 digits 位小数格式化
//       适用于交易所给的值**本身就是 tick**:
//         mexc    priceUnit=1e-05, priceScale=5 -> 0.00001
//         mexc    priceUnit=0.1,   priceScale=1 -> 0.1
//       对 mexc 误用 priceFromStep 会再除一次 10^5, tick 变成 1e-10 (差 10 万倍)。
inline void priceFromStep(md::InstrumentInfo& info, double step, int digits) {
    if (!(step > 0.0)) {
        return;
    }
    if (digits < 0) {
        digits = 0;
    }
    if (digits > kMaxDigits) {
        digits = kMaxDigits;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, step / static_cast<double>(crypto::POW10_INT[digits]));
    priceFromDecimal(info, std::string_view(buf));
}

inline void sizeFromStep(md::InstrumentInfo& info, double step, int digits) {
    if (!(step > 0.0)) {
        return;
    }
    if (digits < 0) {
        digits = 0;
    }
    if (digits > kMaxDigits) {
        digits = kMaxDigits;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, step / static_cast<double>(crypto::POW10_INT[digits]));
    sizeFromDecimal(info, std::string_view(buf));
}

// 值本身就是 tick / 步长, 只按 digits 位小数规范化
inline void priceFromFixed(md::InstrumentInfo& info, double value, int digits) {
    if (!(value > 0.0)) {
        return;
    }
    if (digits < 0) {
        digits = 0;
    }
    if (digits > kMaxDigits) {
        digits = kMaxDigits;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, value);
    priceFromDecimal(info, std::string_view(buf));
}

inline void sizeFromFixed(md::InstrumentInfo& info, double value, int digits) {
    if (!(value > 0.0)) {
        return;
    }
    if (digits < 0) {
        digits = 0;
    }
    if (digits > kMaxDigits) {
        digits = kMaxDigits;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, value);
    sizeFromDecimal(info, std::string_view(buf));
}

// JSON 里直接给 double 的情况 (htx 的 price_tick / kucoin 的 tickSize)。
// 用 12 位小数打印再数有效位数: 0.1 -> "0.100000000000" -> 1 位。
// 比 1e-12 更细的 tick 会退化成 0, 这种交易所在本项目里不存在, 真遇到会打日志。
inline void priceFromDouble(md::InstrumentInfo& info, double tick) {
    if (!(tick > 0.0)) {
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.12f", tick);
    priceFromDecimal(info, std::string_view(buf));
}

inline void sizeFromDouble(md::InstrumentInfo& info, double lot) {
    if (!(lot > 0.0)) {
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.12f", lot);
    sizeFromDecimal(info, std::string_view(buf));
}

// 兜底: 一个精度字段都没拿到时, 至少保证四件套自洽 (pow10=1, tickInt=1),
// 否则下游 quantize() 的 stepInt<=0 分支会直接把原始价格原样透传。
inline void ensurePrecisionDefaults(md::InstrumentInfo& info) {
    if (info.pricePow10 <= 0) {
        info.priceDigits = 0;
        info.pricePow10 = 1;
        info.tickSizeInt = 1;
    }
    if (info.sizePow10 <= 0) {
        info.sizeDigits = 0;
        info.sizePow10 = 1;
        info.lotSizeInt = 1;
    }
}

} // namespace infohelp
