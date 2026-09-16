#pragma once
// ============================================================================
// Common.h —— 所有层共享的公共定义
//
//   Config（编译期常量）、SizeClass（大小类映射）、FreeList（侵入式链表）、
//   SpinLock（自旋锁）、PStats（统计）latform（平台抽象）、
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
// 大小类映射
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
// 侵入式空闲链表
// ---------------------------------------------------------------------------
struct Batch
{
    void*  head = nullptr;
    void*  tail = nullptr;
    size_t count = 0;

    bool empty() const noexcept{
        return head == nullptr;
    }
};

class FreeList
{
public:
    // 读/写块内的 next 指针。
    static void*& next(void* block) noexcept{
        return *reinterpret_cast<void**>(block);
    }

    // 头插。
    static void push(void*& head, void* block) noexcept{
        next(block) = head;
        head = block;
    }

    // 头删。空链表返回 nullptr。
    static void* pop(void*& head) noexcept{
        void* block = head;
        if(block != nullptr)
            head = next(block);
        return block;
    }

    // 把一段链表整体接到 head 前面。
    static void pushRange(void*& head, const Batch& batch) noexcept{
        next(batch.tail) = head;
        head = batch.head;
    }

    // 从 head 摘下最多 count 个块，head 更新为剩余部分。
    static Batch popRange(void*& head, size_t count) noexcept{
        Batch batch;
        if (head == nullptr || count == 0){
            return batch;
        }

        void* tail = head;
        size_t n = 1;
        while (n < count && next(tail) != nullptr){
            tail = next(tail);
            ++n;
        }

        batch.head = head;

        batch.tail = tail;
        batch.count = n;


        head = next(tail);
        next(tail) = nullptr;
        return batch;
        
    }

    //把一整块连续内存按 blockSize 切成链表。
    static Batch build(void* memory, size_t totalBytes, size_t blockSize) noexcept{

        Batch batch;
        size_t count = totalBytes / blockSize;
        if (count == 0)
            return batch;
        
        char* base = static_cast<char*>(memory);
        for(size_t i = 0; i + 1 < count; ++i){
            next(base + i * blockSize) = base + (i + 1) * blockSize;
        }
        next(base + (count - 1) * blockSize) = nullptr;
        char* tail = base + (count - 1) * blockSize;
        batch.head = memory;
        batch.tail = tail;
        batch.count = count;
        return batch;
    }

    //截断链表，留下前keep个节点，返回后半段，*keptTail为前半段的尾节点
    static Batch splitAfter(void* head, size_t keep, void** keptTail) noexcept{
        Batch rest;
        *keptTail = nullptr;
        if (head == nullptr || keep == 0)
            return rest;

        void* node = head;
        for (size_t i = 1; i < keep; ++i)
        {
            if (next(node) == nullptr)
            {
                *keptTail = node;
                return rest; // 长度不足，整条留下
            }
            node = next(node);
        }

        *keptTail = node;

        void* restHead = next(node);
        if (restHead == nullptr)
            return rest;

        next(node) = nullptr; // 断开

        // 走到后半段尾部，凑齐 Batch。
        void* tail = restHead;
        size_t n = 1;
        while (next(tail) != nullptr)
        {
            tail = next(tail);
            ++n;
        }

        rest.head = restHead;
        rest.tail = tail;
        rest.count = n;
        return rest;
    }

    // 计算链表长度
    static size_t length(void* head) noexcept{
        size_t n = 0;
        while (head != nullptr)
        {
            ++n;
            head = next(head);
        }
        return n;
    }
};

// ---------------------------------------------------------------------------
// 平台抽象   把"与操作系统打交道"的几件事封装成统一的、跨平台的接口，
//           供上层（主要是 PageCache）调用。
// ---------------------------------------------------------------------------
class Platform
{
public:

    //申请 numPages 页。失败返回 nullptr（不抛异常）。
    static void* mapPages(size_t numPages) noexcept{
        size_t bytes = numPages * Config::PAGE_SIZE;
#if defined(_WIN32)
        return _aligned_malloc(bytes, Config::PAGE_SIZE);
#else
        void* ptr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return (ptr == MAP_FAILED) ? nullptr : ptr;
#endif
    }

    //归还 mapPages 得到的内存。ptr 必须是原始映射起点，页数必须一致。
    static void unmapPages(void* ptr, size_t numPages) noexcept{
        if (ptr == nullptr)
            return;
#if defined(_WIN32)
        (void)numPages;
        _aligned_free(ptr);
#else
        ::munmap(ptr, numPages * Config::PAGE_SIZE);
#endif
    }

