// BenchInfoShm.cpp
//
// 性能微基准: 三个层次
//   1. sm::shm::Writer::publish() 全量原子交换 —— 各 N 条目的 latency 分布
//   2. sm::shm::Reader::snapshot() 全量读回 —— seqlock 拷贝开销
//   3. sm::SecurityManager::get_instrument_info() —— 实际 hot path
//        - 单线程 QPS
//        - 多线程 (2/4/8) 并发 QPS + p50/p95/p99
//
// 编译时需要 -DUSE_INFO_SHM=ON (SecurityManager 才走 SHM 路径)。
//
// 用法: ./BenchInfoShm [--shm-name /bench_seg]

#include "InstrumentInfoShm.h"
#include "securitymanager.h"
#include "data_struct.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>


namespace {

using clk = std::chrono::steady_clock;
using ns  = std::chrono::nanoseconds;

const char* kBenchShmName = "/bts_bench_info_shm";

std::vector<md::InstrumentInfo> make_fake(int n) {
    std::vector<md::InstrumentInfo> v; v.reserve(n);
    for (int i = 0; i < n; ++i) {
        md::InstrumentInfo info;
        std::memset(&info, 0, sizeof(info));
        info.exchangeTypeEnum = BINANCE;
        info.instTypeEnum     = (i % 2 == 0) ? SPOT : USDT_SWAP;
        std::snprintf(info.instId,       INSTID_SIZE, "BENCH-%05d", i);
        std::snprintf(info.originInstId, INSTID_SIZE, "BENCH%05d",  i);
        std::snprintf(info.base,         INSTID_SIZE, "B%d",        i);
        std::snprintf(info.quote,        INSTID_SIZE, "USDT");
        std::snprintf(info.margin,       INSTID_SIZE, "USDT");
        info.value = 1.0; info.tickSize = 0.01; info.lotSize = 0.001;
        info.minSize = 0.001; info.maxSize = 1e6; info.minAmount = 10.0;
        info.magnifyNumber = 1.0; info.reduceNumber = 1.0;
        v.push_back(info);
    }
    return v;
}

// p50/p95/p99 (纳秒)
struct Percentiles { long p50, p95, p99, max; };
Percentiles pct(std::vector<long>& lat) {
    std::sort(lat.begin(), lat.end());
    auto at = [&](double p) -> long {
        if (lat.empty()) return 0;
        size_t i = std::min<size_t>(lat.size() - 1,
                                    static_cast<size_t>(lat.size() * p));
        return lat[i];
    };
    return { at(0.50), at(0.95), at(0.99), lat.empty() ? 0 : lat.back() };
}

void print_pct(const char* label, long total_ns, size_t iters, const Percentiles& p) {
    double avg_ns = iters ? double(total_ns) / iters : 0.0;
    double qps = iters ? iters / (total_ns / 1e9) : 0.0;
    std::printf("  %-40s  avg=%8.1fns  p50=%6ldns  p95=%6ldns  p99=%6ldns  max=%7ldns  (%.2f M/s)\n",
                label, avg_ns, p.p50, p.p95, p.p99, p.max, qps / 1e6);
}

} // anonymous namespace


