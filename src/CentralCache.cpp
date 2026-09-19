// CentralCache.cpp —— 实现见 CentralCache.h
//   CentralCache() / fetchRange / returnRange / cachedCount / refill / ~CentralCache
//
#include "CentralCache.h"
#include "PageCache.h"

namespace mempool
{

CentralCache::CentralCache()
{
    // 先触发 PageCache 的构造，锁定析构顺序（见头文件说明）。
    (void)PageCache::getInstance();
}

//给ThreadCache供货，一批最多batchNum个块
Batch CentralCache::fetchRange(size_t index, size_t batchNum){
    if(index >= Config::NUM_SIZE_CLASSES || batchNum == 0 ){
        return Batch{};
    }

    Bucket& bucket = buckets_[index];

    {
        SpinLockGuard guard(bucket.lock);

        if(bucket.freeList != nullptr){
            Batch batch = FreeList::popRange(bucket.freeList, batchNum);
            bucket.count -= batch.count;
            return batch;
        }
    }


    // 桶空了。切分新 span 的过程要向 PageCache 申请内存，耗时较长，
    // 因此放在桶锁之外做，避免阻塞同尺寸的其他线程。
    return refill(index, batchNum);
}

//ThreadCache归还一批块
void CentralCache::returnRange(size_t index, const Batch& batch){
    if(index >= Config::NUM_SIZE_CLASSES || batch.empty()){
        return;
    }

    Bucket& bucket = buckets_[index];
    SpinLockGuard guard(bucket.lock);

    FreeList::pushRange(bucket.freeList, batch);
    bucket.count += batch.count;
}

//查询，返回某个大小桶里当前的缓存的块数
size_t CentralCache::cachedCount(size_t index) const noexcept{
    if (index >= Config::NUM_SIZE_CLASSES)
        return 0;

    // 读一个 size_t 不需要加锁的强一致性，但加锁最省心，这里不是热路径。
    Bucket& bucket = const_cast<Bucket&>(buckets_[index]);
    SpinLockGuard guard(bucket.lock);
    return bucket.count;
}

//核心函数：桶空了，向PageCache要新的span并且切块
Batch CentralCache::refill(size_t index, size_t batchNum){
    size_t blockSize = SizeClass::sizeOfIndex(index);
    
    //一次多切一些
    size_t spanPages = SizeClass::spanPagesFor(blockSize, batchNum * 2);
    
    void* span = PageCache::getInstance().allocateSpan(spanPages);
    if(span == nullptr){
        return Batch{};
    }

    {
        std::lock_guard<std::mutex> guard(spansMutex_);
        carvedSpans_.push_back(CarvedSpan{span, spanPages});//记录内容：某个span被切成了多少块
    }

    Batch all = FreeList::build(span, spanPages * Config::PAGE_SIZE, blockSize);
    if(all.empty()){
        return Batch{};
    }

    //取走调用方需要的部分，其余的留在桶里面
    void* head = all.head;
    Batch result = FreeList::popRange(head, batchNum);
    if(head != nullptr){
        Batch rest;
        rest.head = head;
        rest.tail = all.tail;
        rest.count = all.count - result.count;

        Bucket& bucket = buckets_[index];
        SpinLockGuard guard(bucket.lock);
        FreeList::pushRange(bucket.freeList, rest);
        bucket.count += rest.count;
    }

    return result;
}


CentralCache::~CentralCache()
{
    // 进程退出，把切分过的 span 全部还给 PageCache。
    // 此时各线程的 ThreadCache 已析构完毕（构造顺序保证了本对象后于它们销毁）。
    std::lock_guard<std::mutex> guard(spansMutex_);
    for (const CarvedSpan& span : carvedSpans_)
        PageCache::getInstance().deallocateSpan(span.start, span.numPages);
    carvedSpans_.clear();
}


} // namespace mempool
