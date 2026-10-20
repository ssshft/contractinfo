// tools/GetInfo.cpp
// ============================================================================
// contractinfo 的**读侧**工具：完全走生产消费路径（SecurityManager）去查合约信息，
// 回答"db 现在到底能查到什么"。
//
// 和另外两个工具的分工:
//   GetInfo     读侧 —— SecurityManager → SHM/Redis → 内存 map，和 db 看到的一模一样
//   InfoCheck   写侧 —— 直接构造 XxxInfo 拉一遍真实接口，校验我们写进去的对不对
//   DumpInfoShm 裸段 —— 只 dump SHM 里的原始条目，不解释语义
//
// 排查顺序建议:
//   1. InfoCheck --exchange XXX        确认我们写进去的是对的
//   2. DumpInfoShm --exchange XXX      确认 SHM 里真的有
//   3. GetInfo --exchange XXX --stats  确认读侧能拿到
//   任何一步断了，问题就定位在那一段。
//
// 用法:
//   ./GetInfo                                  # 列出全部（表格）
//   ./GetInfo --exchange BITUNIX               # 只看一家
//   ./GetInfo --exchange BITUNIX --inst-type USDT_SWAP
//   ./GetInfo --inst-id BTC-USDT               # 精确查一条（instId 或 originInstId 都行）
//   ./GetInfo --inst-id BTCUSDT --exchange BITUNIX
//   ./GetInfo --stats                          # 只看统计（总条数 / 每家条数 / 新鲜度）
//   ./GetInfo --keys                           # 只打复合键，排查"撞键/退化成空串"
//   ./GetInfo --json                           # 机器可读
//   ./GetInfo --resolve pairs.txt              # 批量解析：每行 "EXCH,INSTTYPE,INSTID"
//   ./GetInfo --shm-name /my_seg               # 覆盖 BTS_INFO_SHM_NAME
//   ./GetInfo --limit 20 --sample
//
// --resolve 是模拟 db 启动时干的事: db 拿着配置里的 (exchange, instType, instId) 三元组
// 去 SecurityManager 查，查不到就打一行 "cannot find originInstId" 然后这个 unit 不启动。
// 所以这个子命令的未命中数 = 那家交易所会有多少个对子静默失效。
//
// 退出码: 0 正常；1 有未命中/查询失败；2 参数或数据源错误。
// ============================================================================

#include "config.h"
#include "data_struct.h"
#include "log_engine.h"
#include "program_util.h"   // crypto::create_directory
#include "securitymanager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <stdlib.h>      // setenv / getenv（POSIX，不在 std:: 里）
#include <sys/stat.h>

