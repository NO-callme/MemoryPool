#pragma once
// ============================================================================
// Common.h —— 所有层共享的公共定义（对应 v2 框架的 Common.h）
//
// 把 lyzself 里散落在 internal/ 的六个文件收进这一个文件：
//   Config（编译期常量）、SizeClass（大小类映射）、FreeList（侵入式链表）、
//   SpinLock（自旋锁）、PStats（统计）latform（平台抽象）、
//
// 这里只有「框架」：常量、类型定义、方法签名。所有实现体待填充。
// ============================================================================
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#if defined(_WIN32)
#  include <malloc.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#endif

namespace mempool
{

// ---------------------------------------------------------------------------
// 编译期配置
// ---------------------------------------------------------------------------
struct Config
{
    // 基础对齐与边界
    // 最小对齐。16 字节保证任意标量类型（含 long double / SSE）都能安全存放，
    static constexpr size_t ALIGNMENT = 16;
    // 小对象上界。超过该值直接走 PageCache（大对象路径）。
    static constexpr size_t MAX_SMALL_SIZE = 256 * 1024; // 256KB
    // 系统页大小。与 x86-64 / ARM64 常见配置一致。
    static constexpr size_t PAGE_SIZE = 4096;
    // 缓存行大小。用于对齐桶结构，消除伪共享。使修改的变量不在同一个缓存行
    static constexpr size_t CACHE_LINE_SIZE = 64;

    // 大小类分组（步长为 2 的幂，size->index 只需移位）
    static constexpr size_t G0_MAX = 1024;
    static constexpr size_t G1_MAX = 8 * 1024;
    static constexpr size_t G2_MAX = 64 * 1024;
    static constexpr size_t G3_MAX = MAX_SMALL_SIZE;

    static constexpr size_t G0_STEP_SHIFT = 4;  // 16B
    static constexpr size_t G1_STEP_SHIFT = 7;  // 128B
    static constexpr size_t G2_STEP_SHIFT = 10; // 1KB
    static constexpr size_t G3_STEP_SHIFT = 13; // 8KB

    //统计每个类的数量
    static constexpr size_t G0_COUNT = G0_MAX >> G0_STEP_SHIFT;
    static constexpr size_t G1_COUNT = (G1_MAX - G0_MAX) >> G1_STEP_SHIFT;
    static constexpr size_t G2_COUNT = (G2_MAX - G1_MAX) >> G2_STEP_SHIFT;
    static constexpr size_t G3_COUNT = (G3_MAX - G2_MAX) >> G3_STEP_SHIFT;

    //各个类的起始索引
    static constexpr size_t G1_BASE = G0_COUNT;
    static constexpr size_t G2_BASE = G1_BASE + G1_COUNT;
    static constexpr size_t G3_BASE = G2_BASE + G2_COUNT;
    static constexpr size_t NUM_SIZE_CLASSES = G3_BASE + G3_COUNT; // 200

    // 批量策略（慢启动）
    // ThreadCache 单次向 CentralCache 取货的字节上限。
    // 8KB 是实测的甜点：小对象能拿到几百个，又不至于一次占用过多内存。
    static constexpr size_t MAX_BATCH_BYTES = 8 * 1024;
    // 慢启动的四档批量基数。冷启动少拿，热路径大批量。
    static constexpr size_t BATCH_COLD = 8;
    static constexpr size_t BATCH_WARM = 32;
    static constexpr size_t BATCH_HOT = 128;
    static constexpr size_t BATCH_BLAZING = 512;
    // 慢启动的档位切换阈值（按该大小类累计分配次数计）。
    static constexpr uint32_t SLOW_START_WARM = 100;
    static constexpr uint32_t SLOW_START_HOT = 1000;
    static constexpr uint32_t SLOW_START_BLAZING = 10000;

