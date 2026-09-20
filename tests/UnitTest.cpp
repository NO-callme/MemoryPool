// UnitTest.cpp —— 单元测试
//
// 覆盖：SizeClass、FreeList、PageCache、CentralCache、ThreadCache、MemoryPool（门面）。
// 对应 lyzself 的 tests/unit/test_*.cpp 合集。
//
#include "Common.h"
#include "PageCache.h"
#include "CentralCache.h"
#include "ThreadCache.h"
#include "MemoryPool.h"
#include "framework/TestFramework.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
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

// ============================================================================
// CentralCache
// ============================================================================

namespace
{
// 把一段 Batch 拆成指针数组，方便逐个检查。
std::vector<void*> toVector(const Batch& batch)
{
    std::vector<void*> blocks;
    void* node = batch.head;
    while (node != nullptr)
    {
        blocks.push_back(node);
        node = FreeList::next(node);
    }
    return blocks;
}

// 归还时需要现成的 Batch，这里从指针数组重建。
Batch fromVector(const std::vector<void*>& blocks)
{
    Batch batch;
    if (blocks.empty())
        return batch;

    for (size_t i = 0; i + 1 < blocks.size(); ++i)
        FreeList::next(blocks[i]) = blocks[i + 1];
    FreeList::next(blocks.back()) = nullptr;

    batch.head = blocks.front();
    batch.tail = blocks.back();
    batch.count = blocks.size();
    return batch;
}
} // namespace

TEST(CentralCache, FetchReturnsRequestedBatch)
{
    CentralCache& cache = CentralCache::getInstance();
    size_t index = SizeClass::getIndex(64);

    Batch batch = cache.fetchRange(index, 16);
    ASSERT_FALSE(batch.empty());
    ASSERT_EQ(batch.count, size_t(16));
    ASSERT_EQ(FreeList::length(batch.head), size_t(16));
    ASSERT_EQ(FreeList::next(batch.tail), static_cast<void*>(nullptr));

    cache.returnRange(index, batch);
}

TEST(CentralCache, BlocksAreDistinctAndWritable)
{
    CentralCache& cache = CentralCache::getInstance();
    size_t index = SizeClass::getIndex(128);
    size_t blockSize = SizeClass::sizeOfIndex(index);

    Batch batch = cache.fetchRange(index, 32);
    std::vector<void*> blocks = toVector(batch);
    ASSERT_EQ(blocks.size(), size_t(32));

    // 地址互不重复。
    std::set<void*> unique(blocks.begin(), blocks.end());
    ASSERT_EQ(unique.size(), blocks.size());

    // 每块整体可写，且互不覆盖。
    for (size_t i = 0; i < blocks.size(); ++i)
        std::memset(blocks[i], static_cast<int>(i + 1), blockSize);
    for (size_t i = 0; i < blocks.size(); ++i)
    {
        auto* bytes = static_cast<unsigned char*>(blocks[i]);
        ASSERT_EQ(bytes[0], static_cast<unsigned char>(i + 1));
        ASSERT_EQ(bytes[blockSize - 1], static_cast<unsigned char>(i + 1));
    }

    cache.returnRange(index, fromVector(blocks));
}

TEST(CentralCache, ReturnedBlocksComeBack)
{
    CentralCache& cache = CentralCache::getInstance();
    size_t index = SizeClass::getIndex(256);

    Batch batch = cache.fetchRange(index, 8);
    std::vector<void*> original = toVector(batch);

    size_t cachedBefore = cache.cachedCount(index);
    cache.returnRange(index, fromVector(original));
    ASSERT_EQ(cache.cachedCount(index), cachedBefore + 8);

    // 再取出来应该是刚归还的那些块。
    Batch again = cache.fetchRange(index, 8);
    std::vector<void*> reused = toVector(again);
    std::set<void*> originalSet(original.begin(), original.end());
    for (void* p : reused)
        ASSERT_TRUE(originalSet.count(p) > 0);

    cache.returnRange(index, fromVector(reused));
}

TEST(CentralCache, FetchClampsToWhatExists)
{
    CentralCache& cache = CentralCache::getInstance();
    size_t index = SizeClass::getIndex(512);

    // 请求量远大于一个 span 能切出的块数，实际拿到多少都可以，
    // 但计数必须与链表真实长度一致。
    Batch batch = cache.fetchRange(index, 100000);
    ASSERT_FALSE(batch.empty());
    ASSERT_EQ(batch.count, FreeList::length(batch.head));

    cache.returnRange(index, batch);
}