int main(int argc, char** argv) {
    std::string shmName = kBenchShmName;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shm-name" && i + 1 < argc) shmName = argv[++i];
    }
    setenv("BTS_INFO_SHM_NAME", shmName.c_str(), 1);
    shm_unlink(shmName.c_str());

    std::printf("=== BenchInfoShm ===\n\n");


    // ================================================================
    // Bench 1: Writer::publish() 全量原子交换 latency
    // ================================================================
    std::printf("[Bench 1] Writer::publish() latency vs N entries\n");
    for (int N : {100, 1000, 5000, 10000, 20000}) {
        shm_unlink(shmName.c_str());
        sm::shm::Writer w;
        if (!w.open(shmName, 32768)) { std::fprintf(stderr, "open failed\n"); return 1; }

        auto fakes = make_fake(N);
        constexpr int kIters = 200;
        std::vector<long> lat; lat.reserve(kIters);

        // warmup
        for (int i = 0; i < 10; ++i) w.publish(fakes);

        long total = 0;
        for (int i = 0; i < kIters; ++i) {
            auto t0 = clk::now();
            w.publish(fakes);
            auto t1 = clk::now();
            long d = std::chrono::duration_cast<ns>(t1 - t0).count();
            lat.push_back(d);
            total += d;
        }
        char label[64]; std::snprintf(label, sizeof(label), "publish N=%d", N);
        print_pct(label, total, kIters, pct(lat));
    }
    std::printf("\n");


    // ================================================================
    // Bench 2: Reader::snapshot() 全量读回 latency
    // ================================================================
    std::printf("[Bench 2] Reader::snapshot() latency vs N entries\n");
    for (int N : {100, 1000, 5000, 10000, 20000}) {
        shm_unlink(shmName.c_str());
        sm::shm::Writer w;
        assert(w.open(shmName, 32768));
        assert(w.publish(make_fake(N)));

        sm::shm::Reader r;
        assert(r.open(shmName, 500));

        constexpr int kIters = 500;
        std::vector<long> lat; lat.reserve(kIters);
        std::vector<md::InstrumentInfo> buf;

        for (int i = 0; i < 20; ++i) r.snapshot(buf);   // warmup
        long total = 0;
        for (int i = 0; i < kIters; ++i) {
            auto t0 = clk::now();
            r.snapshot(buf);
            auto t1 = clk::now();
            long d = std::chrono::duration_cast<ns>(t1 - t0).count();
            lat.push_back(d);
            total += d;
        }
        char label[64]; std::snprintf(label, sizeof(label), "snapshot N=%d", N);
        print_pct(label, total, kIters, pct(lat));
    }
    std::printf("\n");


    // ================================================================
    // Bench 3: SecurityManager::get_instrument_info() 单线程 QPS
    // ================================================================
    std::printf("[Bench 3] SecurityManager::get_instrument_info() single-thread\n");
    {
        shm_unlink(shmName.c_str());
        sm::shm::Writer w;
        assert(w.open(shmName, 32768));
        constexpr int N = 5000;
        assert(w.publish(make_fake(N)));
        w.close();

        sm::SecurityManager smc("", 0, "", false);

        constexpr int kIters = 5'000'000;
        std::vector<long> lat; lat.reserve(1000);
        unsigned seed = 42;

        // warmup
        md::InstrumentInfo tmp;
        for (int i = 0; i < 10000; ++i) {
            char buf[INSTID_SIZE];
            int idx = rand_r(&seed) % N;
            std::snprintf(buf, sizeof(buf), "BENCH-%05d", idx);
            InstType it = (idx % 2 == 0) ? SPOT : USDT_SWAP;
            smc.get_instrument_info(BINANCE, it, buf, tmp);
        }

        auto t0 = clk::now();
        int hits = 0;
        for (int i = 0; i < kIters; ++i) {
            int idx = rand_r(&seed) % N;
            char buf[INSTID_SIZE];
            std::snprintf(buf, sizeof(buf), "BENCH-%05d", idx);
            InstType it = (idx % 2 == 0) ? SPOT : USDT_SWAP;
            if (smc.get_instrument_info(BINANCE, it, buf, tmp)) ++hits;

            // 每 5k 次采一次 latency (避免整体计时开销)
            if ((i & 4095) == 0) {
                auto ta = clk::now();
                smc.get_instrument_info(BINANCE, it, buf, tmp);
                auto tb = clk::now();
                lat.push_back(std::chrono::duration_cast<ns>(tb - ta).count());
            }
        }
        auto t1 = clk::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        double qps = kIters / sec;
        std::printf("    total=%d hits=%d elapsed=%.3fs  %.2f M/s  avg=%.1fns\n",
                    kIters, hits, sec, qps / 1e6, 1e9 / qps);
        auto p = pct(lat);
        std::printf("    sampled lat: p50=%ldns p95=%ldns p99=%ldns max=%ldns\n",
                    p.p50, p.p95, p.p99, p.max);
    }
    std::printf("\n");


    // ================================================================
    // Bench 4: SecurityManager 多线程并发 QPS
    // ================================================================
    std::printf("[Bench 4] SecurityManager::get_instrument_info() multi-thread\n");
    {
        shm_unlink(shmName.c_str());
        sm::shm::Writer w;
        assert(w.open(shmName, 32768));
        constexpr int N = 5000;
        assert(w.publish(make_fake(N)));
        w.close();

        sm::SecurityManager smc("", 0, "", false);

        for (int T : {1, 2, 4, 8, 16}) {
            constexpr int kIters = 2'000'000;
            std::atomic<long> hits{0};
            std::vector<std::thread> ths;
            auto t0 = clk::now();
            for (int t = 0; t < T; ++t) {
                ths.emplace_back([&, t] {
                    unsigned seed = static_cast<unsigned>(t) * 7919u + 1u;
                    md::InstrumentInfo tmp;
                    long local_hits = 0;
                    for (int i = 0; i < kIters; ++i) {
                        int idx = rand_r(&seed) % N;
                        char buf[INSTID_SIZE];
                        std::snprintf(buf, sizeof(buf), "BENCH-%05d", idx);
                        InstType it = (idx % 2 == 0) ? SPOT : USDT_SWAP;
                        if (smc.get_instrument_info(BINANCE, it, buf, tmp)) ++local_hits;
                    }
                    hits.fetch_add(local_hits, std::memory_order_relaxed);
                });
            }
            for (auto& th : ths) th.join();
            auto t1 = clk::now();
            double sec = std::chrono::duration<double>(t1 - t0).count();
            long total = long(T) * kIters;
            double qps = total / sec;
            std::printf("    threads=%2d  total=%9ld  elapsed=%.3fs  aggregate=%.2f M/s  per-thread=%.2f M/s\n",
                        T, total, sec, qps / 1e6, qps / 1e6 / T);
        }
    }
    std::printf("\n");


    // ================================================================
    // Bench 5: refresh 场景 (writer 一边刷新一边 reader 查, 验证无阻塞)
    // ================================================================
    std::printf("[Bench 5] concurrent writer publish + reader query (no read stalls)\n");
    {
        shm_unlink(shmName.c_str());
        sm::shm::Writer w;
        assert(w.open(shmName, 32768));
        constexpr int N = 5000;
        auto fakes = make_fake(N);
        assert(w.publish(fakes));

        sm::SecurityManager smc("", 0, "", false);

        std::atomic<bool> stop{false};
        // Writer 线程: 每 100ms 全量刷新 (模拟极端场景, 实际 60s 一次)
        std::thread writer([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                w.publish(fakes);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        // Reader 线程: 疯狂查
        constexpr int T = 4;
        constexpr int kIters = 1'000'000;
        std::vector<std::thread> readers;
        std::atomic<long> hits{0};
        auto t0 = clk::now();
        for (int t = 0; t < T; ++t) {
            readers.emplace_back([&, t] {
                unsigned seed = static_cast<unsigned>(t) * 31337u + 1u;
                md::InstrumentInfo tmp;
                long local = 0;
                for (int i = 0; i < kIters; ++i) {
                    int idx = rand_r(&seed) % N;
                    char buf[INSTID_SIZE];
                    std::snprintf(buf, sizeof(buf), "BENCH-%05d", idx);
                    InstType it = (idx % 2 == 0) ? SPOT : USDT_SWAP;
                    if (smc.get_instrument_info(BINANCE, it, buf, tmp)) ++local;
                }
                hits.fetch_add(local, std::memory_order_relaxed);
            });
        }
        for (auto& th : readers) th.join();
        auto t1 = clk::now();
        stop.store(true);
        writer.join();

        double sec = std::chrono::duration<double>(t1 - t0).count();
        long total = long(T) * kIters;
        std::printf("    %d readers × %d iters + writer @ 10Hz publish, aggregate=%.2f M/s\n"
                    "    hits=%ld  (期望 == total; SecurityManager map 尚未 refresh 所以旧数据仍在)\n",
                    T, kIters, total / sec / 1e6, hits.load());
    }
    std::printf("\n");

    shm_unlink(shmName.c_str());
    std::printf("=== done ===\n");
    return 0;
}
