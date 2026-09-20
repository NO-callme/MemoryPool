// ThreadCache.cpp —— 实现见 ThreadCache.h
//
// 内容待填充（与 lyzself/src/ThreadCache.cpp 一致）：
//   ThreadCache() / allocate / deallocate / releaseAll / ~ThreadCache
//   fetchFromCentralCache / returnToCentralCache / getBatchNum / growHighWater
//
#include "ThreadCache.h"
#include "CentralCache.h"
#include <algorithm>

namespace mempool
{

ThreadCache::ThreadCache()
{
    // 先触发 CentralCache 的构造。thread_local 对象在线程退出时析构，
    // 那时必须还能把内存还回去，所以 CentralCache 必须先于本对象存在。
    (void)CentralCache::getInstance();
}

ThreadCache::~ThreadCache()
{
    releaseAll();
    GlobalStats::instance().merge(stats_);
}

//快路径

//桶里面有空闲的块就直接取，没有问Central要
void* ThreadCache::allocate(size_t size){
    size_t index = SizeClass::getIndex(size);
    size_t blockSize = SizeClass::sizeOfIndex(index);

    Bucket& bucket = buckets_[index];
    ++bucket.allocCount;

    if constexpr (Config::ENABLE_STATS)
    {
        ++stats_.allocCount;
        stats_.bytesAllocated += blockSize;
    }

    if(bucket.freeList != nullptr){
        void* ptr = FreeList::pop(bucket.freeList);
        bucket.count--;
        if constexpr(Config::ENABLE_STATS){
            ++stats_.threadCacheHits;
        }
        return ptr;
    }

    return fetchFromCentralCache(index, blockSize);
}

//把ptr放回桶里面，如果超过高水位就回吐一部分给Central
void ThreadCache::deallocate(void* ptr, size_t size){
    size_t index = SizeClass::getIndex(size);

    Bucket& bucket = buckets_[index];
    FreeList::push(bucket.freeList, ptr);
    ++bucket.count;

    if constexpr(Config::ENABLE_STATS){
        ++stats_.deallocCount;
        stats_.bytesFreed += SizeClass::sizeOfIndex(index);
    }

    //回吐
    if(bucket.highWater != 0 && bucket.count > bucket.highWater){
        returnToCentralCache(index);
    }
}


//满路经

void* ThreadCache::fetchFromCentralCache(size_t index, size_t blockSize){
    size_t batchNum = getBatchNum(index, blockSize);

    Batch batch = CentralCache::getInstance().fetchRange(index, batchNum);
    if(batch.empty()){
        return nullptr;
    }

    if constexpr(Config::ENABLE_STATS){
        stats_.centralCacheHits++;
    }

    //拿一个给调用方，其余的放入桶内
    void* result = batch.head;
    void* rest = FreeList::next(result);

    if(rest != nullptr){
        Batch remaining;
        remaining.head = rest;
        remaining.tail = batch.tail;
        remaining.count = batch.count - 1;

        FreeList::pushRange(buckets_[index].freeList, remaining);
        buckets_[index].count += remaining.count;
    }

    growHighWater(index, blockSize);
    return result;
}

//回吐逻辑
void ThreadCache::returnToCentralCache(size_t index){
    Bucket& bucket = buckets_[index];
    if (bucket.count <= 1)
        return;

    // 留一部分在本地，免得马上又要回中心层取货。
    size_t keep = std::max<size_t>(bucket.count / Config::RETURN_FRACTION, 1);

    void* keptTail = nullptr;
    Batch rest = FreeList::splitAfter(bucket.freeList, keep, &keptTail);
    if (rest.empty())
        return; // 链表比计数短，说明状态已不一致，保守起来什么都不做

    bucket.count = static_cast<uint32_t>(keep);
    CentralCache::getInstance().returnRange(index, rest);
}

//一次性把本地内存清空
void ThreadCache::releaseAll(){
    for(size_t index = 0; index < Config::NUM_SIZE_CLASSES; index++){
        Bucket& bucket = buckets_[index];
        if(bucket.freeList == nullptr){
            continue;
        }

        Batch batch;
        batch.head = bucket.freeList;
        batch.count = bucket.count;

        //走到尾节点，线程退出只发生一次
        void* tail = batch.head;
        size_t n = 1;
        while(FreeList::next(tail) != nullptr){
            tail = FreeList::next(tail);
            n++;
        }
        batch.tail = tail;
        batch.count = n;

        bucket.freeList = nullptr;
        bucket.count = 0;

        CentralCache::getInstance().returnRange(index, batch);
    }
}


//策略

size_t ThreadCache::getBatchNum(size_t index, size_t blockSize) const noexcept
{
    // 慢启动：冷的大小类少拿，免得为一次性分配预热一大批；
    // 热的大小类大批量拿，把跨层调用次数压到最低。
    uint32_t allocs = buckets_[index].allocCount;

    size_t baseNum;
    if (allocs < Config::SLOW_START_WARM)
        baseNum = Config::BATCH_COLD;
    else if (allocs < Config::SLOW_START_HOT)
        baseNum = Config::BATCH_WARM;
    else if (allocs < Config::SLOW_START_BLAZING)
        baseNum = Config::BATCH_HOT;
    else
        baseNum = Config::BATCH_BLAZING;

    // 单次取货的字节上限，防止大对象一次搬走过多内存。
    size_t maxNum = std::max<size_t>(Config::MAX_BATCH_BYTES / blockSize, 1);

    return std::min(baseNum, maxNum);
}

void ThreadCache::growHighWater(size_t index, size_t blockSize) noexcept
{
    Bucket& bucket = buckets_[index];

    // 上限：字节数与块数双重约束。
    size_t byteCap = std::max<size_t>(Config::MAX_HIGH_WATER_BYTES / blockSize, 1);
    size_t cap = std::min(byteCap, Config::MAX_HIGH_WATER_BLOCKS);

    if (bucket.highWater == 0)
    {
        bucket.highWater = static_cast<uint32_t>(std::min(Config::INITIAL_HIGH_WATER, cap));
        return;
    }

    // 每次回中心层取货都说明本地存量不够用，把阈值翻倍。
    // 反过来，长期不取货的大小类阈值就停在低位，内存不会被白占着。
    size_t next = std::min<size_t>(static_cast<size_t>(bucket.highWater) * 2, cap);
    bucket.highWater = static_cast<uint32_t>(next);
}

} // namespace mempool
