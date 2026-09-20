//
// 单线程基准：内存池 vs new/delete
//
#include "MemoryPool.h"
#include "framework/Benchmark.h"

#include <cstdio>
#include <vector>

using namespace mempool;

namespace
{
constexpr size_t ITERATIONS = 1000000;

// 分配后立刻释放。最坏情况：new/delete 的缓存也是热的。
void benchAllocFree()
{
    bench::Table table("单线程 · 分配后立即释放（各 100 万次）");

    for (size_t size : {size_t(8), size_t(32), size_t(64), size_t(256), size_t(1024)})
    {
        double poolMs = bench::measure([size]() {
            void* ptr = MemoryPool::allocate(size);
            bench::keep(ptr);
            MemoryPool::deallocate(ptr, size);
        }, ITERATIONS);

        double newMs = bench::measure([size]() {
            char* ptr = new char[size];
            bench::keep(ptr);
            delete[] ptr;
        }, ITERATIONS);

        table.row(std::to_string(size) + "B", poolMs, newMs);
    }
}

// 先批量分配再批量释放。更接近真实对象生命周期。
void benchBatch()
{
    bench::Table table("单线程 · 批量分配后批量释放（10 万个对象 × 10 轮）");

    constexpr size_t BATCH = 100000;
    constexpr size_t ROUNDS = 10;

    for (size_t size : {size_t(16), size_t(64), size_t(256)})
    {
        std::vector<void*> pool(BATCH);
        double poolMs = bench::measureOnce([&]() {
            for (size_t r = 0; r < ROUNDS; ++r)
            {
                for (size_t i = 0; i < BATCH; ++i)
                    pool[i] = MemoryPool::allocate(size);
                for (size_t i = 0; i < BATCH; ++i)
                    MemoryPool::deallocate(pool[i], size);
            }
        });

        std::vector<char*> raw(BATCH);
        double newMs = bench::measureOnce([&]() {
            for (size_t r = 0; r < ROUNDS; ++r)
            {
                for (size_t i = 0; i < BATCH; ++i)
                    raw[i] = new char[size];
                for (size_t i = 0; i < BATCH; ++i)
                    delete[] raw[i];
            }
        });

        table.row(std::to_string(size) + "B × " + std::to_string(BATCH), poolMs, newMs);
    }
}

// 单次操作延迟，对照阶段1 定的 ≤20ns 指标。
void benchLatency()
{
    std::printf("\n=== 单次操作延迟（目标 ≤ 20ns）===\n");
    std::printf("%-12s %14s %14s\n", "尺寸", "内存池(ns)", "new(ns)");
    std::printf("%s\n", std::string(42, '-').c_str());

    for (size_t size : {size_t(8), size_t(32), size_t(64), size_t(128), size_t(256)})
    {
        double poolMs = bench::measure([size]() {
            void* ptr = MemoryPool::allocate(size);
            bench::keep(ptr);
            MemoryPool::deallocate(ptr, size);
        }, ITERATIONS);

        double newMs = bench::measure([size]() {
            char* ptr = new char[size];
            bench::keep(ptr);
            delete[] ptr;
        }, ITERATIONS);

        std::printf("%-12s %14.2f %14.2f\n",
                    (std::to_string(size) + "B").c_str(),
                    bench::nsPerOp(poolMs, ITERATIONS),
                    bench::nsPerOp(newMs, ITERATIONS));
    }
}
} // namespace

int main()
{
    std::printf("========== 单线程基准测试 ==========\n");

    // 预热：让慢启动进入热路径，避免把冷启动开销算进结果。
    for (size_t i = 0; i < 100000; ++i)
    {
        void* ptr = MemoryPool::allocate(64);
        MemoryPool::deallocate(ptr, 64);
    }

    benchAllocFree();
    benchBatch();
    benchLatency();

    Stats stats = MemoryPool::getStats();
    std::printf("\n分配次数 %lu，ThreadCache 命中率 %.2f%%，系统内存 %.2f MB\n",
                static_cast<unsigned long>(stats.allocCount),
                stats.hitRate() * 100.0,
                static_cast<double>(stats.systemBytes) / (1024.0 * 1024.0));
    return 0;
}
