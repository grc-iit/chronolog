#ifndef CHRONOLOG_KEEPER_WRITE_GATE_H
#define CHRONOLOG_KEEPER_WRITE_GATE_H

#include <atomic>
#include <chrono>
#include <thread>

namespace chronolog
{

// The recording service's gate for writes at shutdown. A write enters before
// it ingests and leaves once the event is in the ingestion queue; close()
// shuts the gate and waits for every write that got in to leave, so a write
// answered CL_SUCCESS has reached the queue before data collection stops.
// A write counts itself in before it reads whether the gate is open, and
// close() shuts it before it reads the count (both sequentially consistent),
// so a write that saw the gate open is always waited for.
class KeeperWriteGate
{
public:
    // true: the write may ingest and must call leave() once it has; false:
    // the gate is closed and nothing may be stored
    bool enter()
    {
        ++inside;
        if(!open.load())
        {
            --inside;
            return false;
        }
        return true;
    }

    void leave() { --inside; }

    bool isOpen() const { return open.load(); }

    // Shuts the gate. Returns true once no write that entered is still
    // inside, false if one still is after max_wait.
    bool close(std::chrono::milliseconds max_wait)
    {
        open.store(false);
        auto const deadline = std::chrono::steady_clock::now() + max_wait;
        while(inside.load() != 0)
        {
            if(std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

private:
    std::atomic<bool> open{true};
    std::atomic<int> inside{0};
};

} // namespace chronolog

#endif
