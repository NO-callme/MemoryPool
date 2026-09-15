# MemoryPool-self

lyzself 的内容 + v2 的框架。目标：把 lyzself 里散落在 `include/internal/` 的 8 个头文件，
收敛成 v2 那种「一个 Common.h + 四个分层头文件」的扁平结构，功能不变。

## 目录结构

```
MemoryPool-self/
├── CMakeLists.txt
├── include/
│   ├── Common.h        # 常量(Config) + SizeClass + FreeList + SpinLock + Platform + Stats
│   ├── MemoryPool.h    # 门面 + STL Allocator
│   ├── PageCache.h
│   ├── CentralCache.h
│   └── ThreadCache.h
├── src/
│   ├── PageCache.cpp
│   ├── CentralCache.cpp
│   ├── ThreadCache.cpp
│   └── MemoryPool.cpp
└── tests/
    ├── UnitTest.cpp
    └── PerformanceTest.cpp
```

## lyzself → 本目录 的对应关系

| lyzself（原） | 本目录（新） |
|---|---|
| include/Config.h | include/Common.h（Config 段） |
| include/internal/SizeClass.h | include/Common.h（SizeClass 段） |
| include/internal/FreeList.h | include/Common.h（Batch + FreeList 段） |
| include/internal/SpinLock.h | include/Common.h（SpinLock 段） |
| include/internal/Platform.h | include/Common.h（Platform 段） |
| include/internal/Stats.h | include/Common.h（Stats 段） |
| include/MemoryPool.h | include/MemoryPool.h |
| include/internal/PageCache.h | include/PageCache.h |
| include/internal/CentralCache.h | include/CentralCache.h |
| include/internal/ThreadCache.h | include/ThreadCache.h |
| src/*.cpp（4 个） | src/*.cpp（结构一致） |
| tests/unit/*.cpp（6 个） | tests/UnitTest.cpp（合并） |
| tests/benchmark/*.cpp + tests/stress/*.cpp | tests/PerformanceTest.cpp（合并） |

## 当前状态

- [x] 目录结构 + CMakeLists
- [x] 头文件的常量 / 类型 / 接口声明（无实现体）
- [ ] 各层实现体（待一起填充）
- [ ] 测试用例（待一起填充）

## 说明

头文件里只放了「框架」（常量、类型定义、方法签名），没有任何函数实现体，
所以现在还不能编译链接——这是预期状态，等框架确认后再一起填内容。
