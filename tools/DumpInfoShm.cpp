// DumpInfoShm.cpp
//
// 只读工具: 打开 sm::shm 里的币对信息段, 打印 Header + entries。
// 运维/debug 用, 不依赖 Redis。
//
// 用法:
//   ./DumpInfoShm                             # 默认 /bts_instrument_info, 表格输出
//   ./DumpInfoShm --name /my_seg              # 指定 SHM 名字
//   ./DumpInfoShm --exchange BINANCE          # 只显示某交易所
//   ./DumpInfoShm --inst-type USDT_SWAP       # 只显示某 instType
//   ./DumpInfoShm --json                      # JSON 输出
//   ./DumpInfoShm --meta-only                 # 只打 Header, 不列条目
//   ./DumpInfoShm --limit 20                  # 最多打前 N 条

#include "InstrumentInfoShm.h"
#include "data_struct.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>


namespace {

struct Opts {
    std::string name          = sm::shm::kDefaultShmName;
    std::string filter_exch;      // 空 = 不过滤
    std::string filter_instType;  // 空 = 不过滤
    bool        json          = false;
    bool        meta_only     = false;
    int         limit         = -1;    // -1 = 全部
};

void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s [--name /bts_instrument_info] [--exchange EXCH] [--inst-type IT]\n"
        "           [--json] [--meta-only] [--limit N]\n", prog);
}

bool parse_args(int argc, char** argv, Opts& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* opt) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", opt);
                return nullptr;
            }
            return argv[++i];
        };
        if      (a == "--name")      { auto v = next("--name");      if (!v) return false; o.name = v; }
        else if (a == "--exchange")  { auto v = next("--exchange");  if (!v) return false; o.filter_exch = v; }
        else if (a == "--inst-type") { auto v = next("--inst-type"); if (!v) return false; o.filter_instType = v; }
        else if (a == "--limit")     { auto v = next("--limit");     if (!v) return false; o.limit = std::atoi(v); }
        else if (a == "--json")      { o.json = true; }
        else if (a == "--meta-only") { o.meta_only = true; }
        else if (a == "-h" || a == "--help") { usage(argv[0]); return false; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(argv[0]); return false; }
    }
    return true;
}

