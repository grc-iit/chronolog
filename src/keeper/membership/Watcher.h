#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>

namespace chronolog::keeper
{

// Runs `session` until stopped. A session returns true when it made progress, which resets
// the backoff. Between sessions it sleeps 100 ms, doubling up to 2 s.
class Watcher
{
public:
    using Session = std::function<bool(std::stop_token)>;

    explicit Watcher(Session session)
        : thread_([session = std::move(session)](std::stop_token stop) { loop(session, stop); })
    {}

    ~Watcher()
    {
        thread_.request_stop();
        thread_.join();
    }

    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

private:
    static void loop(const Session& session, std::stop_token stop)
    {
        using namespace std::chrono_literals;
        auto delay = 100ms;
        std::mutex mutex;
        std::condition_variable_any cv;
        while(!stop.stop_requested())
        {
            if(session(stop))
                delay = 100ms;
            std::unique_lock lock(mutex);
            cv.wait_for(lock, stop, delay, [] { return false; });
            delay = std::min<std::chrono::milliseconds>(delay * 2, 2000ms);
        }
    }

    std::jthread thread_;
};

} // namespace chronolog::keeper