    // 归还策略
    // ThreadCache 每个桶的初始高水位线。超过则回吐给 CentralCache。
    static constexpr size_t INITIAL_HIGH_WATER = 64;
    // 高水位线的上限，字节数与块数双重约束。
    // 字节上限防止大对象桶吃掉过多线程本地内存；块数上限防止小对象桶无限膨胀。
    static constexpr size_t MAX_HIGH_WATER_BYTES = 256 * 1024;
    static constexpr size_t MAX_HIGH_WATER_BLOCKS = 2048;
    // 归还给 CentralCache 的比例。1/2 意味着超过高水位线的部分只回吐一半。
    static constexpr size_t RETURN_FRACTION = 2;

    // PageCache
    // CentralCache 切分小对象时申请的默认 span 页数。
    static constexpr size_t DEFAULT_SPAN_PAGES = 8;
    // 单次向系统申请的最小页数。减少 mmap 次数。
    static constexpr size_t MIN_SYSTEM_PAGES = 64;

    // 统计开关。关闭后不会收集任何统计数据，节省原子操作开销。
    static constexpr bool ENABLE_STATS = true;
};

// ---------------------------------------------------------------------------
// 大小类映射（原 SizeClass.h）
// ---------------------------------------------------------------------------
class SizeClass
{
public:
    // 向上对齐到所属大小类的块大小。
    static constexpr size_t roundUp(size_t bytes) noexcept{
        if (bytes == 0)
            return Config::ALIGNMENT;
        if (bytes <= Config::G0_MAX)
            return alignTo(bytes, Config::G0_STEP_SHIFT);
        if (bytes <= Config::G1_MAX)
            return alignTo(bytes, Config::G1_STEP_SHIFT);
        if (bytes <= Config::G2_MAX)
            return alignTo(bytes, Config::G2_STEP_SHIFT);
        if (bytes <= Config::G3_MAX)
            return alignTo(bytes, Config::G3_STEP_SHIFT);
        // 大对象按页对齐，由 PageCache 处理。
        return alignTo(bytes, pageShift());
    }

    // 取大小类下标。调用方需保证 bytes <= MAX_SMALL_SIZE。
    static constexpr size_t getIndex(size_t bytes) noexcept{
        if (bytes == 0)
            bytes = Config::ALIGNMENT;

        if (bytes <= Config::G0_MAX)
            return (bytes - 1) >> Config::G0_STEP_SHIFT;
        if (bytes <= Config::G1_MAX)
            return Config::G1_BASE + ((bytes - Config::G0_MAX - 1) >> Config::G1_STEP_SHIFT);
        if (bytes <= Config::G2_MAX)
            return Config::G2_BASE + ((bytes - Config::G1_MAX - 1) >> Config::G2_STEP_SHIFT);
        return Config::G3_BASE + ((bytes - Config::G2_MAX - 1) >> Config::G3_STEP_SHIFT);
    }

    // 下标 -> 该大小类的块大小。反函数，用于各层内部换算。
    static constexpr size_t sizeOfIndex(size_t index) noexcept{
        if (index < Config::G1_BASE)
            return (index + 1) << Config::G0_STEP_SHIFT;
        if (index < Config::G2_BASE)
            return Config::G0_MAX + ((index - Config::G1_BASE + 1) << Config::G1_STEP_SHIFT);
        if (index < Config::G3_BASE)
            return Config::G1_MAX + ((index - Config::G2_BASE + 1) << Config::G2_STEP_SHIFT);
        return Config::G2_MAX + ((index - Config::G3_BASE + 1) << Config::G3_STEP_SHIFT);
    }

    // 该大小类切分时向 PageCache 申请多少页，保证至少能装下 blocksWanted 个块。
    static constexpr size_t spanPagesFor(size_t blockSize, size_t blocksWanted) noexcept{
        size_t bytes = blockSize * blocksWanted;
        size_t pages = (bytes + Config::PAGE_SIZE - 1) / Config::PAGE_SIZE;
        if (pages < Config::DEFAULT_SPAN_PAGES)
            pages = Config::DEFAULT_SPAN_PAGES;
        return pages;
    }
    static constexpr bool isSmall(size_t bytes) noexcept{
        return bytes <= Config::MAX_SMALL_SIZE;
    }

private:
    static constexpr size_t alignTo(size_t bytes, size_t shift) noexcept{
        size_t step = size_t(1) << shift;
        return (bytes + step - 1) & ~(step - 1);
    }
    static constexpr size_t pageShift() noexcept{
        size_t shift = 0;
        size_t page = Config::PAGE_SIZE;
        while (page > 1)
        {
            page >>= 1;
            ++shift;
        }
        return shift;
    }
};

// ---------------------------------------------------------------------------
// 侵入式空闲链表（原 FreeList.h）
// ---------------------------------------------------------------------------
struct Batch
{
    void*  head = nullptr;
    void*  tail = nullptr;
    size_t count = 0;

