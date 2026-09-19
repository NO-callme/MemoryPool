// UnitTest.cpp —— 单元测试
//
// 覆盖：SizeClass（大小类映射）、FreeList（侵入式链表）、PageCache（页级管理）。
// 对应 lyzself 的 tests/unit/test_sizeclass.cpp + test_freelist.cpp + test_pagecache.cpp。
//
#include "Common.h"
#include "PageCache.h"
#include "framework/TestFramework.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

using namespace mempool;

// ============================================================================
// SizeClass
// ============================================================================

TEST(SizeClass, RoundUpAlignsToClassSize)
{
    ASSERT_EQ(SizeClass::roundUp(0), size_t(16));
    ASSERT_EQ(SizeClass::roundUp(1), size_t(16));
    ASSERT_EQ(SizeClass::roundUp(16), size_t(16));
    ASSERT_EQ(SizeClass::roundUp(17), size_t(32));
    ASSERT_EQ(SizeClass::roundUp(31), size_t(32));
    ASSERT_EQ(SizeClass::roundUp(1024), size_t(1024));
    ASSERT_EQ(SizeClass::roundUp(1025), size_t(1152)); // 1KB + 128B 步长
}

TEST(SizeClass, GetIndexAtGroupBoundaries)
{
    ASSERT_EQ(SizeClass::getIndex(1), size_t(0));
    ASSERT_EQ(SizeClass::getIndex(16), size_t(0));
    ASSERT_EQ(SizeClass::getIndex(17), size_t(1));
    ASSERT_EQ(SizeClass::getIndex(32), size_t(1));

    ASSERT_EQ(SizeClass::getIndex(Config::G0_MAX), Config::G1_BASE - 1);
    ASSERT_EQ(SizeClass::getIndex(Config::G0_MAX + 1), Config::G1_BASE);
    ASSERT_EQ(SizeClass::getIndex(Config::G1_MAX), Config::G2_BASE - 1);
    ASSERT_EQ(SizeClass::getIndex(Config::G1_MAX + 1), Config::G2_BASE);
    ASSERT_EQ(SizeClass::getIndex(Config::G2_MAX), Config::G3_BASE - 1);
    ASSERT_EQ(SizeClass::getIndex(Config::G2_MAX + 1), Config::G3_BASE);
    ASSERT_EQ(SizeClass::getIndex(Config::MAX_SMALL_SIZE), Config::NUM_SIZE_CLASSES - 1);
}

TEST(SizeClass, IndexIsMonotonicAndInBounds)
{
    size_t prev = 0;
    for (size_t bytes = 1; bytes <= Config::MAX_SMALL_SIZE; ++bytes)
    {
        size_t index = SizeClass::getIndex(bytes);
        ASSERT_LT(index, Config::NUM_SIZE_CLASSES);
        ASSERT_GE(index, prev);
        prev = index;
    }
}

TEST(SizeClass, RoundTripConsistency)
{
    for (size_t bytes = 1; bytes <= Config::MAX_SMALL_SIZE; ++bytes)
    {
        size_t blockSize = SizeClass::sizeOfIndex(SizeClass::getIndex(bytes));
        ASSERT_EQ(blockSize, SizeClass::roundUp(bytes));
        ASSERT_GE(blockSize, bytes);
    }
}

TEST(SizeClass, EveryClassIsReachable)
{
    for (size_t index = 0; index < Config::NUM_SIZE_CLASSES; ++index)
    {
        size_t blockSize = SizeClass::sizeOfIndex(index);
        ASSERT_EQ(SizeClass::getIndex(blockSize), index);
        ASSERT_GE(blockSize, Config::ALIGNMENT);
        ASSERT_LE(blockSize, Config::MAX_SMALL_SIZE);
    }
}

TEST(SizeClass, InternalFragmentationUnder13Percent)
{
    // 设计目标：块内浪费不超过 12.5%（步长 ≈ 尺寸的 1/8）。
    // 该保证只在尺寸 ≥ 8×步长 时成立；最小组 [16,128) 用 16B 步长，
    // 碎片最高可达 ~47%，属于用空间换速度的合理代价，故从 128B 起检查。
    constexpr size_t START = 8 * Config::ALIGNMENT; // 128B
    for (size_t bytes = START; bytes <= Config::MAX_SMALL_SIZE; ++bytes)
    {
        size_t blockSize = SizeClass::roundUp(bytes);
        double waste = static_cast<double>(blockSize - bytes) / static_cast<double>(blockSize);
        if (waste > 0.125)
            MP_FAIL("size=" + std::to_string(bytes) +
                    " block=" + std::to_string(blockSize) +
                    " 浪费=" + std::to_string(waste * 100) + "%");
    }
}

