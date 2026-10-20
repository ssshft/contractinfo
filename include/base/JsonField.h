#pragma once

// ============================================================================
// 新增交易所（bitunix / phemex / weex / fameex / blofin / hotcoin / echobit / mgbx）
// 专用的 JSON 取值助手 —— 用 simdjson **DOM**，不用 ondemand。
//
// 为什么不用仓库里既有的 ondemand 写法:
//   ondemand 是**前向单向**的，字段必须按 JSON 文档里的出现顺序访问，否则
//   get() 静默失败（拿到默认值），最终表现为"这家所一条合约都没有"。
//   已有 10 家是照着抓包顺序一个个数字段位置写死的（见 XtInfo.cpp 里的注释编号），
//   那套写法在"能抓到真实响应"时是对的，但新增交易所里有一半**抓不到**
//   （blofin 451 地域封锁 / hotcoin "API未开放" / echobit、mgbx 主机不可达），
//   顺序无从核对。照着文档猜顺序 = 埋一颗静默零数据的雷。
//
//   contractinfo 是 60 秒跑一次的离线任务，DOM 的解析开销（微秒级）完全不重要，
//   换来的是"顺序无关 + 可以按别名列表容错"。
//
// 用法:
//   simdjson::dom::parser parser;
//   simdjson::padded_string padded(body);
//   auto doc = parser.parse(padded);
//   if (doc.error()) { ... }
//   std::string sym;
//   if (jf::str(row, {"symbol", "instId", "contractCode"}, sym)) { ... }
// ============================================================================

#include <simdjson.h>

#include <initializer_list>
#include <string>
#include <string_view>

namespace jf {

using obj = simdjson::dom::object;

// 按别名顺序找第一个**存在**的字段；找到返回指针，全都没有返回 nullptr。
//   ⚠️ 只判"字段存在"，不判类型 —— 取值的类型转换在下面四个函数里做。
inline const simdjson::dom::element* find(const obj& o, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        if (!k || !k[0]) {
            continue;
        }
        auto v = o.at_key(std::string_view(k));
        if (!v.error()) {
            return &v.value();
        }
    }
    return nullptr;
}

inline bool str(const obj& o, std::initializer_list<const char*> keys, std::string& out) {
    const simdjson::dom::element* e = find(o, keys);
    if (!e) {
        return false;
    }
    std::string_view sv;
    if (e->get_string().get(sv) != simdjson::SUCCESS) {
        return false;
    }
    out.assign(sv.data(), sv.size());
    return true;
}

// 数字字段在交易所之间既可能是 0.001 也可能是 "0.001"，这里两种都吃。
inline bool dbl(const obj& o, std::initializer_list<const char*> keys, double& out) {
    const simdjson::dom::element* e = find(o, keys);
    if (!e) {
        return false;
    }
    double d = 0.0;
    if (e->get_double().get(d) == simdjson::SUCCESS) {
        out = d;
        return true;
    }
    std::string_view sv;
    if (e->get_string().get(sv) == simdjson::SUCCESS && !sv.empty()) {
        out = crypto::fast_atod(sv);
        return true;
    }
    int64_t i = 0;
    if (e->get_int64().get(i) == simdjson::SUCCESS) {
        out = static_cast<double>(i);
        return true;
    }
    return false;
}

inline bool i64(const obj& o, std::initializer_list<const char*> keys, int64_t& out) {
    const simdjson::dom::element* e = find(o, keys);
    if (!e) {
        return false;
    }
    int64_t i = 0;
    if (e->get_int64().get(i) == simdjson::SUCCESS) {
        out = i;
        return true;
    }
    double d = 0.0;
    if (e->get_double().get(d) == simdjson::SUCCESS) {
        out = static_cast<int64_t>(d);
        return true;
    }
    std::string_view sv;
    if (e->get_string().get(sv) == simdjson::SUCCESS && !sv.empty()) {
        out = static_cast<int64_t>(crypto::fast_atod(sv));
        return true;
    }
    return false;
}

inline bool boolean(const obj& o, std::initializer_list<const char*> keys, bool& out) {
    const simdjson::dom::element* e = find(o, keys);
    if (!e) {
        return false;
    }
    bool b = false;
    if (e->get_bool().get(b) == simdjson::SUCCESS) {
        out = b;
        return true;
    }
    int64_t i = 0;
    if (e->get_int64().get(i) == simdjson::SUCCESS) {
        out = (i != 0);
        return true;
    }
    return false;
}

// 取子数组 / 子对象。
// ⚠️ 这里刻意**不用 `object::operator[]`** —— 那个下标运算符在 simdjson 的不同版本里
//    有的只挂在 element 上，编译期才发现；`at_key` 是 object 的稳定接口。
inline bool array_at(const obj& o, std::string_view key, simdjson::dom::array& out) {
    auto v = o.at_key(key);
    if (v.error()) {
        return false;
    }
    return v.value().get_array().get(out) == simdjson::SUCCESS;
}

inline bool object_at(const obj& o, std::string_view key, simdjson::dom::object& out) {
    auto v = o.at_key(key);
    if (v.error()) {
        return false;
    }
    return v.value().get_object().get(out) == simdjson::SUCCESS;
}

// 取不到就用兜底值；用于"可选字段"，避免每处都写 if。
inline std::string str_or(const obj& o, std::initializer_list<const char*> keys, const char* def = "") {
    std::string s;
    return str(o, keys, s) ? s : std::string(def);
}

// 从各种"外壳"里把合约数组挖出来。给**未实测**的三家（hotcoin / echobit / mgbx）用：
// 抓不到响应，就没法确认它是 {"data":[...]} 还是 {"data":{"list":[...]}} 还是顶层数组。
//   root 是顶层 element，依次尝试:
//     顶层数组 / data / result / rows / list / contracts / symbols
//     data.list / data.rows / data.records / data.items / result.list / result.rows
// 返回 true 时 out 有效。**找到空数组也算成功**（说明外壳对了，是筛选条件的问题）。
inline bool pick_array(const simdjson::dom::element& root, simdjson::dom::array& out) {
    if (root.get_array().get(out) == simdjson::SUCCESS) {
        return true;
    }
    simdjson::dom::object o;
    if (root.get_object().get(o) != simdjson::SUCCESS) {
        return false;
    }
    static const char* kTop[] = {"data", "result", "rows", "list", "contracts", "symbols"};
    for (const char* k : kTop) {
        auto v = o.at_key(std::string_view(k));
        if (v.error()) {
            continue;
        }
        if (v.value().get_array().get(out) == simdjson::SUCCESS) {
            return true;
        }
        simdjson::dom::object inner;
        if (v.value().get_object().get(inner) == simdjson::SUCCESS) {
            static const char* kSub[] = {"list", "rows", "records", "items", "contracts"};
            for (const char* sk : kSub) {
                auto iv = inner.at_key(std::string_view(sk));
                if (!iv.error() && iv.value().get_array().get(out) == simdjson::SUCCESS) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace jf
