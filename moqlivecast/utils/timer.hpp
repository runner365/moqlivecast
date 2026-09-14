#ifndef TIMER_HPP
#define TIMER_HPP
#include <uv.h>
#include <stdint.h>
#include <map>
#include <timeex.hpp>

namespace cpp_streamer {

void StreamerTimerInitialize(uv_loop_t* loop, uint32_t timeout_ms);

class TimerInterface;

class TimerInner
{
public:
    ~TimerInner();

public:
    static TimerInner* GetInstance();

public:
    void Initialize(uv_loop_t* loop, uint32_t timeout_ms);
    void Deinitialize();

private:
    void OnTimer();

public:
    void RegisterTimer(TimerInterface* timer);
    void UnregisterTimer(TimerInterface* timer);
    bool IsRunning();

private:
    TimerInner() = default;

private:
    static void OnUvTimerInnerCallback(uv_timer_t *handle);

private:
    static TimerInner* instance_;

private:
    uv_loop_t* loop_ = nullptr;
    uv_timer_t timer_;
    uint32_t timeout_ms_;
    bool running_ = false;

private:
    std::multimap<int64_t, TimerInterface*> timers_;
};

class TimerInterface
{
public:
    TimerInterface(uint32_t timeout_ms);

    virtual ~TimerInterface();

public:
    virtual bool OnTimer() = 0;

public:
    void StartTimer();
    void StopTimer();
    uint32_t GetTimeOutMs();
    void SetTimeId(int64_t id);
    int64_t GetTimeId();
    bool IsRunning();

protected:
    bool timer_running_ = false;
private:
    uint32_t timeout_ms_;
    int64_t id_ = 0;
};

} // namespace cpp_streamer
#endif
