#pragma once
#include "Common.h"

#include <map>
#include <mutex>

namespace mempool
{

// ============================================================================
// PageCache：最底层，以页为单位管理内存（对应 v2 的 PageCache.h）
//
// 职责：向系统申请大块内存（mmap），按需切分成 span；回收时合并相邻空闲 span。
// 并发：粗粒度互斥锁，调用频率低，锁开销可忽略。
// ============================================================================
class PageCache
{
public:
    static PageCache& getInstance() noexcept;

    void* allocateSpan(size_t numPages);
    void deallocateSpan(void* ptr, size_t numPages);
    size_t releaseFreeSpans();

    size_t totalPages() const noexcept;
    size_t freePages() const noexcept;

    ~PageCache();

    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;

private:
    PageCache() = default;

    struct Span
    {
        void*  start = nullptr;   // 起始地址（页对齐）
        size_t numPages = 0;      // 页数
        void*  chunk = nullptr;   // 所属系统内存块的起点
        bool   isFree = false;
        Span*  prev = nullptr;
        Span*  next = nullptr;
    };

    bool systemAlloc(size_t numPages);
    Span* takeFree(size_t numPages);
    void linkFree(Span* span);
    void unlinkFree(Span* span);
    void splitSpan(Span* span, size_t numPages);
    void coalesce(Span* span);

private:
    std::map<size_t, Span*> freeSpans_;  // 页数 -> 空闲 span 链表头
    std::map<void*, Span*> spanMap_;     // 起始地址 -> span
    std::map<void*, size_t> chunks_;     // 原始内存块：起点 -> 页数

    mutable std::mutex mutex_;
    size_t totalPages_ = 0;
    size_t freePages_ = 0;
};

} // namespace mempool
