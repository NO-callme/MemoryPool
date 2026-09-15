#pragma once
#include "Common.h"

#include <array>
#include <mutex>
#include <vector>

namespace mempool
{

// ============================================================================
// CentralCache：跨线程共享的中间层（对应 v2 的 CentralCache.h）
//
// 职责：向 ThreadCache 批量供货 / 收货；向 PageCache 申请 span 并切分定长块。
// 并发：每个大小类一把自旋锁，不同尺寸互不阻塞。
// ============================================================================
class CentralCache
{
public:
    static CentralCache& getInstance() noexcept;

    Batch fetchRange(size_t index, size_t batchNum);
    void returnRange(size_t index, const Batch& batch);
    size_t cachedCount(size_t index) const noexcept;

    ~CentralCache();

    CentralCache(const CentralCache&) = delete;
    CentralCache& operator=(const CentralCache&) = delete;

private:
    CentralCache();

    Batch refill(size_t index, size_t batchNum);

private:
    struct alignas(Config::CACHE_LINE_SIZE) Bucket
    {
        SpinLock lock;
        void*    freeList = nullptr;
        size_t   count = 0;
    };

    std::array<Bucket, Config::NUM_SIZE_CLASSES> buckets_{};

    struct CarvedSpan
    {
        void*  start;
        size_t numPages;
    };
    std::vector<CarvedSpan> carvedSpans_;
    std::mutex spansMutex_;
};

} // namespace mempool