TEST(CentralCache, InvalidArgumentsAreRejected)
{
    CentralCache& cache = CentralCache::getInstance();

    ASSERT_TRUE(cache.fetchRange(Config::NUM_SIZE_CLASSES, 4).empty());
    ASSERT_TRUE(cache.fetchRange(0, 0).empty());

    cache.returnRange(Config::NUM_SIZE_CLASSES, Batch{}); // 不应崩溃
    cache.returnRange(0, Batch{});
}

TEST(CentralCache, LargestSizeClassYieldsAtLeastOneBlock)
{
    CentralCache& cache = CentralCache::getInstance();
    size_t index = Config::NUM_SIZE_CLASSES - 1;
    size_t blockSize = SizeClass::sizeOfIndex(index);

    Batch batch = cache.fetchRange(index, 1);
    ASSERT_FALSE(batch.empty());
    ASSERT_GE(batch.count, size_t(1));

    std::memset(batch.head, 0x11, blockSize); // 整块可写
    cache.returnRange(index, batch);
}

TEST(CentralCache, ConcurrentFetchNeverHandsOutSameBlock)
{
    CentralCache& cache = CentralCache::getInstance();
    size_t index = SizeClass::getIndex(64);
    constexpr size_t THREADS = 4;
    constexpr size_t ROUNDS = 100;
    constexpr size_t BATCH = 8;

    std::vector<std::thread> workers;
    std::vector<std::vector<void*>> perThread(THREADS);
    std::atomic<size_t> emptyFetches{0};

    for (size_t t = 0; t < THREADS; ++t)
    {
        workers.emplace_back([&, t]() {
            for (size_t r = 0; r < ROUNDS; ++r)
            {
                Batch batch = cache.fetchRange(index, BATCH);
                if (batch.empty())
                {
                    ++emptyFetches;
                    continue;
                }

                std::vector<void*> blocks = toVector(batch);
                // 持有期间写入本线程标记，若块被重复发放会互相踩踏。
                for (void* p : blocks)
                    std::memset(p, static_cast<int>(t + 1), 64);
                for (void* p : blocks)
                {
                    if (static_cast<unsigned char*>(p)[0] != static_cast<unsigned char>(t + 1))
                        perThread[t].push_back(p); // 记录被踩踏的块
                }

                cache.returnRange(index, fromVector(blocks));
            }
        });
    }
    for (std::thread& w : workers)
        w.join();

    ASSERT_EQ(emptyFetches.load(), size_t(0));
    for (size_t t = 0; t < THREADS; ++t)
        ASSERT_TRUE(perThread[t].empty());
}

// ============================================================================
// ThreadCache
// ============================================================================

TEST(ThreadCache, AllocateReturnsWritableBlock)
{
    ThreadCache& cache = ThreadCache::getInstance();

    void* ptr = cache.allocate(64);
    ASSERT_NE(ptr, static_cast<void*>(nullptr));

    std::memset(ptr, 0x7E, 64);
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[63], 0x7E);

    cache.deallocate(ptr, 64);
}

TEST(ThreadCache, AllocationsAreProperlyAligned)
{
    ThreadCache& cache = ThreadCache::getInstance();

    for (size_t size : {size_t(1), size_t(8), size_t(17), size_t(100), size_t(1000)})
    {
        void* ptr = cache.allocate(size);
        ASSERT_NE(ptr, static_cast<void*>(nullptr));
        ASSERT_EQ(reinterpret_cast<uintptr_t>(ptr) % Config::ALIGNMENT, uintptr_t(0));
        cache.deallocate(ptr, size);
    }
}

TEST(ThreadCache, FreedBlockIsImmediatelyReused)
{
    ThreadCache& cache = ThreadCache::getInstance();

    void* first = cache.allocate(128);
    cache.deallocate(first, 128);
    void* second = cache.allocate(128);

    // 刚还回来的块在链表头，下一次分配应该正好拿到它。
    ASSERT_EQ(first, second);
    cache.deallocate(second, 128);
}

TEST(ThreadCache, LiveBlocksNeverAlias)
{
    ThreadCache& cache = ThreadCache::getInstance();
    constexpr size_t COUNT = 2000;
    constexpr size_t SIZE = 48;

    std::vector<void*> blocks;
    blocks.reserve(COUNT);
    for (size_t i = 0; i < COUNT; ++i)
    {
        void* ptr = cache.allocate(SIZE);
        ASSERT_NE(ptr, static_cast<void*>(nullptr));
        std::memset(ptr, static_cast<int>(i & 0xFF), SIZE);
        blocks.push_back(ptr);
    }

    std::set<void*> unique(blocks.begin(), blocks.end());
    ASSERT_EQ(unique.size(), COUNT);

    // 内容没有被后续分配覆盖，说明块之间没有重叠。
    for (size_t i = 0; i < COUNT; ++i)
        ASSERT_EQ(static_cast<unsigned char*>(blocks[i])[0], static_cast<unsigned char>(i & 0xFF));

    for (void* p : blocks)
        cache.deallocate(p, SIZE);
}

