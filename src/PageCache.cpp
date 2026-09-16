
#include "PageCache.h"
#include <cassert>
#include <algorithm>

namespace mempool
{

//返回分配到的span的起始地址，若分配失败返回nullptr
void* PageCache::allocateSpan(size_t numPages){

}

//释放span，若ptr为nullptr或不是本层分配的地址，则忽略
void PageCache::deallocateSpan(void* ptr, size_t numPages){

}

//归还空闲span，返回释放的页数
size_t PageCache::releaseFreeSpans(){

}

//返回总页数
size_t PageCache::totalPages() const noexcept{

}

//返回空闲页数
size_t PageCache::freePages() const noexcept{

}

//析构函数，释放所有系统内存
PageCache::~PageCache(){

}

//系统分配内存，返回分配到的span的起始地址，若分配失败返回false
bool PageCache::systemAlloc(size_t numPages){

}

//从空闲span中取出一个满足要求的span，若没有则返回nullptr
PageCache::Span* PageCache::takeFree(size_t numPages){

}

//连接空闲span到空闲链表中
void PageCache::linkFree(Span* span){

}


//从空闲链表中断开span
void PageCache::unlinkFree(Span* span){

}

//分割span，将其分为两部分，前半部分满足要求，后半部分仍然是空闲span
void PageCache::splitSpan(Span* span, size_t numPages){

}


//合并span，将相邻的空闲span合并为一个大的span
void PageCache::coalesce(Span* span){

    
}


} // namespace mempool
