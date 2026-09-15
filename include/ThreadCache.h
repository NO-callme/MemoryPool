#pragma once
#include "Common.h"

#include <array>

namespace mempool
{

// ============================================================================
// ThreadCache：线程本地缓存，分配路径的第一站（对应 v2 的 ThreadCache.h）
//
// 职责：小对象无锁快路径；向 CentralCache 批量取货（慢启动）、批量回吐（高水位）。
// 并发：thread_local，天生无锁。
// ============================================================================
class alignas(Config::CACHE_LINE_SIZE) ThreadCache
{
public:
    static ThreadCache& getInstance() noexcept;

    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);
    void releaseAll();

    const ThreadLocalStats& stats() const noexcept;
    void flushStats() noexcept;

    ~ThreadCache();

    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;

private:
    ThreadCache();

    void* fetchFromCentralCache(size_t index, size_t blockSize);
    void returnToCentralCache(size_t index);
    size_t getBatchNum(size_t index, size_t blockSize) const noexcept;
    void growHighWater(size_t index, size_t blockSize) noexcept;

private:
    struct Bucket
    {
        void*    freeList = nullptr;
        uint32_t count = 0;
        uint32_t highWater = 0;
        uint32_t allocCount = 0;
    };

    std::array<Bucket, Config::NUM_SIZE_CLASSES> buckets_{};
    ThreadLocalStats stats_{};
};

} // namespace mempool