TEST(ThreadCache, AllSizeClassesWork)
{
    ThreadCache& cache = ThreadCache::getInstance();

    for (size_t index = 0; index < Config::NUM_SIZE_CLASSES; ++index)
    {
        size_t blockSize = SizeClass::sizeOfIndex(index);

        void* ptr = cache.allocate(blockSize);
        if (ptr == nullptr)
            MP_FAIL("大小类 " + std::to_string(index) +
                    "（" + std::to_string(blockSize) + "B）分配失败");

        std::memset(ptr, 0x33, blockSize); // 整块可写
        cache.deallocate(ptr, blockSize);
    }
}

TEST(ThreadCache, HighWaterTriggersReturnToCentral)
{
    ThreadCache& cache = ThreadCache::getInstance();
    constexpr size_t SIZE = 32;
    constexpr size_t COUNT = 4000; // 远超初始高水位

    // 大量分配后一次性释放，桶会越过高水位并回吐给中心层。
    std::vector<void*> blocks;
    for (size_t i = 0; i < COUNT; ++i)
        blocks.push_back(cache.allocate(SIZE));
    for (void* p : blocks)
        cache.deallocate(p, SIZE);

    void* after = cache.allocate(SIZE);
    ASSERT_NE(after, static_cast<void*>(nullptr));
    std::memset(after, 0x01, SIZE);
    cache.deallocate(after, SIZE);
}

TEST(ThreadCache, ReleaseAllEmptiesBucketsAndStaysUsable)
{
    ThreadCache& cache = ThreadCache::getInstance();

    std::vector<void*> blocks;
    for (size_t i = 0; i < 200; ++i)
        blocks.push_back(cache.allocate(96));
    for (void* p : blocks)
        cache.deallocate(p, 96);

    cache.releaseAll();

    // 交还之后还能正常分配（会重新向中心层取货）。
    void* ptr = cache.allocate(96);
    ASSERT_NE(ptr, static_cast<void*>(nullptr));
    cache.deallocate(ptr, 96);
}

TEST(ThreadCache, EachThreadHasItsOwnCache)
{
    ThreadCache* mainCache = &ThreadCache::getInstance();
    ThreadCache* otherCache = nullptr;

    std::thread worker([&otherCache]() {
        otherCache = &ThreadCache::getInstance();
    });
    worker.join();

    ASSERT_NE(mainCache, otherCache);
}

TEST(ThreadCache, ConcurrentThreadsDoNotShareBlocks)
{
    constexpr size_t THREADS = 4;
    constexpr size_t PER_THREAD = 500;
    constexpr size_t SIZE = 64;

    std::vector<std::thread> workers;
    std::vector<size_t> corrupted(THREADS, 0);

    for (size_t t = 0; t < THREADS; ++t)
    {
        workers.emplace_back([t, &corrupted]() {
            ThreadCache& cache = ThreadCache::getInstance();
            std::vector<void*> blocks;
            blocks.reserve(PER_THREAD);

            for (size_t i = 0; i < PER_THREAD; ++i)
            {
                void* ptr = cache.allocate(SIZE);
                std::memset(ptr, static_cast<int>(t + 1), SIZE);
                blocks.push_back(ptr);
            }

            // 全部持有期间内容必须仍是本线程写的标记。
            for (void* p : blocks)
            {
                for (size_t b = 0; b < SIZE; ++b)
                {
                    if (static_cast<unsigned char*>(p)[b] != static_cast<unsigned char>(t + 1))
                    {
                        ++corrupted[t];
                        break;
                    }
                }
            }

            for (void* p : blocks)
                cache.deallocate(p, SIZE);
        });
    }
    for (std::thread& w : workers)
        w.join();

    for (size_t t = 0; t < THREADS; ++t)
        ASSERT_EQ(corrupted[t], size_t(0));
}

// ============================================================================
// MemoryPool（门面）
// ============================================================================

TEST(MemoryPool, AllocateAndDeallocateSmall)
{
    void* ptr = MemoryPool::allocate(100);
    ASSERT_NE(ptr, static_cast<void*>(nullptr));
    std::memset(ptr, 0x42, 100);
    MemoryPool::deallocate(ptr, 100);
}

