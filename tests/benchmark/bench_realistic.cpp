//
// 真实场景模拟
//
// 微基准容易把结论带偏（比如立即分配立即释放，new 的缓存也是热的）。
// 这里模拟三种实际负载：游戏引擎、网络服务器、生产者消费者。
//
#include "MemoryPool.h"
#include "framework/Benchmark.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace mempool;

namespace
{
// ---- 游戏引擎：大量小组件，每帧增删一部分 ----
struct Transform
{
    float position[3];
    float rotation[4];
    float scale[3];
};

struct Entity
{
    uint64_t id;
    Transform* transform;
    void* components[4];
    char name[32];
};

void benchGameEngine()
{
    bench::Table table("游戏引擎 · 每帧增删 10% 实体（1 万实体 × 600 帧）");

    constexpr size_t ENTITIES = 10000;
    constexpr size_t FRAMES = 600;
    constexpr size_t CHURN = ENTITIES / 10;

    double poolMs = bench::measureOnce([]() {
        std::vector<Entity*> entities(ENTITIES, nullptr);
        for (size_t i = 0; i < ENTITIES; ++i)
        {
            entities[i] = MemoryPool::newElement<Entity>();
            entities[i]->transform = MemoryPool::newElement<Transform>();
        }
        for (size_t f = 0; f < FRAMES; ++f)
        {
            size_t base = (f * CHURN) % ENTITIES;
            for (size_t i = 0; i < CHURN; ++i)
            {
                size_t slot = (base + i) % ENTITIES;
                MemoryPool::deleteElement(entities[slot]->transform);
                MemoryPool::deleteElement(entities[slot]);
                entities[slot] = MemoryPool::newElement<Entity>();
                entities[slot]->transform = MemoryPool::newElement<Transform>();
            }
        }
        for (Entity* e : entities)
        {
            MemoryPool::deleteElement(e->transform);
            MemoryPool::deleteElement(e);
        }
    });

    double newMs = bench::measureOnce([]() {
        std::vector<Entity*> entities(ENTITIES, nullptr);
        for (size_t i = 0; i < ENTITIES; ++i)
        {
            entities[i] = new Entity();
            entities[i]->transform = new Transform();
        }
        for (size_t f = 0; f < FRAMES; ++f)
        {
            size_t base = (f * CHURN) % ENTITIES;
            for (size_t i = 0; i < CHURN; ++i)
            {
                size_t slot = (base + i) % ENTITIES;
                delete entities[slot]->transform;
                delete entities[slot];
                entities[slot] = new Entity();
                entities[slot]->transform = new Transform();
            }
        }
        for (Entity* e : entities)
        {
            delete e->transform;
            delete e;
        }
    });

    table.row("单线程", poolMs, newMs);
}

// ---- 网络服务器：连接对象长生命周期，消息对象短生命周期 ----
struct Connection
{
    int fd;
    char buffer[512];
    void* userData;
};

void benchNetworkServer()
{
    bench::Table table("网络服务器 · 4 线程各处理 5 万条消息");

    constexpr size_t CONNECTIONS = 256;
    constexpr size_t MESSAGES = 50000;
    constexpr size_t THREADS = 4;

    double poolMs = bench::measureThreads(THREADS, [](size_t t) {
        std::vector<Connection*> conns(CONNECTIONS);
        for (size_t i = 0; i < CONNECTIONS; ++i)
            conns[i] = MemoryPool::newElement<Connection>();

        size_t seed = t * 104729 + 1;
        for (size_t m = 0; m < MESSAGES; ++m)
        {
            seed = seed * 1103515245 + 12345;
            size_t msgSize = 64 + (seed >> 16) % 1984; // 64B ~ 2KB
            void* msg = MemoryPool::allocate(msgSize);
            std::memset(msg, 1, 64); // 模拟写包头
            bench::keep(msg);
            MemoryPool::deallocate(msg, msgSize);
        }

        for (Connection* c : conns)
            MemoryPool::deleteElement(c);
    });

    double newMs = bench::measureThreads(THREADS, [](size_t t) {
        std::vector<Connection*> conns(CONNECTIONS);
        for (size_t i = 0; i < CONNECTIONS; ++i)
            conns[i] = new Connection();

        size_t seed = t * 104729 + 1;
        for (size_t m = 0; m < MESSAGES; ++m)
        {
            seed = seed * 1103515245 + 12345;
            size_t msgSize = 64 + (seed >> 16) % 1984;
            char* msg = new char[msgSize];
            std::memset(msg, 1, 64);
            bench::keep(msg);
            delete[] msg;
        }

        for (Connection* c : conns)
            delete c;
    });

    table.row("4 线程", poolMs, newMs);
}

// ---- 生产者消费者：一个线程分配，另一个线程释放（跨线程归还） ----
void benchCrossThread()
{
    bench::Table table("生产者消费者 · 跨线程分配与释放（20 万对象）");

    constexpr size_t COUNT = 200000;
    constexpr size_t SIZE = 96;

    double poolMs = bench::measureOnce([]() {
        std::vector<void*> queue(COUNT);
        for (size_t i = 0; i < COUNT; ++i)
            queue[i] = MemoryPool::allocate(SIZE);

        std::thread consumer([&queue]() {
            for (void* p : queue)
                MemoryPool::deallocate(p, SIZE);
        });
        consumer.join();
    });

    double newMs = bench::measureOnce([]() {
        std::vector<char*> queue(COUNT);
        for (size_t i = 0; i < COUNT; ++i)
            queue[i] = new char[SIZE];

        std::thread consumer([&queue]() {
            for (char* p : queue)
                delete[] p;
        });
        consumer.join();
    });

    table.row("分配/释放分离", poolMs, newMs);
}
} // namespace

int main()
{
    std::printf("========== 真实场景基准测试 ==========\n");

    benchGameEngine();
    benchNetworkServer();
    benchCrossThread();

    Stats stats = MemoryPool::getStats();
    std::printf("\nThreadCache 命中率 %.2f%%，系统内存 %.2f MB\n",
                stats.hitRate() * 100.0,
                static_cast<double>(stats.systemBytes) / (1024.0 * 1024.0));

    size_t released = MemoryPool::releaseToSystem();
    std::printf("归还系统 %.2f MB\n", static_cast<double>(released) / (1024.0 * 1024.0));
    return 0;
}
