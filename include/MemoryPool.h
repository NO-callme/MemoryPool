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
// MemoryPool：对外的唯一入口（外观模式，对应 v2 的 MemoryPool.h）
//
// allocate / deallocate 是全部功能的入口。小对象走 ThreadCache 快路径，
// 大对象（> 256KB）直接落到 PageCache。
// ============================================================================
class MemoryPool
{
public:
    static void* allocate(size_t size);
    static void deallocate(void* ptr, size_t size);

    template <typename T, typename... Args>
    static T* newElement(Args&&... args);

    template <typename T>
    static void deleteElement(T* ptr);

    static Stats getStats();
    static void resetStats();
    static void releaseThreadCache();
    static size_t releaseToSystem();

    MemoryPool() = delete;

private:
    static void* allocateLarge(size_t size);
    static void deallocateLarge(void* ptr, size_t size);
};

// ============================================================================
// STL 适配器：std::vector<int, mempool::Allocator<int>> v;
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

    T* allocate(size_t n);
    void deallocate(T* ptr, size_t n) noexcept;

    template <typename U>
    bool operator==(const Allocator<U>&) const noexcept;
    template <typename U>
    bool operator!=(const Allocator<U>&) const noexcept;
};

} // namespace mempool