TEST(MemoryPool, ZeroSizeYieldsUsableBlock)
{
    void* ptr = MemoryPool::allocate(0);
    ASSERT_NE(ptr, static_cast<void*>(nullptr));
    // 至少有 ALIGNMENT 字节可写。
    std::memset(ptr, 0x01, Config::ALIGNMENT);
    MemoryPool::deallocate(ptr, 0);
}

TEST(MemoryPool, DeallocateNullIsNoop)
{
    MemoryPool::deallocate(nullptr, 100);
    MemoryPool::deallocate(nullptr, 0);
}

TEST(MemoryPool, LargeAllocationBypassesCaches)
{
    size_t size = Config::MAX_SMALL_SIZE * 2; // 512KB，走大对象路径

    void* ptr = MemoryPool::allocate(size);
    ASSERT_NE(ptr, static_cast<void*>(nullptr));
    ASSERT_EQ(reinterpret_cast<uintptr_t>(ptr) % Config::PAGE_SIZE, uintptr_t(0));

    std::memset(ptr, 0x5A, size); // 整块可写
    ASSERT_EQ(static_cast<unsigned char*>(ptr)[size - 1], 0x5A);

    MemoryPool::deallocate(ptr, size);
}

TEST(MemoryPool, SizesAroundLargeThreshold)
{
    // 阈值两侧都要正确工作，且互不干扰。
    for (size_t size : {Config::MAX_SMALL_SIZE - 1,
                        Config::MAX_SMALL_SIZE,
                        Config::MAX_SMALL_SIZE + 1})
    {
        void* ptr = MemoryPool::allocate(size);
        if (ptr == nullptr)
            MP_FAIL("size=" + std::to_string(size) + " 分配失败");
        std::memset(ptr, 0x24, size);
        ASSERT_EQ(static_cast<unsigned char*>(ptr)[size - 1], 0x24);
        MemoryPool::deallocate(ptr, size);
    }
}

TEST(MemoryPool, MixedSizesNeverAlias)
{
    struct Live
    {
        void* ptr;
        size_t size;
        unsigned char tag;
    };

    const size_t sizes[] = {8, 16, 17, 64, 100, 255, 256, 1000, 4096, 20000, 100000};
    std::vector<Live> live;
    unsigned char tag = 1;

    for (int round = 0; round < 5; ++round)
    {
        for (size_t size : sizes)
        {
            void* ptr = MemoryPool::allocate(size);
            ASSERT_NE(ptr, static_cast<void*>(nullptr));
            std::memset(ptr, tag, size);
            live.push_back(Live{ptr, size, tag});
            tag = static_cast<unsigned char>(tag + 1 == 0 ? 1 : tag + 1);
        }
    }

    // 地址唯一。
    std::set<void*> unique;
    for (const Live& l : live)
        unique.insert(l.ptr);
    ASSERT_EQ(unique.size(), live.size());

    // 内容没被其他分配覆盖。
    for (const Live& l : live)
    {
        auto* bytes = static_cast<unsigned char*>(l.ptr);
        for (size_t i = 0; i < l.size; ++i)
        {
            if (bytes[i] != l.tag)
                MP_FAIL("size=" + std::to_string(l.size) + " 第 " + std::to_string(i) +
                        " 字节被覆盖");
        }
    }

    for (const Live& l : live)
        MemoryPool::deallocate(l.ptr, l.size);
}

namespace
{
struct Probe
{
    static int liveCount;
    int value;

    explicit Probe(int v) : value(v) { ++liveCount; }
    ~Probe() { --liveCount; }
};
int Probe::liveCount = 0;

struct Throwing
{
    Throwing() { throw std::runtime_error("boom"); }
};
} // namespace

TEST(MemoryPool, NewElementRunsConstructorAndDestructor)
{
    int before = Probe::liveCount;

    Probe* p = MemoryPool::newElement<Probe>(42);
    ASSERT_NE(p, static_cast<Probe*>(nullptr));
    ASSERT_EQ(p->value, 42);
    ASSERT_EQ(Probe::liveCount, before + 1);

    MemoryPool::deleteElement(p);
    ASSERT_EQ(Probe::liveCount, before);
}

TEST(MemoryPool, DeleteElementNullIsNoop)
{
    MemoryPool::deleteElement<Probe>(nullptr);
}

