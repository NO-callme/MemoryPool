// UnitTest.cpp —— 单元测试
//
// 当前覆盖 Common.h 里的纯逻辑：SizeClass（大小类映射）与 FreeList（侵入式链表）。
// 对应 lyzself 的 tests/unit/test_sizeclass.cpp + test_freelist.cpp。
//
#include "Common.h"
#include "framework/TestFramework.h"

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

int main()
{
    return ::testing::runAll();
}