std::string age_str(uint64_t last_us) {
    if (last_us == 0) return "(never)";
    uint64_t now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    if (now_us <= last_us) return "0s ago";
    uint64_t d = now_us - last_us;
    if (d < 1'000'000ULL)        return std::to_string(d / 1'000ULL) + "ms ago";
    if (d < 60'000'000ULL)       return std::to_string(d / 1'000'000ULL) + "s ago";
    if (d < 3'600'000'000ULL)    return std::to_string(d / 60'000'000ULL) + "m ago";
    return std::to_string(d / 3'600'000'000ULL) + "h ago";
}

void print_header(const sm::shm::Reader& r) {
    std::printf("SHM Header:\n");
    std::printf("  count      : %u\n", r.count());
    std::printf("  capacity   : %u  (usage %.1f%%)\n",
                r.capacity(),
                r.capacity() > 0 ? 100.0 * r.count() / r.capacity() : 0.0);
    std::printf("  generation : %llu\n", static_cast<unsigned long long>(r.generation()));
    std::printf("  last_update: %llu us  (%s)\n",
                static_cast<unsigned long long>(r.last_update_us()),
                age_str(r.last_update_us()).c_str());
}

void print_table_row(const md::InstrumentInfo& info) {
    std::printf("%-10s %-14s %-24s %-24s %-8s %-8s %-8s %12.8f %12.8f %14.4f %14.4f %14.4f\n",
                ExchangeTypeEnum2StrMap[info.exchangeTypeEnum].c_str(),
                InstTypeEnum2StrMap[info.instTypeEnum].c_str(),
                info.instId,
                info.originInstId,
                info.base,
                info.quote,
                info.margin,
                info.tickSize,
                info.lotSize,
                info.minSize,
                info.maxSize,
                info.minAmount);
}

void print_table_head() {
    std::printf("%-10s %-14s %-24s %-24s %-8s %-8s %-8s %12s %12s %14s %14s %14s\n",
                "EXCHANGE","INSTTYPE","INSTID","ORIGININSTID","BASE","QUOTE","MARGIN",
                "TICKSIZE","LOTSIZE","MINSIZE","MAXSIZE","MINAMOUNT");
}

// 简易 JSON escape (足够 base/quote/margin/instId 这些 ASCII 字符; 不做完整实现)
std::string js_esc(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') { out += '\\'; out += *p; }
        else if (*p < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", *p); out += buf; }
        else out += *p;
    }
    return out;
}

void print_json(const md::InstrumentInfo& info, bool comma) {
    std::printf("%s{\"exchange\":\"%s\",\"instType\":\"%s\","
                "\"instId\":\"%s\",\"originInstId\":\"%s\","
                "\"base\":\"%s\",\"quote\":\"%s\",\"margin\":\"%s\","
                "\"value\":%g,\"tickSize\":%g,\"lotSize\":%g,"
                "\"minSize\":%g,\"maxSize\":%g,\"minAmount\":%g,"
                "\"magnifyNumber\":%g,\"reduceNumber\":%g}",
                comma ? ",\n  " : "",
                js_esc(ExchangeTypeEnum2StrMap[info.exchangeTypeEnum].c_str()).c_str(),
                js_esc(InstTypeEnum2StrMap[info.instTypeEnum].c_str()).c_str(),
                js_esc(info.instId).c_str(),
                js_esc(info.originInstId).c_str(),
                js_esc(info.base).c_str(),
                js_esc(info.quote).c_str(),
                js_esc(info.margin).c_str(),
                info.value, info.tickSize, info.lotSize,
                info.minSize, info.maxSize, info.minAmount,
                info.magnifyNumber, info.reduceNumber);
}

bool filter_pass(const md::InstrumentInfo& info, const Opts& o) {
    if (!o.filter_exch.empty()
        && ExchangeTypeEnum2StrMap[info.exchangeTypeEnum] != o.filter_exch)
        return false;
    if (!o.filter_instType.empty()
        && InstTypeEnum2StrMap[info.instTypeEnum] != o.filter_instType)
        return false;
    return true;
}

} // anonymous namespace


int main(int argc, char** argv) {
    Opts o;
    if (!parse_args(argc, argv, o)) return 1;

    sm::shm::Reader reader;
    if (!reader.open(o.name, /*wait_for_writer_ms=*/0)) {
        std::fprintf(stderr, "failed to open SHM '%s' (contractinfo not running?)\n",
                     o.name.c_str());
        return 2;
    }

    if (!o.json) {
        print_header(reader);
        if (o.meta_only) return 0;
    }

    std::vector<md::InstrumentInfo> vec;
    if (!reader.snapshot(vec)) {
        std::fprintf(stderr, "snapshot failed (writer racing too fast?)\n");
        return 3;
    }

    if (o.json) {
        std::printf("{\n  \"header\": {\"count\": %u, \"capacity\": %u, "
                    "\"generation\": %llu, \"last_update_us\": %llu},\n  \"entries\": [\n  ",
                    reader.count(), reader.capacity(),
                    static_cast<unsigned long long>(reader.generation()),
                    static_cast<unsigned long long>(reader.last_update_us()));
        int shown = 0;
        for (auto& info : vec) {
            if (!filter_pass(info, o)) continue;
            if (o.limit >= 0 && shown >= o.limit) break;
            print_json(info, shown > 0);
            ++shown;
        }
        std::printf("\n  ]\n}\n");
    } else {
        std::printf("\n");
        print_table_head();
        int shown = 0, total_matched = 0;
        for (auto& info : vec) {
            if (!filter_pass(info, o)) continue;
            ++total_matched;
            if (o.limit >= 0 && shown >= o.limit) continue;
            print_table_row(info);
            ++shown;
        }
        std::printf("\n%d entries shown", shown);
        if (total_matched != shown) std::printf(" (of %d matched by filter)", total_matched);
        std::printf(", %zu total in SHM\n", vec.size());
    }
    return 0;
}