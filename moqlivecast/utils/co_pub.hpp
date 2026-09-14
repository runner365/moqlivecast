#ifndef CO_PUB_HPP
#define CO_PUB_HPP
#include <coroutine>
#include <string>
#include <stddef.h>
#include <stdint.h>
#include <exception>
#include <iostream>

namespace cpp_streamer
{
class CoVoidTask
{
public:
    class promise_type
    {
    public:
        CoVoidTask get_return_object() { return {};}
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
    };
};

template<typename T>
class CoTask
{
public:
    class promise_type
    {
    public:
        CoTask get_return_object() {
            return CoTask(co_conn_);
        }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_value(T co_conn) noexcept {
            co_conn_ = co_conn;
        }
        void unhandled_exception() noexcept {
            std::cerr << "Unhandled exception in coroutine" << std::endl;
        }

        T GetConnect() const noexcept {
            return co_conn_;
        }

    private:
        T co_conn_;
    };

public:
    using promise_type = CoTask::promise_type;

    CoTask(T co_conn) : co_conn_(co_conn) {}
    ~CoTask() {}

    T GetConnect() const noexcept {
        return co_conn_;
    }

private:
    T co_conn_;
};
}

#endif