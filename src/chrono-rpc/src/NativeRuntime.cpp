#include <NativeRuntime.h>
#include <client_errcode.h>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <pthread.h>
#include <stdexcept>
namespace chronolog::rpc
{
namespace
{
void checked(int status, char const* operation)
{
    if(status != ABT_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed");
}
// One actual process owner, separate from the std-only runtime reaper. B12's
// daemon adapter must reuse/relocate this owner, never create a second owner.
// There is one fixed command cell and one active process generation, no queue.
// The pthread remains process infrastructure between fully retired generations.
class NativeLifecycle
{
public:
    static NativeLifecycle& instance()
    {
        static auto* owner = new NativeLifecycle;
        return *owner;
    }
    int acquire(uint64_t& generation)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if(phase_ == Phase::Rejected)
            return CL_ERR_NATIVE_RUNTIME_UNSUPPORTED;
        if(phase_ != Phase::Dormant || request_ != Request::None)
            return CL_ERR_UNKNOWN;
        phase_ = Phase::Starting;
        request_ = Request::Start;
        ready_.notify_one();
        settled_.wait(lock, [&] { return request_ == Request::None; });
        if(phase_ == Phase::Live)
        {
            generation = generation_;
            return CL_SUCCESS;
        }
        return phase_ == Phase::Rejected ? CL_ERR_NATIVE_RUNTIME_UNSUPPORTED : CL_ERR_UNKNOWN;
    }
    struct ReleaseProof
    {
        bool samePrimary;
        int streams;
        bool uninitialized;
    };
    ReleaseProof release(uint64_t generation)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if(phase_ != Phase::Live || generation == 0 || generation != generation_ || request_ != Request::None)
            throw std::runtime_error("Invalid native lifecycle generation");
        phase_ = Phase::Finalizing;
        request_ = Request::Finish;
        ready_.notify_one();
        settled_.wait(lock, [&] { return request_ == Request::None; });
        if(phase_ != Phase::Dormant)
            throw std::runtime_error("Native lifecycle retirement incomplete");
        return releaseProof_;
    }

private:
    enum class Phase
    {
        Dormant,
        Starting,
        Live,
        Finalizing,
        Rejected,
        Poisoned
    };
    enum class Request
    {
        None,
        Start,
        Finish
    };
    NativeLifecycle()
    {
        if(pthread_create(&thread_, nullptr, &NativeLifecycle::entry, this) != 0)
            phase_ = Phase::Poisoned;
        // No destructor/join runs on a caller, loader finalizer or process
        // reaper. This fixed owner and its pthread have process lifetime.
    }
    static void* entry(void* pointer) noexcept
    {
        static_cast<NativeLifecycle*>(pointer)->loop();
        return nullptr;
    }
    void loop() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for(;;)
        {
            ready_.wait(lock, [&] { return request_ != Request::None; });
            auto request = request_;
            lock.unlock();
            Phase result = Phase::Poisoned;
            if(request == Request::Start)
            {
                // Reject foreign native state before environment mutation or
                // Margo construction. Restore Margo's normal cold profile via
                // its public pre-ABT API: 2MiB stacks and eight cached stacks.
                // Check effective settings too: the pinned setter does not
                // propagate its internal setenv failure through the return code.
                if(ABT_initialized() == ABT_SUCCESS)
                    result = Phase::Rejected;
                else if(ABT_initialized() == ABT_ERR_UNINITIALIZED && margo_set_environment(nullptr) == 0 &&
                        std::getenv("ABT_THREAD_STACKSIZE") &&
                        std::strcmp(std::getenv("ABT_THREAD_STACKSIZE"), "2097152") == 0 &&
                        std::getenv("ABT_MEM_MAX_NUM_STACKS") &&
                        std::strcmp(std::getenv("ABT_MEM_MAX_NUM_STACKS"), "8") == 0 &&
                        ABT_init(0, nullptr) == ABT_SUCCESS)
                {
                    size_t stackSize = 0;
                    ABT_bool primary = ABT_FALSE;
                    ABT_xstream current = ABT_XSTREAM_NULL;
                    if(ABT_self_is_primary(&primary) == ABT_SUCCESS && primary == ABT_TRUE &&
                       ABT_xstream_self(&current) == ABT_SUCCESS &&
                       ABT_info_query_config(ABT_INFO_QUERY_KIND_DEFAULT_THREAD_STACKSIZE, &stackSize) == ABT_SUCCESS &&
                       stackSize == 2097152)
                    {
                        primary_ = current;
                        result = Phase::Live;
                    }
                    // A foreign racing initializer can make ABT_init warm.
                    // Never finalize/retry that uncertain reference or expose
                    // it to Margo; retain the poisoned generation instead.
                }
            }
            else
            {
                ABT_bool primary = ABT_FALSE;
                ABT_xstream current = ABT_XSTREAM_NULL;
                int count = -1;
                bool same = ABT_self_is_primary(&primary) == ABT_SUCCESS && primary == ABT_TRUE &&
                            ABT_xstream_self(&current) == ABT_SUCCESS && current == primary_;
                bool uninitialized = false;
                if(same && ABT_xstream_get_num(&count) == ABT_SUCCESS && count == 1 && ABT_finalize() == ABT_SUCCESS)
                    uninitialized = ABT_initialized() == ABT_ERR_UNINITIALIZED;
                releaseProof_ = {same, count, uninitialized};
                if(uninitialized)
                    result = Phase::Dormant;
            }
            lock.lock();
            if(result == Phase::Live)
            {
                if(generation_ == std::numeric_limits<uint64_t>::max())
                    result = Phase::Poisoned;
                else
                    ++generation_;
            }
            phase_ = result;
            request_ = Request::None;
            settled_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_, settled_;
    pthread_t thread_{};
    Phase phase_ = Phase::Dormant;
    Request request_ = Request::None;
    uint64_t generation_ = 0;
    ABT_xstream primary_ = ABT_XSTREAM_NULL;
    ReleaseProof releaseProof_{false, -1, false};
};
} // namespace
NativeProcessGeneration::NativeProcessGeneration(std::size_t capacity)
    : capacity_(capacity)
{
    if(capacity == 0 || capacity > 3)
        throw std::invalid_argument("A native process generation owns one fixed MID set (1..3)");
}
int NativeProcessGeneration::initialize()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(closing_)
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    if(attempted_)
        return status_;
    attempted_ = true;
    uncertain_ = true;
    status_ = NativeLifecycle::instance().acquire(generation_);
    if(status_ == CL_SUCCESS)
        observedGeneration_.store(generation_, std::memory_order_release);
    uncertain_ = status_ != CL_SUCCESS && status_ != CL_ERR_NATIVE_RUNTIME_UNSUPPORTED;
    return status_;
}
NativeRetirementSnapshot NativeProcessGeneration::snapshot() const noexcept
{
    NativeRetirementSnapshot result;
    result.generation = observedGeneration_.load(std::memory_order_acquire);
    result.initializedMids = initializedMids_.load(std::memory_order_acquire);
    result.finalizedMids = finalizedMids_.load(std::memory_order_acquire);
    result.finalizeCalls = finalizeCalls_.load(std::memory_order_acquire);
    result.finalizeReturns = finalizeReturns_.load(std::memory_order_acquire);
    result.constructorUnitsCreated = constructorCreated_.load(std::memory_order_acquire);
    result.constructorUnitsRetired = constructorRetired_.load(std::memory_order_acquire);
    result.rpcStreamsCreated = streamsCreated_.load(std::memory_order_acquire);
    result.rpcStreamsRetired = streamsRetired_.load(std::memory_order_acquire);
    result.poolsCreated = poolsCreated_.load(std::memory_order_acquire);
    result.poolsRetired = poolsRetired_.load(std::memory_order_acquire);
    result.primaryReleases = releases_.load(std::memory_order_acquire);
    result.streamsBeforeRelease = streamsBeforeRelease_.load(std::memory_order_acquire);
    result.samePrimary = samePrimary_.load(std::memory_order_acquire);
    result.abtUninitialized = abtUninitialized_.load(std::memory_order_acquire);
    result.complete = complete_.load(std::memory_order_acquire);
    return result;
}
int NativeProcessGeneration::retainMid()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(closing_ || status_ != CL_SUCCESS || !generation_)
        return CL_ERR_SHUTDOWN_INCOMPLETE;
    if(mids_ == capacity_)
        return CL_ERR_RPC_CAPACITY;
    ++mids_;
    return CL_SUCCESS;
}
void NativeProcessGeneration::retireMid()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if(!mids_)
        throw std::logic_error("Native MID retirement has no retained generation");
    --mids_;
    finalizedMids_.fetch_add(1, std::memory_order_release);
}
void NativeProcessGeneration::cleanup()
{
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
    if(uncertain_)
        throw std::runtime_error("Native process initialization or retirement remains uncertain");
    if(mids_ != 0)
        return; // The last actual MID's charged cleanup completes the generation.
    if(generation_)
    {
        uncertain_ = true;
        auto proof = NativeLifecycle::instance().release(generation_);
        samePrimary_.store(proof.samePrimary, std::memory_order_release);
        streamsBeforeRelease_.store(proof.streams, std::memory_order_release);
        abtUninitialized_.store(proof.uninitialized, std::memory_order_release);
        releases_.fetch_add(1, std::memory_order_release);
        generation_ = 0;
        uncertain_ = false;
    }
    complete_.store(true, std::memory_order_release);
}
NativeMidRuntime::NativeMidRuntime(std::shared_ptr<NativeProcessGeneration> process)
    : process_(std::move(process))
{
    if(!process_)
        throw std::invalid_argument("Native MID needs its actual process generation");
}
void NativeMidRuntime::construct(void* pointer) noexcept
{
    auto& self = *static_cast<NativeMidRuntime*>(pointer);
    try
    {
        margo_init_info options = MARGO_INIT_INFO_INITIALIZER;
        options.json_config = R"({"use_progress_thread":true,"rpc_thread_count":1})";
        options.rpc_pool = self.rpcPool_;
        self.mid_ = margo_init_ext(self.spec_.address.c_str(), self.spec_.mode, &options);
        if(self.mid_ == MARGO_INSTANCE_NULL)
            return;
        self.process_->initializedMids_.fetch_add(1, std::memory_order_release);
        self.engine_ = std::make_shared<thallium::engine>(self.mid_);
        self.constructed_ = true;
    }
    catch(...)
    {}
}
int NativeMidRuntime::initialize(NativeMidSpec spec)
{
    if(attempted_)
        return status_;
    attempted_ = true;
    status_ = CL_ERR_INVALID_ARG;
    if(spec.handlerStreams == 0 || spec.handlerStreams > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return status_;
    spec_ = std::move(spec);
    uncertain_ = true;
    status_ = process_->initialize();
    if(status_ != CL_SUCCESS)
    {
        uncertain_ = status_ != CL_ERR_NATIVE_RUNTIME_UNSUPPORTED && status_ != CL_ERR_SHUTDOWN_INCOMPLETE;
        return status_;
    }
    status_ = process_->retainMid();
    if(status_ != CL_SUCCESS)
    {
        uncertain_ = false;
        return status_;
    }
    retained_ = true;
    status_ = CL_ERR_UNKNOWN;
    checked(ABT_pool_create_basic(ABT_POOL_FIFO_WAIT, ABT_POOL_ACCESS_MPMC, ABT_FALSE, &rpcPool_),
            "ABT_pool_create_basic");
    process_->poolsCreated_.fetch_add(1, std::memory_order_release);
    rpcStreams_.resize(spec_.handlerStreams, ABT_XSTREAM_NULL);
    for(auto& stream: rpcStreams_)
    {
        checked(ABT_xstream_create_basic(ABT_SCHED_BASIC_WAIT, 1, &rpcPool_, ABT_SCHED_CONFIG_NULL, &stream),
                "ABT_xstream_create_basic");
        process_->streamsCreated_.fetch_add(1, std::memory_order_release);
    }
    checked(ABT_thread_create(rpcPool_, &NativeMidRuntime::construct, this, ABT_THREAD_ATTR_NULL, &constructor_),
            "ABT_thread_create");
    process_->constructorCreated_.fetch_add(1, std::memory_order_release);
    checked(ABT_thread_join(constructor_), "ABT_thread_join constructor");
    checked(ABT_thread_free(&constructor_), "ABT_thread_free constructor");
    process_->constructorRetired_.fetch_add(1, std::memory_order_release);
    if(constructed_)
    {
        uncertain_ = false;
        status_ = CL_SUCCESS;
    }
    return status_;
}
void NativeMidRuntime::cleanup()
{
    if(complete_.load(std::memory_order_acquire))
        return;
    if(uncertain_)
        throw std::runtime_error("Native initialization or retirement is not proven settled");
    if(retained_)
    {
        // Call only after non-handler/outgoing/bulk access and copied requests
        // settle. The proved owned finalizer supplies the incoming wrapper fence.
        if(mid_ == MARGO_INSTANCE_NULL || finalizeCalled_ || constructor_ != ABT_THREAD_NULL)
            throw std::runtime_error("Native finalization is not safe to repeat");
        uncertain_ = true;
        finalizeCalled_ = true;
        process_->finalizeCalls_.fetch_add(1, std::memory_order_release);
        margo_finalize_and_wait(mid_);
        process_->finalizeReturns_.fetch_add(1, std::memory_order_release);
        mid_ = MARGO_INSTANCE_NULL;
        engine_.reset(); // Original wrapper is non-owning; no native destructor.
        for(auto& stream: rpcStreams_)
        {
            checked(ABT_xstream_join(stream), "ABT_xstream_join RPC");
            checked(ABT_xstream_free(&stream), "ABT_xstream_free RPC");
            process_->streamsRetired_.fetch_add(1, std::memory_order_release);
        }
        rpcStreams_.clear();
        checked(ABT_pool_free(&rpcPool_), "ABT_pool_free RPC");
        process_->poolsRetired_.fetch_add(1, std::memory_order_release);
        process_->retireMid();
        retained_ = false;
        uncertain_ = false;
    }
    process_->cleanup();
    complete_.store(true, std::memory_order_release);
}
} // namespace chronolog::rpc