TEST(SizeClass, BlockSizeHoldsNextPointer)
{
    ASSERT_GE(SizeClass::sizeOfIndex(0), sizeof(void*));
}

TEST(SizeClass, SpanPagesFitsRequestedBlocks)
{
    for (size_t index = 0; index < Config::NUM_SIZE_CLASSES; ++index)
    {
        size_t blockSize = SizeClass::sizeOfIndex(index);
        size_t pages = SizeClass::spanPagesFor(blockSize, 16);

        ASSERT_GE(pages, Config::DEFAULT_SPAN_PAGES);
        ASSERT_GE(pages * Config::PAGE_SIZE, blockSize);
    }
}

TEST(SizeClass, IsSmallMatchesThreshold)
{
    ASSERT_TRUE(SizeClass::isSmall(Config::MAX_SMALL_SIZE));
    ASSERT_FALSE(SizeClass::isSmall(Config::MAX_SMALL_SIZE + 1));
}

// ============================================================================
// FreeList
// ============================================================================

namespace
{
struct Arena
{
    static constexpr size_t BLOCK = 32;
    std::vector<char> storage;

    explicit Arena(size_t blocks) : storage(blocks * BLOCK) {}

    void* base() { return storage.data(); }
    size_t bytes() const { return storage.size(); }
    void* block(size_t i) { return storage.data() + i * BLOCK; }
};
} // namespace

TEST(FreeList, PushPopIsLifo)
{
    Arena arena(3);
    void* head = nullptr;

    FreeList::push(head, arena.block(0));
    FreeList::push(head, arena.block(1));
    FreeList::push(head, arena.block(2));

    ASSERT_EQ(FreeList::length(head), size_t(3));
    ASSERT_EQ(FreeList::pop(head), arena.block(2));
    ASSERT_EQ(FreeList::pop(head), arena.block(1));
    ASSERT_EQ(FreeList::pop(head), arena.block(0));
    ASSERT_EQ(head, static_cast<void*>(nullptr));
}

TEST(FreeList, PopFromEmptyReturnsNull)
{
    void* head = nullptr;
    ASSERT_EQ(FreeList::pop(head), static_cast<void*>(nullptr));
}

TEST(FreeList, BuildChainsWholeArena)
{
    Arena arena(10);
    Batch batch = FreeList::build(arena.base(), arena.bytes(), Arena::BLOCK);

    ASSERT_EQ(batch.count, size_t(10));
    ASSERT_EQ(batch.head, arena.block(0));
    ASSERT_EQ(batch.tail, arena.block(9));
    ASSERT_EQ(FreeList::length(batch.head), size_t(10));
    ASSERT_EQ(FreeList::next(batch.tail), static_cast<void*>(nullptr));
}

TEST(FreeList, BuildWithInsufficientBytesYieldsEmpty)
{
    Arena arena(1);
    Batch batch = FreeList::build(arena.base(), Arena::BLOCK - 1, Arena::BLOCK);
    ASSERT_TRUE(batch.empty());
    ASSERT_EQ(batch.count, size_t(0));
}

TEST(FreeList, PopRangeTakesExactCount)
{
    Arena arena(10);
    Batch all = FreeList::build(arena.base(), arena.bytes(), Arena::BLOCK);

    void* head = all.head;
    Batch taken = FreeList::popRange(head, 4);

    ASSERT_EQ(taken.count, size_t(4));
    ASSERT_EQ(FreeList::length(taken.head), size_t(4));
    ASSERT_EQ(FreeList::length(head), size_t(6));
    ASSERT_EQ(taken.tail, arena.block(3));
    ASSERT_EQ(head, arena.block(4));
}

TEST(FreeList, PopRangeClampsToAvailable)
{
    Arena arena(3);
    Batch all = FreeList::build(arena.base(), arena.bytes(), Arena::BLOCK);

    void* head = all.head;
    Batch taken = FreeList::popRange(head, 100);

    ASSERT_EQ(taken.count, size_t(3));
    ASSERT_EQ(head, static_cast<void*>(nullptr));
}

