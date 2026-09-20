//
// 压力测试
//
// 随机尺寸、随机生命周期、多线程混跑，每块内存写满特征字节并在释放前校验。
// 目的是把单元测试覆盖不到的时序问题、越界和重复发放暴露出来。
//
#include "MemoryPool.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace mempool;

namespace
{
constexpr size_t THREADS = 4;
constexpr size_t OPS_PER_THREAD = 200000;
constexpr size_t MAX_LIVE = 2000;

std::atomic<size_t> g_errors{0};
std::atomic<size_t> g_allocFailures{0};
std::atomic<size_t> g_totalOps{0};

struct Block
{
    void*         ptr;
    size_t        size;
    unsigned char tag;
};

// 写入特征字节。tag 由指针与尺寸派生，任何越界写都会破坏邻居的特征。
void fill(Block& block)
{
    std::memset(block.ptr, block.tag, block.size);
}

bool verify(const Block& block)
{
    auto* bytes = static_cast<const unsigned char*>(block.ptr);
    for (size_t i = 0; i < block.size; ++i)
    {
        if (bytes[i] != block.tag)
        {
            std::printf("[错误] 内存被破坏: ptr=%p size=%zu 偏移=%zu 期望=%u 实际=%u\n",
                        block.ptr, block.size, i, block.tag, bytes[i]);
            return false;
        }
    }
    return true;
}

// 随机尺寸，按阶段1 定的分布：小对象占多数，偶尔来个大对象。
size_t randomSize(std::mt19937& rng)
{
    std::uniform_int_distribution<int> bucket(0, 99);
    int roll = bucket(rng);

    if (roll < 60)
        return std::uniform_int_distribution<size_t>(8, 64)(rng);
    if (roll < 90)
        return std::uniform_int_distribution<size_t>(65, 256)(rng);
    if (roll < 99)
        return std::uniform_int_distribution<size_t>(257, 4096)(rng);
    return std::uniform_int_distribution<size_t>(4097, 512 * 1024)(rng); // 含大对象路径
}

void worker(size_t threadId)
{
    std::mt19937 rng(static_cast<unsigned>(threadId * 6364136223846793005ULL + 1));
    std::vector<Block> live;
    live.reserve(MAX_LIVE);

    unsigned char tag = static_cast<unsigned char>(threadId + 1);

    for (size_t op = 0; op < OPS_PER_THREAD; ++op)
    {
        bool doAlloc = live.empty() ||
                       (live.size() < MAX_LIVE &&
                        std::uniform_int_distribution<int>(0, 99)(rng) < 55);

        if (doAlloc)
        {
            Block block;
            block.size = randomSize(rng);
            block.ptr = MemoryPool::allocate(block.size);
            if (block.ptr == nullptr)
            {
                ++g_allocFailures;
                continue;
            }

            tag = static_cast<unsigned char>(tag == 255 ? 1 : tag + 1);
            block.tag = tag;
            fill(block);
            live.push_back(block);
        }
        else
        {
            // 随机挑一个释放，制造乱序生命周期。
            size_t victim = std::uniform_int_distribution<size_t>(0, live.size() - 1)(rng);
            std::swap(live[victim], live.back());
            Block block = live.back();
            live.pop_back();

            if (!verify(block))
                ++g_errors;

            MemoryPool::deallocate(block.ptr, block.size);
        }

        ++g_totalOps;
    }

    // 收尾：剩下的全部校验并释放。
    for (const Block& block : live)
    {
        if (!verify(block))
            ++g_errors;
        MemoryPool::deallocate(block.ptr, block.size);
    }
}

// 反复创建销毁线程，检查 thread_local 缓存的建立与回收是否稳定。
void churnThreads()
{
    std::printf("\n[2/2] 线程反复创建销毁（40 轮 × 4 线程）\n");

    for (size_t round = 0; round < 40; ++round)
    {
        std::vector<std::thread> workers;
        for (size_t t = 0; t < 4; ++t)
        {
            workers.emplace_back([]() {
                std::vector<void*> blocks;
                for (size_t i = 0; i < 2000; ++i)
                {
                    size_t size = 16 + (i % 300);
                    void* ptr = MemoryPool::allocate(size);
                    if (ptr == nullptr)
                    {
                        ++g_allocFailures;
                        continue;
                    }
                    std::memset(ptr, 0x77, size);
                    blocks.push_back(ptr);
                }
                for (size_t i = 0; i < blocks.size(); ++i)
                    MemoryPool::deallocate(blocks[i], 16 + (i % 300));
                // 线程在此退出，ThreadCache 析构，内存应全部回到中心层。
            });
        }
        for (std::thread& w : workers)
            w.join();
    }
    std::printf("      完成\n");
}
} // namespace

int main()
{
    std::printf("========== 压力测试 ==========\n");
    std::printf("[1/2] 随机负载: %zu 线程 × %zu 次操作\n", THREADS, OPS_PER_THREAD);

    std::vector<std::thread> workers;
    for (size_t t = 0; t < THREADS; ++t)
        workers.emplace_back(worker, t);
    for (std::thread& w : workers)
        w.join();
    std::printf("      完成 %zu 次操作\n", g_totalOps.load());

    churnThreads();

    Stats stats = MemoryPool::getStats();
    std::printf("\n---------- 结果 ----------\n");
    std::printf("内存破坏      : %zu\n", g_errors.load());
    std::printf("分配失败      : %zu\n", g_allocFailures.load());
    std::printf("累计分配      : %lu 次\n", static_cast<unsigned long>(stats.allocCount));
    std::printf("累计释放      : %lu 次\n", static_cast<unsigned long>(stats.deallocCount));
    std::printf("系统内存      : %.2f MB\n",
                static_cast<double>(stats.systemBytes) / (1024.0 * 1024.0));
    std::printf("在用 span     : %lu\n", static_cast<unsigned long>(stats.spansInUse));

    size_t released = MemoryPool::releaseToSystem();
    std::printf("归还系统      : %.2f MB\n", static_cast<double>(released) / (1024.0 * 1024.0));

    bool ok = g_errors.load() == 0 && g_allocFailures.load() == 0;
    std::printf("\n%s\n", ok ? "压力测试通过" : "压力测试失败");
    return ok ? 0 : 1;
}