TEST(MemoryPool, ThrowingConstructorDoesNotLeak)
{
    bool caught = false;
    try
    {
        MemoryPool::newElement<Throwing>();
    }
    catch (const std::runtime_error&)
    {
        caught = true;
    }
    ASSERT_TRUE(caught);

    // 构造失败的那块内存应已回收，后续分配仍然正常。
    void* ptr = MemoryPool::allocate(sizeof(Throwing));
    ASSERT_NE(ptr, static_cast<void*>(nullptr));
    MemoryPool::deallocate(ptr, sizeof(Throwing));
}

TEST(MemoryPool, StatsTrackAllocations)
{
    Stats before = MemoryPool::getStats();

    constexpr size_t COUNT = 100;
    std::vector<void*> blocks;
    for (size_t i = 0; i < COUNT; ++i)
        blocks.push_back(MemoryPool::allocate(64));
    for (void* p : blocks)
        MemoryPool::deallocate(p, 64);

    Stats after = MemoryPool::getStats();

    ASSERT_GE(after.allocCount, before.allocCount + COUNT);
    ASSERT_GE(after.deallocCount, before.deallocCount + COUNT);
    ASSERT_GE(after.bytesAllocated, before.bytesAllocated + COUNT * 64);
    ASSERT_GT(after.systemBytes, uint64_t(0));
}

TEST(MemoryPool, StlAllocatorWorksWithVector)
{
    std::vector<int, Allocator<int>> numbers;
    for (int i = 0; i < 10000; ++i)
        numbers.push_back(i);

    ASSERT_EQ(numbers.size(), size_t(10000));
    for (int i = 0; i < 10000; ++i)
        ASSERT_EQ(numbers[static_cast<size_t>(i)], i);
}

TEST(MemoryPool, StlAllocatorWorksWithString)
{
    using PoolString = std::basic_string<char, std::char_traits<char>, Allocator<char>>;

    PoolString s;
    for (int i = 0; i < 1000; ++i)
        s.push_back(static_cast<char>('a' + (i % 26)));

    ASSERT_EQ(s.size(), size_t(1000));
    ASSERT_EQ(s[0], 'a');
}

TEST(MemoryPool, ReleaseThreadCacheKeepsPoolUsable)
{
    std::vector<void*> blocks;
    for (size_t i = 0; i < 500; ++i)
        blocks.push_back(MemoryPool::allocate(72));
    for (void* p : blocks)
        MemoryPool::deallocate(p, 72);

    MemoryPool::releaseThreadCache();

    void* ptr = MemoryPool::allocate(72);
    ASSERT_NE(ptr, static_cast<void*>(nullptr));
    MemoryPool::deallocate(ptr, 72);
}

TEST(MemoryPool, WorksAcrossThreads)
{
    constexpr size_t THREADS = 4;
    constexpr size_t PER_THREAD = 1000;

    std::vector<std::thread> workers;
    std::vector<size_t> failures(THREADS, 0);

    for (size_t t = 0; t < THREADS; ++t)
    {
        workers.emplace_back([t, &failures]() {
            std::vector<void*> blocks;
            for (size_t i = 0; i < PER_THREAD; ++i)
            {
                size_t size = 16 + (i % 500);
                void* ptr = MemoryPool::allocate(size);
                if (ptr == nullptr)
                {
                    ++failures[t];
                    continue;
                }
                std::memset(ptr, static_cast<int>(t + 1), size);
                blocks.push_back(ptr);
            }
            for (size_t i = 0; i < blocks.size(); ++i)
                MemoryPool::deallocate(blocks[i], 16 + (i % 500));
        });
    }
    for (std::thread& w : workers)
        w.join();

    for (size_t t = 0; t < THREADS; ++t)
        ASSERT_EQ(failures[t], size_t(0));
}

TEST(MemoryPool, AllocateInOneThreadFreeInAnother)
{
    constexpr size_t COUNT = 200;
    constexpr size_t SIZE = 128;

    std::vector<void*> blocks;
    for (size_t i = 0; i < COUNT; ++i)
    {
        void* ptr = MemoryPool::allocate(SIZE);
        ASSERT_NE(ptr, static_cast<void*>(nullptr));
        blocks.push_back(ptr);
    }

    // 跨线程释放：块会进入另一个线程的本地桶，最终回到中心层。
    std::thread freer([&blocks]() {
        for (void* p : blocks)
            MemoryPool::deallocate(p, SIZE);
    });
    freer.join();

    void* ptr = MemoryPool::allocate(SIZE);
    ASSERT_NE(ptr, static_cast<void*>(nullptr));
    MemoryPool::deallocate(ptr, SIZE);
}

int main()
{
    return ::testing::runAll();
}