namespace {

// SHM 默认名。刻意不引用 sm::shm::kDefaultShmName —— 那个符号只在
// -DUSE_INFO_SHM 下才存在，写死一个字面量可以让本工具在两种编译模式下都能过。
constexpr const char* kDefaultShm = "/bts_instrument_info";

struct Opts {
    std::string shmName;      // 空 = 不覆盖环境变量
    std::string config;       // 可选：非 SHM 模式下从中读 redis 配置
    std::string exchange;
    std::string instType;
    std::string instId;
    std::string resolveFile;
    int  limit  = -1;
    bool json   = false;
    bool stats  = false;
    bool keys   = false;
    bool sample = false;
};

void usage(const char* prog) {
    std::fprintf(stderr,
        "用法: %s [选项]\n"
        "\n"
        "  --exchange <NAME>    只显示某交易所（精确匹配，大小写敏感）\n"
        "  --inst-type <T>      只显示某 instType（USDT_SWAP / C_SWAP / SPOT ...）\n"
        "  --inst-id <ID>       精确查一条（instId 或 originInstId 都可以）\n"
        "  --resolve <file>     批量解析，每行 \"EXCH,INSTTYPE,INSTID\"（# 开头为注释）\n"
        "  --stats              只打印统计\n"
        "  --keys               只打印复合键\n"
        "  --json               JSON 输出\n"
        "  --sample             额外打印 calcType / instIdCode 等细节\n"
        "  --limit <N>          最多显示 N 条\n"
        "  --shm-name <name>    覆盖 BTS_INFO_SHM_NAME（默认 %s）\n"
        "  --config <path>      非 SHM 模式下读 redis 配置\n"
        "  -h, --help\n", prog, kDefaultShm);
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
        if (a == "--exchange")       { auto v = next(i, "--exchange");  if (!v) return false; o.exchange = v; }
        else if (a == "--inst-type") { auto v = next(i, "--inst-type"); if (!v) return false; o.instType = v; }
        else if (a == "--inst-id")   { auto v = next(i, "--inst-id");   if (!v) return false; o.instId = v; }
        else if (a == "--resolve")   { auto v = next(i, "--resolve");   if (!v) return false; o.resolveFile = v; }
        else if (a == "--limit")     { auto v = next(i, "--limit");     if (!v) return false; o.limit = std::atoi(v); }
        else if (a == "--shm-name")  { auto v = next(i, "--shm-name");  if (!v) return false; o.shmName = v; }
        else if (a == "--config")    { auto v = next(i, "--config");    if (!v) return false; o.config = v; }
        else if (a == "--json")      { o.json = true; }
        else if (a == "--stats")     { o.stats = true; }
        else if (a == "--keys")      { o.keys = true; }
        else if (a == "--sample")    { o.sample = true; }
        else if (a == "-h" || a == "--help") { usage(argv[0]); return false; }
        else {
            std::fprintf(stderr, "未知参数: %s\n", a.c_str());
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

std::string jesc(const char* s) {
    std::string out;
    for (const char* p = s; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += static_cast<char>(c);
    }
    return out;
}

// enum -> 名字。用 find，不用 map[]（后者会插空串，把"映射缺失"伪装成"交易所叫空名"）。
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

void printRecord(const md::InstrumentInfo& i, bool detail) {
    std::printf("%-10s %-14s %-22s %-22s %-6s %-6s %-6s %14.10g %14.10g %12.6g %12.6g %12.6g\n",
                exchName(i.exchangeTypeEnum).c_str(),
                instTypeName(i.instTypeEnum).c_str(),
                i.instId, i.originInstId, i.base, i.quote, i.margin,
                i.tickSize, i.lotSize, i.minSize, i.maxSize, i.minAmount);
    if (detail) {
        std::printf("     value=%.10g magnify=%.10g reduce=%.10g priceDigits=%d sizeDigits=%d "
                    "pricePow10=%lld sizePow10=%lld tickSizeInt=%lld lotSizeInt=%lld "
                    "calcType=%d instIdCode=%lld\n",
                    i.value, i.magnifyNumber, i.reduceNumber,
                    i.priceDigits, i.sizeDigits,
                    static_cast<long long>(i.pricePow10), static_cast<long long>(i.sizePow10),
                    static_cast<long long>(i.tickSizeInt), static_cast<long long>(i.lotSizeInt),
                    i.calcType, static_cast<long long>(i.instIdCode));
    }
}

void printHead() {
    std::printf("%-10s %-14s %-22s %-22s %-6s %-6s %-6s %14s %14s %12s %12s %12s\n",
                "EXCHANGE", "INSTTYPE", "INSTID", "ORIGININSTID", "BASE", "QUOTE", "MARGIN",
                "TICKSIZE", "LOTSIZE", "MINSIZE", "MAXSIZE", "MINAMOUNT");
}

bool matchFilter(const md::InstrumentInfo& i, const Opts& o) {
    if (!o.exchange.empty() && exchName(i.exchangeTypeEnum) != o.exchange) return false;
    if (!o.instType.empty() && instTypeName(i.instTypeEnum) != o.instType) return false;
    return true;
}

void printStats(const std::vector<md::InstrumentInfo>& v) {
    std::map<std::string, int> byExch;
    std::map<std::string, int> byType;
    std::map<std::string, std::map<std::string, int>> byExchType;
    for (const auto& i : v) {
        std::string e = exchName(i.exchangeTypeEnum);
        std::string t = instTypeName(i.instTypeEnum);
        byExch[e]++;
        byType[t]++;
        byExchType[e][t]++;
    }

    std::printf("总条数: %zu\n\n", v.size());
    std::printf("%-12s %8s   instType 分布\n", "EXCHANGE", "COUNT");
    for (const auto& kv : byExch) {
        std::string dist;
        for (const auto& t : byExchType[kv.first]) {
            if (!dist.empty()) dist += " ";
            dist += t.first + "=" + std::to_string(t.second);
        }
        std::printf("%-12s %8d   %s\n", kv.first.c_str(), kv.second, dist.c_str());
    }
    std::printf("\n按 instType 汇总:\n");
    for (const auto& kv : byType) {
        std::printf("  %-14s %8d\n", kv.first.c_str(), kv.second);
    }
}

int doResolve(sm::SecurityManager& smc, const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) {
        std::fprintf(stderr, "打不开 %s\n", path.c_str());
        return 2;
    }
    int total = 0, hit = 0;
    char line[512];
    while (std::fgets(line, sizeof(line), fp)) {
        std::string s(line);
        // 去注释、去空白
        size_t hash = s.find('#');
        if (hash != std::string::npos) s.resize(hash);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
            s.pop_back();
        }
        size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
        s = s.substr(a);
        if (s.empty()) continue;

        // "EXCH,INSTTYPE,INSTID"
        size_t p1 = s.find(',');
        size_t p2 = (p1 == std::string::npos) ? std::string::npos : s.find(',', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) {
            std::printf("  [BAD ] %-40s  (格式应为 EXCH,INSTTYPE,INSTID)\n", s.c_str());
            ++total;
            continue;
        }
        std::string exch = s.substr(0, p1);
        std::string ityp = s.substr(p1 + 1, p2 - p1 - 1);
        std::string iid  = s.substr(p2 + 1);

        md::InstrumentInfo info;
        std::memset(&info, 0, sizeof(info));
        ++total;
        if (smc.get_instrument_info(exch.c_str(), ityp.c_str(), iid.c_str(), info)) {
            ++hit;
            std::printf("  [ OK ] %-10s %-12s %-20s -> instId=%s origin=%s tick=%g lot=%g\n",
                        exch.c_str(), ityp.c_str(), iid.c_str(),
                        info.instId, info.originInstId, info.tickSize, info.lotSize);
        }
        else {
            std::printf("  [MISS] %-10s %-12s %-20s  <- db 会打 'cannot find originInstId' 并跳过\n",
                        exch.c_str(), ityp.c_str(), iid.c_str());
        }
    }
    std::fclose(fp);
    std::printf("\n解析 %d 条，命中 %d，未命中 %d\n", total, hit, total - hit);
    return hit == total ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    if (!parseArgs(argc, argv, o)) {
        return 2;
    }

    // SecurityManager 构造函数里就会 LOG_ERROR（SHM 打不开 / 数据陈旧），
    // 所以日志必须先初始化，否则那些诊断信息会丢。
    std::string logDir = "logs";
    if (!crypto::create_directory(logDir)) {
        logDir = ".";
    }
    log_maintain("GetInfo", logDir, "debug");

    if (!o.shmName.empty()) {
        ::setenv("BTS_INFO_SHM_NAME", o.shmName.c_str(), 1);
    }

    // 读侧凭据：编译了 USE_INFO_SHM 时构造函数会忽略它们。
    std::string host = "127.0.0.1";
    std::string port = "9379";
    std::string pass;

#ifndef USE_INFO_SHM
    if (!o.config.empty()) {
        struct stat st;
        if (::stat(o.config.c_str(), &st) != 0) {
            // Config::load() 对不存在的文件直接 exit(-1)，所以先探一下
            std::fprintf(stderr, "配置文件不存在: %s（继续用默认 redis 凭据）\n", o.config.c_str());
        }
        else {
            Config* cfg = Config::instance();
            cfg->load(o.config.c_str());
            std::string k = "redis";
            if (cfg->has_exchange(k)) {
                cfg->get_redis_config("host", host);
                cfg->get_redis_config("port", port);
                cfg->get_redis_config("password", pass);
            }
        }
    }
#else
    (void)o.config;
#endif

    // needUpdate=false：工具是一次性进程，不需要后台每 5 分钟重建缓存的后台线程。
    sm::SecurityManager smc(host.c_str(), std::atoi(port.c_str()), pass.c_str(), false);

    // ---------------- --resolve ----------------
    if (!o.resolveFile.empty()) {
        return doResolve(smc, o.resolveFile);
    }

    // ---------------- --inst-id（走 hot path） ----------------
    if (!o.instId.empty()) {
        md::InstrumentInfo info;
        std::memset(&info, 0, sizeof(info));

        // 不指定交易所/instType 时，把所有组合都试一遍，省得用户自己猜。
        // 组合来自 enum<->str 两张表，**不写死名单** —— 写死的名单一定会漏掉
        // 最新加的那家交易所，正好让"新交易所查不到"这个 bug 藏起来。
        std::vector<std::pair<std::string, std::string>> combos;
        if (!o.exchange.empty() && !o.instType.empty()) {
            combos.emplace_back(o.exchange, o.instType);
        }
        else {
            for (const auto& e : ExchangeTypeEnum2StrMap) {
                if (e.first == ExchangeType_MIN || e.second.empty()) continue;
                if (!o.exchange.empty() && o.exchange != e.second) continue;
                for (const auto& t : InstTypeEnum2StrMap) {
                    if (t.first == InstType_MIN || t.second.empty()) continue;
                    if (!o.instType.empty() && o.instType != t.second) continue;
                    combos.emplace_back(e.second, t.second);
                }
            }
        }

        int hits = 0;
        for (const auto& c : combos) {
            if (smc.get_instrument_info(c.first.c_str(), c.second.c_str(), o.instId.c_str(), info)) {
                ++hits;
                if (o.json) {
                    std::printf("{\"exchange\":\"%s\",\"instType\":\"%s\",\"instId\":\"%s\","
                                "\"originInstId\":\"%s\",\"base\":\"%s\",\"quote\":\"%s\","
                                "\"margin\":\"%s\",\"value\":%.10g,\"tickSize\":%.10g,"
                                "\"lotSize\":%.10g,\"minSize\":%.10g,\"maxSize\":%.10g,"
                                "\"minAmount\":%.10g,\"magnifyNumber\":%.10g,"
                                "\"reduceNumber\":%.10g,\"calcType\":%d,\"instIdCode\":%lld}\n",
                                // jesc 返回 std::string，printf 是可变参函数，必须显式取 c_str()。
                                // 这里对临时对象取 c_str() 是安全的：临时量存活到整个 full-expression
                                // 结束，而 printf 调用本身就在同一个 full-expression 里。
                                jesc(c.first.c_str()).c_str(), jesc(c.second.c_str()).c_str(),
                                jesc(info.instId).c_str(), jesc(info.originInstId).c_str(),
                                jesc(info.base).c_str(), jesc(info.quote).c_str(),
                                jesc(info.margin).c_str(),
                                info.value, info.tickSize, info.lotSize,
                                info.minSize, info.maxSize, info.minAmount,
                                info.magnifyNumber, info.reduceNumber,
                                info.calcType, static_cast<long long>(info.instIdCode));
                }
                else {
                    std::printf("命中 %s / %s\n", c.first.c_str(), c.second.c_str());
                    printHead();
                    printRecord(info, true);
                }
            }
        }
        if (hits == 0) {
            std::fprintf(stderr, "未找到 '%s'（instId 与 originInstId 都没命中）\n", o.instId.c_str());
            return 1;
        }
        return 0;
    }

    // ---------------- 全量 ----------------
    std::vector<md::InstrumentInfo> all;
    if (!smc.get_all_instruments(all)) {
        std::fprintf(stderr, "读不到合约信息。\n");
#ifdef USE_INFO_SHM
        std::fprintf(stderr,
            "  编译时启用了 USE_INFO_SHM，请确认 contractinfo 在跑、SHM 名字对（默认 %s）。\n"
            "  当前 SHM 名字: %s\n",
            kDefaultShm,
            ::getenv("BTS_INFO_SHM_NAME") ? ::getenv("BTS_INFO_SHM_NAME") : kDefaultShm);
#else
        std::fprintf(stderr, "  编译时未启用 USE_INFO_SHM，请确认 redis 可达。\n");
#endif
        return 2;
    }
    if (all.empty()) {
        std::fprintf(stderr, "合约信息为空（contractinfo 可能还没完成第一轮 publish，等 60s 再试）\n");
        return 2;
    }

    if (o.stats) {
        printStats(all);
        return 0;
    }

    // 过滤
    std::vector<const md::InstrumentInfo*> shown;
    shown.reserve(all.size());
    for (const auto& i : all) {
        if (matchFilter(i, o)) {
            shown.push_back(&i);
        }
    }

    if (o.json) {
        std::printf("{\n  \"count\": %zu,\n  \"entries\": [\n", shown.size());
        for (size_t k = 0; k < shown.size(); ++k) {
            const md::InstrumentInfo& i = *shown[k];
            if (o.limit >= 0 && static_cast<int>(k) >= o.limit) break;
            std::printf("%s    {\"exchange\":\"%s\",\"instType\":\"%s\",\"instId\":\"%s\","
                        "\"originInstId\":\"%s\",\"base\":\"%s\",\"quote\":\"%s\",\"margin\":\"%s\","
                        "\"value\":%.10g,\"tickSize\":%.10g,\"lotSize\":%.10g,"
                        "\"minSize\":%.10g,\"maxSize\":%.10g,\"minAmount\":%.10g,"
                        "\"magnifyNumber\":%.10g,\"reduceNumber\":%.10g,"
                        "\"calcType\":%d,\"instIdCode\":%lld}",
                        k ? ",\n" : "",
                        jesc(exchName(i.exchangeTypeEnum).c_str()).c_str(),
                        jesc(instTypeName(i.instTypeEnum).c_str()).c_str(),
                        jesc(i.instId).c_str(), jesc(i.originInstId).c_str(),
                        jesc(i.base).c_str(), jesc(i.quote).c_str(), jesc(i.margin).c_str(),
                        i.value, i.tickSize, i.lotSize, i.minSize, i.maxSize, i.minAmount,
                        i.magnifyNumber, i.reduceNumber, i.calcType,
                        static_cast<long long>(i.instIdCode));
        }
        std::printf("\n  ]\n}\n");
        return 0;
    }

    if (o.keys) {
        // 复合键 = "{EXCH}.{INSTTYPE}.{INSTID}.INSTRUMENT_INFO"。
        // 交易所枚举映射缺失时这里会看到 ".USDT_SWAP.BTC-USDT.INSTRUMENT_INFO" —— 空串开头。
        for (const auto* i : shown) {
            std::printf("%s\n",
                        crypto::get_instrumentInfo_channel_key(
                            exchName(i->exchangeTypeEnum), instTypeName(i->instTypeEnum), i->instId)
                            .c_str());
        }
        std::printf("\n%zu 条\n", shown.size());
        return 0;
    }

    printHead();
    int printed = 0;
    for (const auto* i : shown) {
        if (o.limit >= 0 && printed >= o.limit) break;
        printRecord(*i, o.sample);
        ++printed;
    }
    std::printf("\n%d 条显示", printed);
    if (static_cast<int>(shown.size()) != printed) {
        std::printf("（过滤后共 %zu 条）", shown.size());
    }
    std::printf("，全量 %zu 条\n", all.size());
    std::printf("日志: %s/GetInfo_*.log\n", logDir.c_str());
    return 0;
}
