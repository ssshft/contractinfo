// TestInfoShm.cpp
//
// 端到端测试: 用 sm::shm::Writer 把已知数据写入 SHM, 用 SecurityManager 读回来,
// 校验字段一致。 顺便测:
//   - 首次开段 (empty state)
//   - full publish 后 SecurityManager 能查到
//   - 覆盖后再查, 数据是新版本
//   - 多线程并发调 get_instrument_info (只读, 应零竞争)
//   - 容量溢出时的截断行为
//
// 编译时需要 -DUSE_INFO_SHM=ON, 因为 SecurityManager 在这个模式下才走 SHM。
//
// 用法: ./TestInfoShm [--shm-name /test_seg]
//   默认使用一个独立的 test-only SHM 名字, 避免污染生产段。 测试结束会 shm_unlink。

#include "InstrumentInfoShm.h"
#include "securitymanager.h"
#include "data_struct.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>


namespace {

const char* kTestShmName = "/bts_test_info_shm";

// 构造一批 fake InstrumentInfo, 用来做写读校验。
// N 条: exchange = BINANCE, instType = SPOT/USDT_SWAP 轮换, instId = TEST0001..TESTNNNN
std::vector<md::InstrumentInfo> make_fake(int n, double px_scale = 1.0) {
    std::vector<md::InstrumentInfo> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        md::InstrumentInfo info;
        std::memset(&info, 0, sizeof(info));
        info.exchangeTypeEnum = BINANCE;
        info.instTypeEnum     = (i % 2 == 0) ? SPOT : USDT_SWAP;
        std::snprintf(info.instId,       INSTID_SIZE, "TEST-%04d", i);
        std::snprintf(info.originInstId, INSTID_SIZE, "TEST%04d",  i);
        std::snprintf(info.base,         INSTID_SIZE, "T%d",       i);
        std::snprintf(info.quote,        INSTID_SIZE, "USDT");
        std::snprintf(info.margin,       INSTID_SIZE, "USDT");
        info.value          = 1.0;
        info.tickSize       = 0.01 * px_scale;
        info.lotSize        = 0.001;
        info.minSize        = 0.001;
        info.maxSize        = 1e6;
        info.minAmount      = 10.0;
        info.magnifyNumber  = 1.0;
        info.reduceNumber   = 1.0;
        v.push_back(info);
    }
    return v;
}

int g_pass = 0, g_fail = 0;
#define EXPECT(cond, msg) do { \
    if (cond) { std::printf("  \033[32mPASS\033[0m %s\n", msg); ++g_pass; } \
    else      { std::printf("  \033[31mFAIL\033[0m %s\n", msg); ++g_fail; } \
} while (0)

} // anonymous namespace


