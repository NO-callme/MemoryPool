// MemoryPool.cpp —— 实现见 MemoryPool.h
//
// 
//   allocateLarge / deallocateLarge / getStats / resetStats / releaseToSystem
//
#include "MemoryPool.h"
#include "PageCache.h"

namespace mempool
{

void* MemoryPool::allocateLarge(size_t size)
{
    size_t pages = (size + Config::PAGE_SIZE - 1) / Config::PAGE_SIZE;
    void* ptr = PageCache::getInstance().allocateSpan(pages);

    if (ptr != nullptr)
        GlobalStats::instance().addLargeAlloc();

    return ptr;
}

void MemoryPool::deallocateLarge(void* ptr, size_t size)
{
    size_t pages = (size + Config::PAGE_SIZE - 1) / Config::PAGE_SIZE;
    PageCache::getInstance().deallocateSpan(ptr, pages);
}


// ---------------------------------------------------------------------------
// 统计与维护
// ---------------------------------------------------------------------------

Stats MemoryPool::getStats()
{
    // 先把本线程的本地计数并进全局，读到的数才包含当前线程的最新活动。
    ThreadCache::getInstance().flushStats();
    return GlobalStats::instance().snapshot();
}

void MemoryPool::resetStats()
{
    ThreadCache::getInstance().flushStats();
    GlobalStats::instance().reset();
}

size_t MemoryPool::releaseToSystem()
{
    return PageCache::getInstance().releaseFreeSpans() * Config::PAGE_SIZE;
}

} // namespace mempool
