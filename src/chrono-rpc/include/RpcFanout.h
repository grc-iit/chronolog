#ifndef CHRONOLOG_RPC_FANOUT_H
#define CHRONOLOG_RPC_FANOUT_H
#include <RpcDeadline.h>
#include <atomic>
#include <thread>
namespace chronolog::rpc
{
// Fixed admitted roster, not a runtime or a queue. Every child arrives once,
// after issue or after a local no-dispatch failure, before any native wait.
class FanoutIssueBarrier
{
public:
    explicit FanoutIssueBarrier(unsigned count)
        : count_(count)
    {}
    void arriveAndWait(RpcDeadline const& deadline) noexcept
    {
        issued_.fetch_add(1, std::memory_order_acq_rel);
        while(issued_.load(std::memory_order_acquire) < count_ && !deadline.expired())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

private:
    unsigned const count_;
    std::atomic<unsigned> issued_{0};
};
} // namespace chronolog::rpc
#endif