TEST(FreeList, PushRangeSplicesInFront)
{
    Arena arena(6);
    Batch first = FreeList::build(arena.base(), 3 * Arena::BLOCK, Arena::BLOCK);
    Batch second = FreeList::build(arena.block(3), 3 * Arena::BLOCK, Arena::BLOCK);

    void* head = first.head;
    FreeList::pushRange(head, second);

    ASSERT_EQ(head, second.head);
    ASSERT_EQ(FreeList::length(head), size_t(6));
}

TEST(FreeList, SplitAfterCutsAtBoundary)
{
    Arena arena(10);
    Batch all = FreeList::build(arena.base(), arena.bytes(), Arena::BLOCK);

    void* keptTail = nullptr;
    Batch rest = FreeList::splitAfter(all.head, 4, &keptTail);

    ASSERT_EQ(keptTail, arena.block(3));
    ASSERT_EQ(FreeList::length(all.head), size_t(4));
    ASSERT_EQ(rest.count, size_t(6));
    ASSERT_EQ(rest.head, arena.block(4));
    ASSERT_EQ(rest.tail, arena.block(9));
}

TEST(FreeList, SplitAfterWholeListLeavesNothing)
{
    Arena arena(4);
    Batch all = FreeList::build(arena.base(), arena.bytes(), Arena::BLOCK);

    void* keptTail = nullptr;
    Batch rest = FreeList::splitAfter(all.head, 4, &keptTail);

    ASSERT_TRUE(rest.empty());
    ASSERT_EQ(keptTail, arena.block(3));
    ASSERT_EQ(FreeList::length(all.head), size_t(4));
}

TEST(FreeList, SplitAfterBeyondLengthKeepsListIntact)
{
    Arena arena(3);
    Batch all = FreeList::build(arena.base(), arena.bytes(), Arena::BLOCK);

    void* keptTail = nullptr;
    Batch rest = FreeList::splitAfter(all.head, 99, &keptTail);

    ASSERT_TRUE(rest.empty());
    ASSERT_EQ(FreeList::length(all.head), size_t(3));
}

// ============================================================================
// PageCache
// ============================================================================

// PageCache 是单例，测试之间共享状态，因此断言都基于增量而非绝对值。

TEST(PageCache, AllocateReturnsUsablePageAlignedMemory)
{
    PageCache& cache = PageCache::getInstance();

    void* ptr = cache.allocateSpan(1);
    ASSERT_NE(ptr, static_cast<void*>(nullptr));

    // 页对齐。
    ASSERT_EQ(reinterpret_cast<uintptr_t>(ptr) % Config::PAGE_SIZE, uintptr_t(0));

    // 整页都必须真的可写。
    std::memset(ptr, 0xAB, Config::PAGE_SIZE);
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[Config::PAGE_SIZE - 1], 0xAB);

    cache.deallocateSpan(ptr, 1);
}

TEST(PageCache, ZeroPagesTreatedAsOne)
{
    PageCache& cache = PageCache::getInstance();

    void* ptr = cache.allocateSpan(0);
    ASSERT_NE(ptr, static_cast<void*>(nullptr));
    cache.deallocateSpan(ptr, 1);
}

TEST(PageCache, DistinctSpansDoNotOverlap)
{
    PageCache& cache = PageCache::getInstance();
    constexpr size_t COUNT = 32;
    constexpr size_t PAGES = 2;

    std::vector<void*> spans;
    for (size_t i = 0; i < COUNT; ++i)
    {
        void* ptr = cache.allocateSpan(PAGES);
        ASSERT_NE(ptr, static_cast<void*>(nullptr));
        spans.push_back(ptr);
    }

    // 地址互不相同，且区间互不重叠。
    std::set<void*> unique(spans.begin(), spans.end());
    ASSERT_EQ(unique.size(), COUNT);

    std::vector<char*> sorted;
    for (void* p : spans)
        sorted.push_back(static_cast<char*>(p));
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i)
        ASSERT_GE(sorted[i], sorted[i - 1] + PAGES * Config::PAGE_SIZE);

    // 每段都可写，确认没有把同一片内存发给两个调用方。
    for (size_t i = 0; i < spans.size(); ++i)
        std::memset(spans[i], static_cast<int>(i & 0xFF), PAGES * Config::PAGE_SIZE);
    for (size_t i = 0; i < spans.size(); ++i)
        ASSERT_EQ(static_cast<unsigned char*>(spans[i])[0], static_cast<unsigned char>(i & 0xFF));

    for (void* p : spans)
        cache.deallocateSpan(p, PAGES);
}

