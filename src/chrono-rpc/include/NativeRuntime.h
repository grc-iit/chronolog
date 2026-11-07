#ifndef CHRONOLOG_NATIVE_RUNTIME_H
#define CHRONOLOG_NATIVE_RUNTIME_H
#include <margo.h>
#include <thallium.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace chronolog::rpc
{
struct NativeRetirementSnapshot
{
    uint64_t generation = 0, initializedMids = 0, finalizedMids = 0;
    uint64_t finalizeCalls = 0, finalizeReturns = 0;
    uint64_t constructorUnitsCreated = 0, constructorUnitsRetired = 0;
    uint64_t rpcStreamsCreated = 0, rpcStreamsRetired = 0, poolsCreated = 0, poolsRetired = 0;
    uint64_t primaryReleases = 0;
    int streamsBeforeRelease = -1;
    bool samePrimary = false, abtUninitialized = false, complete = false;
};
// The generation reuses the one ABI primary in the shared chrono_rpc DSO.
// All native methods run on charged workers. Destruction does no native work.
class NativeProcessGeneration
{
public:
    explicit NativeProcessGeneration(std::size_t midCapacity = 1);
    NativeProcessGeneration(NativeProcessGeneration const&) = delete;
    int initialize();
    NativeRetirementSnapshot snapshot() const noexcept;
    int retainMid();
    void retireMid();
    void cleanup();
    bool cleanupComplete() const noexcept { return complete_.load(std::memory_order_acquire); }

private:
    friend class NativeMidRuntime;
    std::atomic<uint64_t> observedGeneration_{0}, initializedMids_{0}, finalizedMids_{0};
    std::atomic<uint64_t> finalizeCalls_{0}, finalizeReturns_{0}, constructorCreated_{0}, constructorRetired_{0};
    std::atomic<uint64_t> streamsCreated_{0}, streamsRetired_{0}, poolsCreated_{0}, poolsRetired_{0}, releases_{0};
    std::atomic<int> streamsBeforeRelease_{-1};
    std::atomic<bool> samePrimary_{false}, abtUninitialized_{false};
    std::mutex mutex_;
    std::size_t const capacity_;
    std::size_t mids_ = 0;
    uint64_t generation_ = 0;
    bool attempted_ = false, closing_ = false, uncertain_ = false;
    int status_ = 0;
    std::atomic<bool> complete_{false};
};
struct NativeMidSpec
{
    std::string address;
    int mode = MARGO_CLIENT_MODE;
    std::size_t handlerStreams = 1;
};
class NativeMidRuntime
{
public:
    explicit NativeMidRuntime(std::shared_ptr<NativeProcessGeneration> process);
    NativeMidRuntime(NativeMidRuntime const&) = delete;
    ~NativeMidRuntime() = default;
    int initialize(NativeMidSpec spec);
    void cleanup();
    std::shared_ptr<thallium::engine> engine() const { return engine_; }
    bool cleanupComplete() const noexcept { return complete_.load(std::memory_order_acquire); }

private:
    static void construct(void*) noexcept;
    std::shared_ptr<NativeProcessGeneration> process_;
    NativeMidSpec spec_;
    bool attempted_ = false, retained_ = false, constructed_ = false, uncertain_ = false, finalizeCalled_ = false;
    int status_ = 0;
    ABT_pool rpcPool_ = ABT_POOL_NULL;
    std::vector<ABT_xstream> rpcStreams_;
    ABT_thread constructor_ = ABT_THREAD_NULL;
    margo_instance_id mid_ = MARGO_INSTANCE_NULL;
    std::shared_ptr<thallium::engine> engine_;
    std::atomic<bool> complete_{false};
};
} // namespace chronolog::rpc
#endif
