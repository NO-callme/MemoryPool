#pragma once
//
// 极简单元测试框架（与 lyzself 一致）
//
// 环境里没有 gtest，与其引入依赖，不如用 80 行实现同样形状的 API。
// 测试写法与 gtest 一致：TEST(Suite, Name) { ASSERT_EQ(a, b); }
//
#include <cstdio>
#include <exception>
#include <string>
#include <type_traits>
#include <vector>

namespace testing
{

// 断言失败时抛出，由 runner 捕获。
struct AssertionFailure
{
    std::string message;
};

struct TestCase
{
    const char* suite;
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar
{
    Registrar(const char* suite, const char* name, void (*fn)())
    {
        registry().push_back(TestCase{suite, name, fn});
    }
};

inline int runAll()
{
    size_t passed = 0;
    std::vector<std::string> failures;

    std::printf("[==========] 共 %zu 个测试\n", registry().size());

    for (const TestCase& tc : registry())
    {
        std::printf("[ RUN      ] %s.%s\n", tc.suite, tc.name);
        try
        {
            tc.fn();
            std::printf("[       OK ] %s.%s\n", tc.suite, tc.name);
            ++passed;
        }
        catch (const AssertionFailure& e)
        {
            std::printf("[  FAILED  ] %s.%s\n%s\n", tc.suite, tc.name, e.message.c_str());
            failures.push_back(std::string(tc.suite) + "." + tc.name);
        }
        catch (const std::exception& e)
        {
            std::printf("[  FAILED  ] %s.%s  抛出异常: %s\n", tc.suite, tc.name, e.what());
            failures.push_back(std::string(tc.suite) + "." + tc.name);
        }
    }

    std::printf("[==========] %zu 通过, %zu 失败\n", passed, failures.size());
    for (const std::string& name : failures)
        std::printf("[  FAILED  ] %s\n", name.c_str());

    return failures.empty() ? 0 : 1;
}

} // namespace testing

#define TEST(suite, name)                                                     \
    static void suite##_##name##_body();                                      \
    static ::testing::Registrar suite##_##name##_reg(#suite, #name,           \
                                                     suite##_##name##_body);  \
    static void suite##_##name##_body()

#define MP_FAIL(text)                                                         \
    do                                                                        \
    {                                                                         \
        throw ::testing::AssertionFailure{                                    \
            std::string("  ") + __FILE__ + ":" + std::to_string(__LINE__) +   \
            "\n  " + (text)};                                                 \
    } while (false)

#define ASSERT_TRUE(cond)                                                     \
    do                                                                        \
    {                                                                         \
        if (!(cond))                                                          \
            MP_FAIL(std::string("期望为真: ") + #cond);                        \
    } while (false)

#define ASSERT_FALSE(cond)                                                    \
    do                                                                        \
    {                                                                         \
        if ((cond))                                                           \
            MP_FAIL(std::string("期望为假: ") + #cond);                        \
    } while (false)

#define MP_ASSERT_OP(a, b, op)                                                \
    do                                                                        \
    {                                                                         \
        auto mpA = (a);                                                       \
        auto mpB = (b);                                                       \
        if (!(mpA op mpB))                                                    \
            MP_FAIL(std::string("期望 ") + #a + " " #op " " + #b +            \
                    "\n  实际左值: " + ::testing::show(mpA) +                 \
                    "\n  实际右值: " + ::testing::show(mpB));                  \
    } while (false)

#define ASSERT_EQ(a, b) MP_ASSERT_OP(a, b, ==)
#define ASSERT_NE(a, b) MP_ASSERT_OP(a, b, !=)
#define ASSERT_LT(a, b) MP_ASSERT_OP(a, b, <)
#define ASSERT_LE(a, b) MP_ASSERT_OP(a, b, <=)
#define ASSERT_GT(a, b) MP_ASSERT_OP(a, b, >)
#define ASSERT_GE(a, b) MP_ASSERT_OP(a, b, >=)

namespace testing
{
// 断言失败时把值印出来。指针走 %p，其余走 to_string。
template <typename T>
inline std::string show(const T& value)
{
    if constexpr (std::is_pointer_v<T>)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%p", static_cast<const void*>(value));
        return buf;
    }
    else
    {
        return std::to_string(value);
    }
}
} // namespace testing
