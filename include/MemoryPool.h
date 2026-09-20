#pragma once
#include "Common.h"
#include "ThreadCache.h"

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace mempool
{

// ============================================================================
// MemoryPool：对外的唯一入口（外观模式
//
// allocate / deallocate 是全部功能的入口。小对象走 ThreadCache 快路径，
// 大对象（> 256KB）直接落到 PageCache。
// ============================================================================
class MemoryPool
{
public:
    // 分配 size 字节。size 为 0 时按最小对齐分配，保证返回值可比较、可释放。
    // 内存不足返回 nullptr（不抛异常）。
    static void* allocate(size_t size){
        if(size == 0){
            size = Config::ALIGNMENT;
        }

        if(__builtin_expect(size > Config::MAX_SMALL_SIZE, 0) ){
            return allocateLarge(size);
        }

        return ThreadCache::getInstance().allocate(size);
    }

    // 释放。ptr 可以为 nullptr；size 必须与分配时一致。
    static void deallocate(void* ptr, size_t size){
        if (ptr == nullptr)
            return;

        if (size == 0)
            size = Config::ALIGNMENT;

        if (__builtin_expect(size > Config::MAX_SMALL_SIZE, 0))
        {
            deallocateLarge(ptr, size);
            return;
        }

        ThreadCache::getInstance().deallocate(ptr, size);
    }


    // ---- 对象级封装 ----

    // 构造一个 T。构造函数抛异常时内存会被正确回收。
    template <typename T, typename... Args>
    static T* newElement(Args&&... args){
        void* memory = allocate(sizeof(T));
        if (memory == nullptr)
            return nullptr;

        try
        {
            return new (memory) T(std::forward<Args>(args)...);
        }
        catch (...)
        {
            deallocate(memory, sizeof(T));
            throw;
        }
    }


    // 析构并归还。ptr 可以为 nullptr。
    template <typename T>
    static void deleteElement(T* ptr){
        if (ptr == nullptr)
            return;
        ptr->~T();
        deallocate(ptr, sizeof(T));
    }


    // ---- 统计与维护 ----

    // 统计快照。会先把当前线程的本地计数并入全局，
    // 其他线程尚未汇总的计数不包含在内，因此是近似值。
    static Stats getStats();
    static void resetStats();

    // 把当前线程缓存的内存全部交还中心层。线程长期空闲前调用可以降低驻留内存。
    static void releaseThreadCache(){
        ThreadCache::getInstance().releaseAll();
    }

    // 把 PageCache 中完整空闲的内存块还给操作系统。返回归还的字节数。
    static size_t releaseToSystem();

    MemoryPool() = delete;

private:
    static void* allocateLarge(size_t size);
    static void deallocateLarge(void* ptr, size_t size);
};

// ============================================================================
// STL 适配器
//
//   std::vector<int, mempool::Allocator<int>> v;
// ============================================================================
template <typename T>
class Allocator
{
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;

    Allocator() noexcept = default;

    template <typename U>
    Allocator(const Allocator<U>&) noexcept {}

    template <typename U>
    struct rebind
    {
        using other = Allocator<U>;
    };

    T* allocate(size_t n){
        if (n > static_cast<size_t>(-1) / sizeof(T))
            throw std::bad_alloc();

        void* ptr = MemoryPool::allocate(n * sizeof(T));
        if (ptr == nullptr)
            throw std::bad_alloc();
        return static_cast<T*>(ptr);
    }
    void deallocate(T* ptr, size_t n) noexcept{
        MemoryPool::deallocate(ptr, n * sizeof(T));
    }

    template <typename U>
    bool operator==(const Allocator<U>&) const noexcept{
        return true;
    }
    template <typename U>
    bool operator!=(const Allocator<U>&) const noexcept{
        return false;
    }
};

} // namespace mempool