    // CPU 让步提示，用于自旋锁退避。
    static void cpuPause() noexcept{
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
#else
        // 其他架构上退化为空操作，由上层的 yield 兜底。
#endif
    }
};

// ---------------------------------------------------------------------------
// 自旋锁
// ---------------------------------------------------------------------------
class SpinLock
{
public:
    void lock() noexcept{
        //快路径:无竞争时一次 exchange 成交。
        if (!flag_.exchange(true, std::memory_order_acquire))
            return;

        //慢路径:读-自旋，避免反复写导致缓存行来回弹跳。
        int spins = 0;
        while (true){
            while (flag_.load(std::memory_order_relaxed)){
                if (++spins < SPIN_LIMIT)
                    Platform::cpuPause();
                else{
                    std::this_thread::yield();
                    spins = 0;
                }
            }
            if (!flag_.exchange(true, std::memory_order_acquire))
                return;
        }    
    }


    void unlock() noexcept{
        flag_.store(false, std::memory_order_release);
    }


    bool tryLock() noexcept{
        return !flag_.exchange(true, std::memory_order_acquire);
    }

private:
    static constexpr int SPIN_LIMIT = 64;
    std::atomic<bool> flag_{false};
};

class SpinLockGuard
{
public:
    explicit SpinLockGuard(SpinLock& lock) noexcept: lock_(lock) { lock_.lock(); }

    ~SpinLockGuard() noexcept { lock_.unlock(); }

    SpinLockGuard(const SpinLockGuard&) = delete;
    SpinLockGuard& operator=(const SpinLockGuard&) = delete;

private:
    SpinLock& lock_;
};

// ============================================================================
// 统计数据
//
// 分两层：
//   - ThreadLocalStats：线程本地，普通整数，无原子开销，热路径累加它。
//   - GlobalStats：进程级原子量，线程退出或调用 getStats() 时汇总。
//
// ENABLE_STATS 关闭时所有累加都是死代码，会被编译器完全消除。
// ============================================================================

// 对外暴露的统计快照。
struct Stats
{
    uint64_t allocCount = 0;        // 分配次数
    uint64_t deallocCount = 0;      // 释放次数
    uint64_t bytesAllocated = 0;    // 累计分配字节（对齐后）
    uint64_t bytesFreed = 0;        // 累计释放字节（对齐后）

    uint64_t threadCacheHits = 0;   // ThreadCache 命中次数
    uint64_t centralCacheHits = 0;  // 下沉到 CentralCache 的次数
    uint64_t pageCacheHits = 0;     // 下沉到 PageCache 的次数
    uint64_t largeAllocCount = 0;   // 走大对象路径的次数

    uint64_t systemBytes = 0;       // 当前向系统申请且未归还的字节
    uint64_t spansInUse = 0;        // 当前在用 span 数

    // 当前占用（近似）：已分配 - 已释放。
    uint64_t liveBytes() const noexcept{
        return bytesAllocated >= bytesFreed ? bytesAllocated - bytesFreed : 0;
    }

    double hitRate() const noexcept{
        return allocCount == 0 ? 0.0
                               : static_cast<double>(threadCacheHits) / static_cast<double>(allocCount);
    }
};

