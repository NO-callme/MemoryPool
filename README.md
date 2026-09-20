# MemoryPool-self

一个 C++17 的高性能线程内存池。采用 tcmalloc 式的**四层架构**，`thread_local` 无锁快路径，
多线程场景下稳定快于 `new/delete`。

框架沿用 v2 的扁平风格：所有共享定义收进一个 `Common.h`，四个分层头文件直接平铺在 `include/` 下。

## 架构

```
用户接口（MemoryPool API）
        │  allocate(size) / deallocate(ptr, size)
        ▼
ThreadCache（thread_local，无锁）        ← 小对象快路径（命中 = 一次链表头删）
        │ miss
        ▼
CentralCache（每大小类一把自旋锁）        ← 批量供货/收货，切分 span 成定长块
        │ miss
        ▼
PageCache（粗粒度互斥锁）                ← 页级管理，span 切分与合并，mmap
```

- **大小类**：200 个，步长为 2 的幂（16B/128B/1KB/8KB），`size→index` 纯移位无除法。
- **无头部开销**：`deallocate` 传相同 size，块的前 8 字节复用为链表 `next` 指针。
- **慢启动批量**：按分配频率分四档（8/32/128/512），跨层调用被摊到最稀。
- **自适应高水位**：桶超阈值回吐 1/2，取货时阈值翻倍，平衡内存占用与性能。

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
    ├── framework/
    │   ├── TestFramework.h   # 轻量单元测试框架（不依赖 gtest）
    │   └── Benchmark.h       # 基准测试工具
    ├── UnitTest.cpp          # 单元测试（59 用例）
    ├── benchmark/            # 基准：内存池 vs new/delete
    │   ├── bench_single_thread.cpp
    │   ├── bench_multi_thread.cpp
    │   └── bench_realistic.cpp
    └── stress/
        └── stress_test.cpp   # 随机负载压力测试
```

## 构建与运行

```bash
mkdir -p build && cd build
cmake .. && make -j

./unit_test               # 单元测试
./stress_test             # 压力测试
./bench_single_thread     # 单线程基准
./bench_multi_thread      # 多线程基准
./bench_realistic         # 真实场景基准
```

也可用便捷目标：`make run_test` / `make run_stress` / `make run_bench`。

## 测试结果

- **单元测试**：59/59 通过（SizeClass 9、FreeList 10、PageCache 9、CentralCache 7、ThreadCache 9、MemoryPool 15）
- **压力测试**：80 万次随机操作 + 线程反复创建销毁，0 内存破坏、0 分配失败

## 性能（本机实测，2 核）

| 场景 | 内存池 | new/delete | 加速比 |
|---|---|---|---|
| 单线程 · 分配立即释放（32B） | 6.7ns/次 | 15.5ns/次 | **2.5x** |
| 单线程 · 各尺寸综合 | — | — | **2.38x** |
| 多线程 · 独立分配释放（32B，8 线程） | — | — | **2.24x** |
| 多线程 · 批量持有 1000 对象（64B） | — | — | **最高 4.41x** |
| 真实场景 · 游戏引擎 | — | — | 1.32x |
| 真实场景 · 网络服务器（4 线程） | — | — | 1.59x |
| 真实场景 · 生产者消费者（跨线程释放） | — | — | ~1x（打平，见下方边界说明） |

- ThreadCache 命中率：**99.7%+**（慢启动 + 高水位策略生效）
- 单次分配延迟约 **6.5ns**，远优于 20ns 的设计目标

> 注：以上为 2 核虚拟机上的测量结果，**benchmark 噪声较大**（同配置多次运行可波动 50%+），
> 数值反映趋势，不宜当作精确指标。

## 已知边界与优化方向

**跨线程释放**是当前设计的弱项（`bench_realistic` 的"生产者消费者"场景）：

- 一个线程分配、另一个线程释放时，块进入**释放线程**的本地桶，分配线程拿不到，只能频繁下沉到
  CentralCache 取货；释放线程又要反复抢 CentralCache 锁回吐。
- 实测该场景内存池与 new/delete 基本打平（多次测量 0.8x~1.4x 波动）。

未来可选优化（按性价比）：

| 方向 | 说明 |
|---|---|
| CentralCache 无锁化 | lock-free 栈 + tagged pointer 替换自旋锁，减少锁竞争与 CPU 空转 |
| 远程释放队列 | 跨线程释放的块直接回到分配线程，绕开 CentralCache（需 span 归属追踪） |
| Span 级回收 | span 内块全部空闲时归还 PageCache，降低长期内存驻留 |
| 预热机制 | 启动时预分配常用大小，降低冷启动延迟 |

## 使用示例

```cpp
#include "MemoryPool.h"

// 原始接口：deallocate 必须传与 allocate 相同的 size
void* p = mempool::MemoryPool::allocate(64);
mempool::MemoryPool::deallocate(p, 64);

// 对象封装：构造抛异常时内存自动回收
auto* obj = mempool::MemoryPool::newElement<MyClass>(arg1, arg2);
mempool::MemoryPool::deleteElement(obj);

// STL 适配器
std::vector<int, mempool::Allocator<int>> v;
```

## 与 lyzself 的对应关系

本目录是 [lyzself](../lyzself) 的框架简化版，功能一致：

| lyzself | 本目录 |
|---|---|
| `include/internal/` 下 8 个头文件 | 收进 `include/Common.h` + 4 个分层头文件 |
| `tests/unit/*.cpp`（6 个） | 合并进 `tests/UnitTest.cpp` |
| `tests/benchmark/*` + `tests/stress/*` | 原样保留目录结构 |
