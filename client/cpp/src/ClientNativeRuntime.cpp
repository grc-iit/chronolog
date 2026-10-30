#include "ClientNativeRuntime.h"
#include <client_errcode.h>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
namespace chronolog
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
// There is one fixed command cell and one active client generation, no queue.
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
    void release(uint64_t generation)
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
                if(ABT_self_is_primary(&primary) == ABT_SUCCESS && primary == ABT_TRUE &&
                   ABT_xstream_self(&current) == ABT_SUCCESS && current == primary_ &&
                   ABT_xstream_get_num(&count) == ABT_SUCCESS && count == 1 && ABT_finalize() == ABT_SUCCESS &&
                   ABT_initialized() == ABT_ERR_UNINITIALIZED)
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
};
} // namespace
void ClientNativeRuntime::construct(void* pointer) noexcept
{
    auto& self = *static_cast<ClientNativeRuntime*>(pointer);
    try
    {
        margo_init_info options = MARGO_INIT_INFO_INITIALIZER;
        options.json_config = R"({"use_progress_thread":true,"rpc_thread_count":1})";
        options.rpc_pool = self.rpcPool_;
        self.mid_ = margo_init_ext(self.address_.c_str(), self.mode_, &options);
        if(self.mid_ == MARGO_INSTANCE_NULL)
            return; // Native partial-init cleanup is not a safe retry boundary.
        self.engine_ = std::make_shared<thallium::engine>(self.mid_); // non-owning wrapper
        self.constructed_ = true;
    }
    catch(...)
    {}
}
int ClientNativeRuntime::initialize(std::string address, int mode)
{
    if(attempted_)
        return status_;
    attempted_ = true;
    status_ = CL_ERR_UNKNOWN;
    uncertain_ = true;
    status_ = NativeLifecycle::instance().acquire(generation_);
    if(status_ != CL_SUCCESS)
    {
        uncertain_ = status_ != CL_ERR_NATIVE_RUNTIME_UNSUPPORTED;
        return status_;
    }
    status_ = CL_ERR_UNKNOWN;
    address_ = std::move(address);
    mode_ = mode;
    checked(ABT_pool_create_basic(ABT_POOL_FIFO_WAIT, ABT_POOL_ACCESS_MPMC, ABT_FALSE, &rpcPool_),
            "ABT_pool_create_basic");
    checked(ABT_xstream_create_basic(ABT_SCHED_BASIC_WAIT, 1, &rpcPool_, ABT_SCHED_CONFIG_NULL, &rpcES_),
            "ABT_xstream_create_basic");
    checked(ABT_thread_create(rpcPool_, &ClientNativeRuntime::construct, this, ABT_THREAD_ATTR_NULL, &constructor_),
            "ABT_thread_create");
    checked(ABT_thread_join(constructor_), "ABT_thread_join constructor");
    checked(ABT_thread_free(&constructor_), "ABT_thread_free constructor");
    if(constructed_)
    {
        status_ = CL_SUCCESS;
        uncertain_ = false;
    }
    return status_;
}
void ClientNativeRuntime::cleanup()
{
    if(uncertain_)
        throw std::runtime_error("Native initialization or retirement is not proven settled");
    if(!generation_)
        return;
    // Every handler/borrower and outbound operation must have settled before
    // this method. Never call from a handler or on the external RPC ES itself.
    if(mid_ == MARGO_INSTANCE_NULL || finalizeCalled_ || constructor_ != ABT_THREAD_NULL)
        throw std::runtime_error("Native finalization is not safe to repeat");
    uncertain_ = true;
    finalizeCalled_ = true;
    margo_finalize_and_wait(mid_);
    mid_ = MARGO_INSTANCE_NULL;
    engine_.reset();
    checked(ABT_xstream_join(rpcES_), "ABT_xstream_join RPC");
    checked(ABT_xstream_free(&rpcES_), "ABT_xstream_free RPC");
    checked(ABT_pool_free(&rpcPool_), "ABT_pool_free RPC");
    NativeLifecycle::instance().release(generation_);
    generation_ = 0;
    uncertain_ = false;
}
} // namespace chronolog