    bool empty() const noexcept;
};

class FreeList
{
public:
    static void*& next(void* block) noexcept;
    static void push(void*& head, void* block) noexcept;
    static void* pop(void*& head) noexcept;
    static void pushRange(void*& head, const Batch& batch) noexcept;
    static Batch popRange(void*& head, size_t count) noexcept;
    static Batch build(void* memory, size_t totalBytes, size_t blockSize) noexcept;
    static Batch splitAfter(void* head, size_t keep, void** keptTail) noexcept;
    static size_t length(void* head) noexcept;
};

// ---------------------------------------------------------------------------
// 自旋锁（原 SpinLock.h）
// ---------------------------------------------------------------------------
class SpinLock
{
public:
    void lock() noexcept;
    void unlock() noexcept;
    bool tryLock() noexcept;

private:
    static constexpr int SPIN_LIMIT = 64;
    std::atomic<bool> flag_{false};
};

class SpinLockGuard
{
public:
    explicit SpinLockGuard(SpinLock& lock) noexcept;
    ~SpinLockGuard() noexcept;

    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    SpinLock& lock_;
};

// ---------------------------------------------------------------------------
// 平台抽象（原 Platform.h）
// ---------------------------------------------------------------------------
class Platform
{
public:
    static void* mapPages(size_t numPages) noexcept;
    static void unmapPages(void* ptr, size_t numPages) noexcept;
    static void cpuPause() noexcept;
};

// ---------------------------------------------------------------------------
// 统计（原 Stats.h）
// ---------------------------------------------------------------------------
struct Stats
{
    uint64_t allocCount = 0;
    uint64_t deallocCount = 0;
    uint64_t bytesAllocated = 0;
    uint64_t bytesFreed = 0;
    uint64_t threadCacheHits = 0;
    uint64_t centralCacheHits = 0;
    uint64_t pageCacheHits = 0;
    uint64_t largeAllocCount = 0;
    uint64_t systemBytes = 0;
    uint64_t spansInUse = 0;

    uint64_t liveBytes() const noexcept;
    double hitRate() const noexcept;
};

struct ThreadLocalStats
{
    uint64_t allocCount = 0;
    uint64_t deallocCount = 0;
    uint64_t bytesAllocated = 0;
    uint64_t bytesFreed = 0;
    uint64_t threadCacheHits = 0;
    uint64_t centralCacheHits = 0;
};

class GlobalStats
{
public:
    static GlobalStats& instance() noexcept;

    void merge(const ThreadLocalStats& local) noexcept;
    void addPageCacheHit() noexcept;
    void addLargeAlloc() noexcept;
    void addSystemBytes(int64_t delta) noexcept;
    void addSpans(int64_t delta) noexcept;

    Stats snapshot() const noexcept;
    void reset() noexcept;

private:
    GlobalStats() = default;

    std::atomic<uint64_t> allocCount_{0};
    std::atomic<uint64_t> deallocCount_{0};
    std::atomic<uint64_t> bytesAllocated_{0};
    std::atomic<uint64_t> bytesFreed_{0};
    std::atomic<uint64_t> threadCacheHits_{0};
    std::atomic<uint64_t> centralCacheHits_{0};
    std::atomic<uint64_t> pageCacheHits_{0};
    std::atomic<uint64_t> largeAllocCount_{0};
    std::atomic<int64_t> systemBytes_{0};
    std::atomic<int64_t> spansInUse_{0};
};

} // namespace mempool
