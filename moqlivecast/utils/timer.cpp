#include "timer.hpp"
#include <iostream>

namespace cpp_streamer 
{
TimerInner* TimerInner::instance_ = nullptr;

void StreamerTimerInitialize(uv_loop_t* loop, uint32_t timeout_ms) {
    TimerInner::GetInstance()->Initialize(loop, timeout_ms);
}

TimerInner::~TimerInner() {
    Deinitialize();
}


TimerInner* TimerInner::GetInstance() {
    if (instance_ == nullptr) {
        instance_ = new TimerInner();
    }
    return instance_;
}


void TimerInner::Initialize(uv_loop_t* loop, uint32_t timeout_ms) {
    if(running_) {
        return;
    }
    running_ = true;

    timeout_ms_ = timeout_ms;
    loop_ = loop;

    uv_timer_init(loop_, &timer_);
    timer_.data = this;
    uv_timer_start(&timer_, OnUvTimerInnerCallback, timeout_ms_, timeout_ms_);
}

void TimerInner::Deinitialize() {
    if (!running_) {
        return;
    }
    running_ = false;
    uv_timer_stop(&timer_);
}

void TimerInner::RegisterTimer(TimerInterface* timer) {
    int64_t id = now_millisec() + timer->GetTimeOutMs();
    timer->SetTimeId(id);
    timers_.insert(std::make_pair(id, timer));
}
void TimerInner::UnregisterTimer(TimerInterface* timer) {
    for (auto it = timers_.begin(); it != timers_.end(); ) {
        if (it->second == timer) {
            it = timers_.erase(it);
        } else {
            ++it;
        }
    }
}

bool TimerInner::IsRunning() {
    return running_;
}

void TimerInner::OnTimer() {
    if (!running_) return;
    if (timers_.empty()) return;

    int64_t now = now_millisec();

    auto it = timers_.begin();
    // Iterate safely: ensure iterator is valid before dereferencing.
    while (it != timers_.end()) {
        if (it->first > now) break; // stop when the earliest due key is not due

        TimerInterface* timer = it->second;
        // read values we need after callback (can't access timer after it may be
        // deleted), then erase from container so callback can safely unregister
        // itself.
        uint32_t timeout = timer ? timer->GetTimeOutMs() : 0;
        timers_.erase(it);
        if (!timer) {
            it = timers_.begin();
            continue;
        }

        // OnTimer returns whether the timer wants to continue running.
        bool keep_running = timer->OnTimer();
        if (!keep_running) {
            it = timers_.begin();
            continue;
        }

        int64_t new_id = now + timeout;
        // It's assumed that if OnTimer returned true the object is still
        // valid; update id and re-insert.
        timer->SetTimeId(new_id);
        timers_.insert(std::make_pair(new_id, timer));
        it = timers_.begin();
    }
}

void TimerInner::OnUvTimerInnerCallback(uv_timer_t *handle) {
    TimerInner* timer = (TimerInner*)handle->data;
    if (timer && timer->running_) {
        timer->OnTimer();
    }
}

TimerInterface::TimerInterface(uint32_t timeout_ms)
    : timeout_ms_(timeout_ms)
{
}

TimerInterface::~TimerInterface()
{
    StopTimer();
}


void TimerInterface::StartTimer() {
    if (timer_running_) {
        return;
    }
    timer_running_ = true;
    TimerInner::GetInstance()->RegisterTimer(this);
}

void TimerInterface::StopTimer() {
    if (!timer_running_) {
        return;
    }
    timer_running_ = false;
    TimerInner::GetInstance()->UnregisterTimer(this);
}
uint32_t TimerInterface::GetTimeOutMs() {
    return timeout_ms_;
}

void TimerInterface::SetTimeId(int64_t id) {
    id_ = id;
}
int64_t TimerInterface::GetTimeId() {
    return id_;
}

bool TimerInterface::IsRunning() {
    return timer_running_;
}

}//namespace cpp_streamer