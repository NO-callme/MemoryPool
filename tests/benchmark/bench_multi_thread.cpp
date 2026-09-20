//
// 多线程基准：这是内存池真正要赢的战场
//
// ThreadCache 无锁，线程数上去以后 new/delete 的全局锁竞争会越来越明显。
//
#include "MemoryPool.h"
#include "framework/Benchmark.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace mempool;

namespace
{
constexpr size_t OPS_PER_THREAD = 200000;

// 每个线程独立分配释放，无共享。
void benchIndependent()
{
    bench::Table table("多线程 · 各线程独立分配释放（每线程 20 万次 × 32B）");

    for (size_t threads : {size_t(1), size_t(2), size_t(4), size_t(8)})
    {
        double poolMs = bench::measureThreads(threads, [](size_t) {
            for (size_t i = 0; i < OPS_PER_THREAD; ++i)
            {
                void* ptr = MemoryPool::allocate(32);
                bench::keep(ptr);
                MemoryPool::deallocate(ptr, 32);
            }
        });

        double newMs = bench::measureThreads(threads, [](size_t) {
            for (size_t i = 0; i < OPS_PER_THREAD; ++i)
            {
                char* ptr = new char[32];
                bench::keep(ptr);
                delete[] ptr;
            }
        });

        table.row(std::to_string(threads) + " 线程", poolMs, newMs);
    }
}

// 混合尺寸，更接近真实负载。
void benchMixedSizes()
{
    bench::Table table("多线程 · 混合尺寸（8B ~ 1KB）");

    for (size_t threads : {size_t(1), size_t(2), size_t(4), size_t(8)})
    {
        double poolMs = bench::measureThreads(threads, [](size_t t) {
            size_t seed = t * 7919 + 1;
            for (size_t i = 0; i < OPS_PER_THREAD; ++i)
            {
                seed = seed * 1103515245 + 12345;
                size_t size = 8 + (seed >> 16) % 1016;
                void* ptr = MemoryPool::allocate(size);
                bench::keep(ptr);
                MemoryPool::deallocate(ptr, size);
            }
        });

        double newMs = bench::measureThreads(threads, [](size_t t) {
            size_t seed = t * 7919 + 1;
            for (size_t i = 0; i < OPS_PER_THREAD; ++i)
            {
                seed = seed * 1103515245 + 12345;
                size_t size = 8 + (seed >> 16) % 1016;
                char* ptr = new char[size];
                bench::keep(ptr);
                delete[] ptr;
            }
        });

        table.row(std::to_string(threads) + " 线程", poolMs, newMs);
    }
}

// 批量持有：每线程先攒一批再统一释放。
void benchBatchHold()
{
    bench::Table table("多线程 · 批量持有 1000 个对象（64B）");

    constexpr size_t BATCH = 1000;
    constexpr size_t ROUNDS = 200;

    for (size_t threads : {size_t(1), size_t(2), size_t(4), size_t(8)})
    {
        double poolMs = bench::measureThreads(threads, [](size_t) {
            std::vector<void*> blocks(BATCH);
            for (size_t r = 0; r < ROUNDS; ++r)
            {
                for (size_t i = 0; i < BATCH; ++i)
                    blocks[i] = MemoryPool::allocate(64);
                for (size_t i = 0; i < BATCH; ++i)
                    MemoryPool::deallocate(blocks[i], 64);
            }
        });

        double newMs = bench::measureThreads(threads, [](size_t) {
            std::vector<char*> blocks(BATCH);
            for (size_t r = 0; r < ROUNDS; ++r)
            {
                for (size_t i = 0; i < BATCH; ++i)
                    blocks[i] = new char[64];
                for (size_t i = 0; i < BATCH; ++i)
                    delete[] blocks[i];
            }
        });

        table.row(std::to_string(threads) + " 线程", poolMs, newMs);
    }
}
} // namespace

int main()
{
    std::printf("========== 多线程基准测试 ==========\n");
    std::printf("（本机 CPU 核心数: %u）\n", std::thread::hardware_concurrency());

    benchIndependent();
    benchMixedSizes();
    benchBatchHold();

    Stats stats = MemoryPool::getStats();
    std::printf("\n系统内存 %.2f MB，在用 span %lu 个\n",
                static_cast<double>(stats.systemBytes) / (1024.0 * 1024.0),
                static_cast<unsigned long>(stats.spansInUse));
    return 0;
}