// 线程本地计数器：无锁、无原子，随便加。
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
    static GlobalStats& instance() noexcept{
        static GlobalStats s;
        return s;
    }

    // 汇总线程本地计数器到全局计数器。线程退出或外部查询时调用。
    void merge(const ThreadLocalStats& local) noexcept{
        if constexpr (!Config::ENABLE_STATS)
            return;
        allocCount_.fetch_add(local.allocCount, std::memory_order_relaxed);
        deallocCount_.fetch_add(local.deallocCount, std::memory_order_relaxed);
        bytesAllocated_.fetch_add(local.bytesAllocated, std::memory_order_relaxed);
        bytesFreed_.fetch_add(local.bytesFreed, std::memory_order_relaxed);
        threadCacheHits_.fetch_add(local.threadCacheHits, std::memory_order_relaxed);
        centralCacheHits_.fetch_add(local.centralCacheHits, std::memory_order_relaxed);
    }


    void addPageCacheHit() noexcept{
        if constexpr (Config::ENABLE_STATS)
            pageCacheHits_.fetch_add(1, std::memory_order_relaxed);
    }


    void addLargeAlloc() noexcept{
        if constexpr (Config::ENABLE_STATS)
            largeAllocCount_.fetch_add(1, std::memory_order_relaxed);
    }
    void addSystemBytes(int64_t delta) noexcept{
        if constexpr (Config::ENABLE_STATS)
            systemBytes_.fetch_add(delta, std::memory_order_relaxed);
    }
    void addSpans(int64_t delta) noexcept{
        if constexpr (Config::ENABLE_STATS)
            spansInUse_.fetch_add(delta, std::memory_order_relaxed);
    }

    Stats snapshot() const noexcept{
        Stats s;
        s.allocCount = allocCount_.load(std::memory_order_relaxed);
        s.deallocCount = deallocCount_.load(std::memory_order_relaxed);
        s.bytesAllocated = bytesAllocated_.load(std::memory_order_relaxed);
        s.bytesFreed = bytesFreed_.load(std::memory_order_relaxed);
        s.threadCacheHits = threadCacheHits_.load(std::memory_order_relaxed);
        s.centralCacheHits = centralCacheHits_.load(std::memory_order_relaxed);
        s.pageCacheHits = pageCacheHits_.load(std::memory_order_relaxed);
        s.largeAllocCount = largeAllocCount_.load(std::memory_order_relaxed);
        s.systemBytes = static_cast<uint64_t>(systemBytes_.load(std::memory_order_relaxed));
        s.spansInUse = static_cast<uint64_t>(spansInUse_.load(std::memory_order_relaxed));
        return s;
    }
    void reset() noexcept{
        allocCount_.store(0, std::memory_order_relaxed);
        deallocCount_.store(0, std::memory_order_relaxed);
        bytesAllocated_.store(0, std::memory_order_relaxed);
        bytesFreed_.store(0, std::memory_order_relaxed);
        threadCacheHits_.store(0, std::memory_order_relaxed);
        centralCacheHits_.store(0, std::memory_order_relaxed);
        pageCacheHits_.store(0, std::memory_order_relaxed);
        largeAllocCount_.store(0, std::memory_order_relaxed);
        // systemBytes_ / spansInUse_ 是当前状态量，不清零。
    }

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



// ---- 编译期自检：边界必须落在正确的大小类上 ----
static_assert(SizeClass::roundUp(1) == 16, "roundUp(1)");
static_assert(SizeClass::roundUp(16) == 16, "roundUp(16)");
static_assert(SizeClass::roundUp(17) == 32, "roundUp(17)");
static_assert(SizeClass::roundUp(1024) == 1024, "roundUp(1024)");
static_assert(SizeClass::roundUp(1025) == 1024 + 128, "roundUp(1025)");

static_assert(SizeClass::getIndex(1) == 0, "index(1)");
static_assert(SizeClass::getIndex(16) == 0, "index(16)");
static_assert(SizeClass::getIndex(17) == 1, "index(17)");
static_assert(SizeClass::getIndex(1024) == 63, "index(1024)");
static_assert(SizeClass::getIndex(1025) == 64, "index(1025)");
static_assert(SizeClass::getIndex(8 * 1024) == 119, "index(8K)");
static_assert(SizeClass::getIndex(8 * 1024 + 1) == 120, "index(8K+1)");
static_assert(SizeClass::getIndex(64 * 1024) == 175, "index(64K)");
static_assert(SizeClass::getIndex(64 * 1024 + 1) == 176, "index(64K+1)");
static_assert(SizeClass::getIndex(256 * 1024) == Config::NUM_SIZE_CLASSES - 1, "index(256K)");

static_assert(SizeClass::sizeOfIndex(0) == 16, "size(0)");
static_assert(SizeClass::sizeOfIndex(63) == 1024, "size(63)");
static_assert(SizeClass::sizeOfIndex(64) == 1024 + 128, "size(64)");
static_assert(SizeClass::sizeOfIndex(Config::NUM_SIZE_CLASSES - 1) == 256 * 1024, "size(last)");

// roundUp 与 index/sizeOfIndex 必须自洽。
static_assert(SizeClass::sizeOfIndex(SizeClass::getIndex(700)) == SizeClass::roundUp(700), "consistency");
static_assert(SizeClass::sizeOfIndex(SizeClass::getIndex(5000)) == SizeClass::roundUp(5000), "consistency");
static_assert(SizeClass::sizeOfIndex(SizeClass::getIndex(50000)) == SizeClass::roundUp(50000), "consistency");



} // namespace mempool