int main(int argc, char** argv) {
    std::string shmName = kTestShmName;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shm-name" && i + 1 < argc) shmName = argv[++i];
    }

    // 环境隔离: 让 SecurityManager 通过 BTS_INFO_SHM_NAME 环境变量指向测试段
    setenv("BTS_INFO_SHM_NAME", shmName.c_str(), 1);

    // 保证测试独立: 清掉可能残留的老段
    shm_unlink(shmName.c_str());

    std::printf("=== TestInfoShm start (shm='%s') ===\n\n", shmName.c_str());


    // ------------------------------------------------------------------
    // Test 1: Writer 打开新段 + 初始 empty
    // ------------------------------------------------------------------
    std::printf("[Test 1] open new SHM + initial empty state\n");
    {
        sm::shm::Writer w;
        EXPECT(w.open(shmName, /*capacity=*/1024), "Writer::open new segment");
        EXPECT(w.capacity() == 1024, "capacity as configured");

        sm::shm::Reader r;
        EXPECT(r.open(shmName, /*wait_ms=*/500), "Reader::open");
        EXPECT(r.capacity() == 1024, "reader sees same capacity");
        EXPECT(r.count() == 0, "count == 0 before any publish");
        EXPECT(r.generation() == 0, "generation starts at 0");

        std::vector<md::InstrumentInfo> vec;
        EXPECT(r.snapshot(vec), "snapshot on empty segment");
        EXPECT(vec.empty(), "snapshot vec is empty");
    }
    std::printf("\n");


    // ------------------------------------------------------------------
    // Test 2: Writer publish + SecurityManager 读回校验
    // ------------------------------------------------------------------
    std::printf("[Test 2] publish + SecurityManager readback\n");
    {
        auto fakes = make_fake(100);
        sm::shm::Writer w;
        assert(w.open(shmName, 1024));
        EXPECT(w.publish(fakes), "Writer::publish 100 entries");

        // 关闭后再 open — 模拟另一进程的 reader
        w.close();

        // SecurityManager ctor 会 mmap SHM + rebuild map (needUpdate=false 不起后台线程)
        sm::SecurityManager smc("", 0, "", /*needUpdate=*/false);

        // 校验每一条都能查到, 字段一致
        int matched = 0;
        for (auto& f : fakes) {
            md::InstrumentInfo got;
            if (smc.get_instrument_info(f.exchangeTypeEnum, f.instTypeEnum, f.instId, got)) {
                if (std::strcmp(got.instId, f.instId) == 0 &&
                    std::strcmp(got.originInstId, f.originInstId) == 0 &&
                    got.tickSize == f.tickSize &&
                    got.lotSize == f.lotSize) {
                    ++matched;
                }
            }
        }
        EXPECT(matched == static_cast<int>(fakes.size()),
               "all 100 entries queryable via SecurityManager");

        // originInstId 也能查
        md::InstrumentInfo got;
        bool okOrig = smc.get_instrument_info(BINANCE, SPOT, "TEST0000", got);
        EXPECT(okOrig, "lookup by originInstId also works");

        // 不存在的 key 返回 false
        md::InstrumentInfo none;
        EXPECT(!smc.get_instrument_info(BINANCE, SPOT, "NONEXISTENT", none),
               "unknown instId returns false");
    }
    std::printf("\n");


    // ------------------------------------------------------------------
    // Test 3: 覆盖 publish, 新 SecurityManager 看到新数据
    // ------------------------------------------------------------------
    std::printf("[Test 3] republish with different data -> fresh SecurityManager sees update\n");
    {
        auto fakes_v2 = make_fake(50, /*px_scale=*/10.0);   // tickSize = 0.1 (变了)
        sm::shm::Writer w;
        assert(w.open(shmName, 1024));
        EXPECT(w.publish(fakes_v2), "Writer::publish 50 entries (v2)");

        sm::SecurityManager smc("", 0, "", false);
        md::InstrumentInfo got;
        bool ok = smc.get_instrument_info(BINANCE, SPOT, "TEST-0000", got);
        EXPECT(ok && got.tickSize == 0.1, "tickSize reflects updated value");

        // 老数据不应该还查到 (count 从 100 降到 50)
        md::InstrumentInfo old;
        bool okOld = smc.get_instrument_info(BINANCE, SPOT, "TEST-0090", old);
        EXPECT(!okOld, "instId beyond new count is gone");
    }
    std::printf("\n");


    // ------------------------------------------------------------------
    // Test 4: 并发 reader (多线程调 get_instrument_info, 只读应零竞争)
    // ------------------------------------------------------------------
    std::printf("[Test 4] concurrent SecurityManager readers (no data races)\n");
    {
        auto fakes = make_fake(1000);
        sm::shm::Writer w;
        assert(w.open(shmName, 4096));
        assert(w.publish(fakes));

        sm::SecurityManager smc("", 0, "", false);

        constexpr int kThreads   = 8;
        constexpr int kIters     = 100'000;
        std::atomic<int> total_ok{0}, total_notfound{0};

        std::vector<std::thread> ths;
        auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < kThreads; ++t) {
            ths.emplace_back([&, t] {
                unsigned seed = static_cast<unsigned>(t) * 12345u + 1u;
                for (int i = 0; i < kIters; ++i) {
                    int idx = static_cast<int>(rand_r(&seed) % 1000);
                    char buf[INSTID_SIZE];
                    std::snprintf(buf, sizeof(buf), "TEST-%04d", idx);
                    md::InstrumentInfo got;
                    InstType instTy = (idx % 2 == 0) ? SPOT : USDT_SWAP;
                    if (smc.get_instrument_info(BINANCE, instTy, buf, got)) {
                        total_ok.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        total_notfound.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& th : ths) th.join();
        auto t1 = std::chrono::steady_clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        long total = kThreads * kIters;
        double qps = total / sec;
        std::printf("    %d threads × %d iters = %ld queries in %.3fs (%.1f M/s)\n",
                    kThreads, kIters, total, sec, qps / 1e6);

        EXPECT(total_ok.load() == total, "all queries succeeded, no torn reads");
        EXPECT(total_notfound.load() == 0, "no unexpected not-found");
    }
    std::printf("\n");


    // ------------------------------------------------------------------
    // Test 5: 容量溢出被 gracefully 截断
    // ------------------------------------------------------------------
    std::printf("[Test 5] capacity overflow -> truncated + warned\n");
    {
        // 用一个小容量段测试
        shm_unlink(shmName.c_str());
        sm::shm::Writer w;
        assert(w.open(shmName, /*capacity=*/64));

        auto too_many = make_fake(200);
        EXPECT(w.publish(too_many), "publish over-capacity returns true (truncated)");

        sm::shm::Reader r;
        assert(r.open(shmName, 500));
        EXPECT(r.count() == 64, "count is clamped to capacity");
    }
    std::printf("\n");


    // ------------------------------------------------------------------
    // Test 6: reopen 复用 (capacity 一致时 Writer 重启不重建段)
    // ------------------------------------------------------------------
    std::printf("[Test 6] Writer reopen with same capacity -> reuses segment, generation preserved\n");
    {
        shm_unlink(shmName.c_str());

        sm::shm::Writer w1;
        assert(w1.open(shmName, 512));
        auto fakes = make_fake(10);
        assert(w1.publish(fakes));

        sm::shm::Reader rBefore;
        assert(rBefore.open(shmName, 500));
        uint64_t gen_before = rBefore.generation();

        // Writer 1 close, Writer 2 open (模拟 contractinfo 崩溃重启)
        w1.close();
        sm::shm::Writer w2;
        EXPECT(w2.open(shmName, 512), "reopen with same capacity");
        EXPECT(w2.capacity() == 512, "capacity unchanged after reopen");

        // 再 publish, generation 应该继续增长 (不重置为 0)
        assert(w2.publish(fakes));
        sm::shm::Reader rAfter;
        assert(rAfter.open(shmName, 500));
        EXPECT(rAfter.generation() > gen_before,
               "generation continues from previous value (no reset)");
    }
    std::printf("\n");


    // 清理
    shm_unlink(shmName.c_str());

    std::printf("=== summary: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