TEST(PageCache, FreedSpanIsReused)
{
    PageCache& cache = PageCache::getInstance();

    // 先把空闲池预热，之后的分配都应该复用而不是继续向系统要。
    void* warm = cache.allocateSpan(4);
    cache.deallocateSpan(warm, 4);

    size_t before = cache.totalPages();
    for (int i = 0; i < 20; ++i)
    {
        void* ptr = cache.allocateSpan(4);
        ASSERT_NE(ptr, static_cast<void*>(nullptr));
        cache.deallocateSpan(ptr, 4);
    }
    ASSERT_EQ(cache.totalPages(), before); // 没有新增系统内存
}

TEST(PageCache, AdjacentSpansCoalesce)
{
    PageCache& cache = PageCache::getInstance();

    // 切出三段相邻的小 span，全部归还后应能合并成一段大的。
    void* a = cache.allocateSpan(2);
    void* b = cache.allocateSpan(2);
    void* c = cache.allocateSpan(2);
    ASSERT_NE(a, static_cast<void*>(nullptr));
    ASSERT_NE(b, static_cast<void*>(nullptr));
    ASSERT_NE(c, static_cast<void*>(nullptr));

    cache.deallocateSpan(a, 2);
    cache.deallocateSpan(b, 2);
    cache.deallocateSpan(c, 2);

    size_t totalBefore = cache.totalPages();

    // 若没合并，6 页的请求就得向系统另开一块。
    void* big = cache.allocateSpan(6);
    ASSERT_NE(big, static_cast<void*>(nullptr));
    ASSERT_EQ(cache.totalPages(), totalBefore);

    cache.deallocateSpan(big, 6);
}

TEST(PageCache, LargeSpanSplitsFromBigChunk)
{
    PageCache& cache = PageCache::getInstance();

    // 超过 MIN_SYSTEM_PAGES 的请求也要能满足。
    size_t pages = Config::MIN_SYSTEM_PAGES * 2;
    void* ptr = cache.allocateSpan(pages);
    ASSERT_NE(ptr, static_cast<void*>(nullptr));

    std::memset(ptr, 0x5A, pages * Config::PAGE_SIZE);
    cache.deallocateSpan(ptr, pages);
}

TEST(PageCache, DeallocateNullAndUnknownPointerIsSafe)
{
    PageCache& cache = PageCache::getInstance();

    cache.deallocateSpan(nullptr, 1); // 不应崩溃

    int onStack = 0;
    cache.deallocateSpan(&onStack, 1); // 不是本层发出的地址，忽略即可
}

TEST(PageCache, DoubleFreeIsIgnored)
{
    PageCache& cache = PageCache::getInstance();

    void* ptr = cache.allocateSpan(2);
    cache.deallocateSpan(ptr, 2);

    size_t freeBefore = cache.freePages();
    cache.deallocateSpan(ptr, 2); // 第二次归还必须被识别并丢弃
    ASSERT_EQ(cache.freePages(), freeBefore);
}

TEST(PageCache, ConcurrentAllocateIsSafe)
{
    PageCache& cache = PageCache::getInstance();
    constexpr size_t THREADS = 4;
    constexpr size_t PER_THREAD = 50;

    std::vector<std::thread> workers;
    std::atomic<size_t> failures{0};

    for (size_t t = 0; t < THREADS; ++t)
    {
        workers.emplace_back([&cache, &failures]() {
            std::vector<void*> local;
            for (size_t i = 0; i < PER_THREAD; ++i)
            {
                void* ptr = cache.allocateSpan(1);
                if (ptr == nullptr)
                {
                    ++failures;
                    continue;
                }
                std::memset(ptr, 0xCD, Config::PAGE_SIZE);
                local.push_back(ptr);
            }
            for (void* p : local)
                cache.deallocateSpan(p, 1);
        });
    }
    for (std::thread& w : workers)
        w.join();

    ASSERT_EQ(failures.load(), size_t(0));
}

int main()
{
    return ::testing::runAll();
}
