#pragma once
//
// 基准测试小工具
//
// 只做三件事：计时、多线程跑同一份负载、把内存池和 new/delete 摆在一起对比。
//
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace bench
{

// 阻止编译器把被测代码优化掉。
template <typename T>
inline void keep(T&& value)
{
    asm volatile("" : : "r,m"(value) : "memory");
}

// 跑 iterations 次，返回总耗时（毫秒）。
template <typename Func>
double measure(Func&& func, size_t iterations)
{
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i)
        func();
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// 跑一次整体负载（负载自己内部循环），返回耗时（毫秒）。
template <typename Func>
double measureOnce(Func&& func)
{
    auto start = std::chrono::steady_clock::now();
    func();
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// threads 个线程各跑一遍 func(threadId)，返回墙钟耗时（毫秒）。
template <typename Func>
double measureThreads(size_t threads, Func&& func)
{
    std::vector<std::thread> workers;
    workers.reserve(threads);

    auto start = std::chrono::steady_clock::now();
    for (size_t t = 0; t < threads; ++t)
        workers.emplace_back([&func, t]() { func(t); });
    for (std::thread& w : workers)
        w.join();
    auto end = std::chrono::steady_clock::now();

    return std::chrono::duration<double, std::milli>(end - start).count();
}

// 一组对比结果的表格输出。
class Table
{
public:
    explicit Table(std::string title) : title_(std::move(title))
    {
        std::printf("\n=== %s ===\n", title_.c_str());
        std::printf("%-28s %12s %12s %10s\n", "场景", "内存池(ms)", "new(ms)", "加速比");
        std::printf("%s\n", std::string(66, '-').c_str());
    }

    void row(const std::string& name, double poolMs, double newMs)
    {
        double speedup = poolMs > 0.0 ? newMs / poolMs : 0.0;
        std::printf("%-28s %12.2f %12.2f %9.2fx\n", name.c_str(), poolMs, newMs, speedup);
        totalPool_ += poolMs;
        totalNew_ += newMs;
        ++rows_;
    }

    ~Table()
    {
        if (rows_ > 1)
        {
            std::printf("%s\n", std::string(66, '-').c_str());
            double speedup = totalPool_ > 0.0 ? totalNew_ / totalPool_ : 0.0;
            std::printf("%-28s %12.2f %12.2f %9.2fx\n", "合计", totalPool_, totalNew_, speedup);
        }
    }

private:
    std::string title_;
    double totalPool_ = 0.0;
    double totalNew_ = 0.0;
    size_t rows_ = 0;
};

// 单次操作耗时（纳秒）。
inline double nsPerOp(double ms, size_t ops)
{
    return ops == 0 ? 0.0 : ms * 1e6 / static_cast<double>(ops);
}

} // namespace bench
