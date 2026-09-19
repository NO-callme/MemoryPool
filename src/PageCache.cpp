
#include "PageCache.h"
#include <cassert>
#include <algorithm>

namespace mempool
{

//------对外接口

//返回分配到的span的起始地址，若分配失败返回nullptr
void* PageCache::allocateSpan(size_t numPages){
    if(numPages == 0){
        numPages = 1;
    }

    std::lock_guard<std::mutex> guard(mutex_);

    Span* span = takeFree(numPages);
    if(span == nullptr){
        //空闲池里没有够大的，向系统要一块新的再试一次
        if(!systemAlloc(numPages)){
            return nullptr;
        }
        span = takeFree(numPages);
        if(span == nullptr){
            return nullptr; //理论上不会走到这里
        }
    }

    GlobalStats::instance().addPageCacheHit();//下沉到PageCache的次数 + 1
    GlobalStats::instance().addSpans(1);//当前在用的span + 1
    return span->start;
}

//释放span(挂回空闲链表，还没有归还给操作系统)，若ptr为nullptr或不是本层分配的地址，则忽略
void PageCache::deallocateSpan(void* ptr, size_t numPages){
    if(ptr == nullptr){
        return;
    }

    std::lock_guard<std::mutex> guard(mutex_);

    auto it = spanMap_.find(ptr);
    if(it == spanMap_.end()){
        return; //不是本层发出的地址，忽略
    }

    Span* span = it->second;
    if(span->isFree){
        return; //重复归还，忽略
    }

    //断言检查分配和释放的页数是否一致
    assert(span->numPages == numPages && "deallocateSpan 页数与分配时不一致");
    (void)numPages;//避免编译器警告

    freePages_ += span->numPages;
    coalesce(span); //内部负责把最终的span挂回空闲链表

    GlobalStats::instance().addSpans(-1);
}

//归还空闲span给操作系统，返回释放的页数
size_t PageCache::releaseFreeSpans(){
    std::lock_guard<std::mutex> guard(mutex_);

    size_t released = 0;
    for(auto chunkIt = chunks_.begin(); chunkIt != chunks_.end();){

        void* chunkStart = chunkIt->first;
        size_t chunkPages = chunkIt->second;

        auto spanIt = spanMap_.find(chunkStart);
        //判断该chunk是否整块空闲
        bool wholeChunkFree = spanIt != spanMap_.end() &&
                                        spanIt->second->isFree &&
                                        spanIt->second->numPages == chunkPages;
        
        if(!wholeChunkFree){
            ++chunkIt;
            continue;
        }

        Span* span = spanIt->second;
        unlinkFree(span);
        spanMap_.erase(spanIt);
        delete span;

        Platform::unmapPages(chunkStart, chunkPages);
        GlobalStats::instance().addSystemBytes(-static_cast<int64_t>(chunkPages * Config::PAGE_SIZE));

        totalPages_ -= chunkPages;
        freePages_ -= chunkPages;
        released += chunkPages;

        chunkIt = chunks_.erase(chunkIt); //删除当前chunk后，迭代器指向下一个chunk
    }
    return released;
}

//返回总页数
size_t PageCache::totalPages() const noexcept{
    std::lock_guard<std::mutex> guard(mutex_);
    return totalPages_;
}

//返回空闲页数
size_t PageCache::freePages() const noexcept{
    std::lock_guard<std::mutex> guard(mutex_);
    return freePages_;
}

//析构函数，释放所有系统内存
PageCache::~PageCache(){
    //系统退出，把所有东西都还给操作系统
    for(auto& entry : spanMap_){
        delete entry.second;
    }
    spanMap_.clear();
    freeSpans_.clear();

    for(auto& chunk : chunks_){
        Platform::unmapPages(chunk.first, chunk.second);
    }
    chunks_.clear();

}   

//------对内实现


//系统分配内存，返回分配到的span的起始地址，若分配失败返回false
bool PageCache::systemAlloc(size_t numPages){
    // 一次多要一些，摊薄 mmap 的系统调用开销。
    size_t pages = std::max(numPages, Config::MIN_SYSTEM_PAGES);

    void* memory = Platform::mapPages(pages);
    if(memory == nullptr){
        return false;
    }

    Span* span = new(std::nothrow) Span();
    if(span == nullptr){
        Platform::unmapPages(memory, pages);
        return false;
    }

    span->start = memory;
    span->numPages = pages;
    span->chunk = memory;

    chunks_[memory] = pages;
    spanMap_[memory] = span;
    linkFree(span);

    totalPages_ += pages;
    freePages_ += pages;

    GlobalStats::instance().addSystemBytes(static_cast<int64_t>(pages * Config::PAGE_SIZE));
    return true;

}

//从空闲span中取出一个满足要求的span，若没有则返回nullptr
PageCache::Span* PageCache::takeFree(size_t numPages){
    auto it = freeSpans_.lower_bound(numPages);
    while(it != freeSpans_.end() && it->second == nullptr){
        //清理空桶
        it = freeSpans_.erase(it);
    }

    if(it == freeSpans_.end()){
        return nullptr;
    }

    Span* span = it->second;
    unlinkFree(span);
    freePages_ -= span->numPages;

    if(span->numPages > numPages){
        splitSpan(span, numPages);
    }

    return span;

}

//连接空闲span到空闲链表中
void PageCache::linkFree(Span* span){
    assert(!span->isFree);
    span->isFree = true;

    Span*& head = freeSpans_[span->numPages];
    span->next = head;
    span->prev = nullptr;
    if(head != nullptr){
        head->prev = span;
    }
    head = span;
}


//从空闲链表中断开span
void PageCache::unlinkFree(Span* span){
    assert(span->isFree);
    span->isFree = false;

    if(span->prev != nullptr){
        span->prev->next = span->next;
    }else{
        auto it = freeSpans_.find(span->numPages);
        it->second = span->next;
    }

    if(span->next != nullptr){
        span->next->prev = span->prev;
    }

    span->next = nullptr;
    span->prev = nullptr;

}

//分割span，将其分为两部分，前半部分满足要求，后半部分仍然是空闲span
void PageCache::splitSpan(Span* span, size_t numPages){
    assert(span->numPages > numPages);
    assert(!span->isFree);

    size_t restPages = span->numPages - numPages;
    void* restStart = static_cast<char*>(span->start) + numPages * Config::PAGE_SIZE;

    Span* rest = new(std::nothrow) Span();
    if(rest == nullptr){
        return;
    }

    rest->start = restStart;
    rest->numPages = restPages;

    //虽然分割了，但是他们总是从系统的同一块内存切出来的，
    //无论两个部分分别用途是什么，他们对应的系统信息要一致
    rest->chunk = span->chunk;
    span->numPages = numPages;

    spanMap_[restStart] = rest;
    linkFree(rest);
    freePages_ += rest->numPages; 
}


//合并span，将相邻的空闲span合并为一个大的span
void PageCache::coalesce(Span* span){
    assert(!span->isFree);
    
    //向后合并，紧邻的部分若空闲并且两者同属于一块chunk就吞并
    while(true){
        void* nextStart = static_cast<char*>(span->start) + span->numPages * Config::PAGE_SIZE;
        auto it = spanMap_.find(nextStart);
        if( it == spanMap_.end() || !it->second->isFree || it->second->chunk != span->chunk){
            break;
        }

        Span* nextSpan = it->second;
        unlinkFree(nextSpan);
        span->numPages += nextSpan->numPages;
        spanMap_.erase(it);
        delete nextSpan;
    }

    //向前合并，找到地址小于当前span的最后一个span，判断是否首尾相接
    while(true){
        //会找到当前span
        auto it = spanMap_.lower_bound(span->start);
        if(it == spanMap_.begin()){
            break;
        }
        --it;//指向前一个

        Span* prevSpan = it->second;
        if(!prevSpan->isFree || prevSpan->chunk != span->chunk){
            break;
        }

        void* endPosition = static_cast<char*>(prevSpan->start) + prevSpan->numPages * Config::PAGE_SIZE;
        if(endPosition != span->start){
            break;
        }

        // 把本段并入前一段，前一段成为新的当前段。
        unlinkFree(prevSpan);
        prevSpan->numPages += span->numPages;
        spanMap_.erase(span->start);   // 删掉 span 这条
        delete span;
        span = prevSpan;               // 存活者换成 prevSpan，key 不变

    }

    linkFree(span);
}


} // namespace mempool
