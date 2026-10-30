#ifndef CHRONOLOG_CLIENT_NATIVE_RUNTIME_H
#define CHRONOLOG_CLIENT_NATIVE_RUNTIME_H
#include <margo.h>
#include <thallium.hpp>
#include <memory>
#include <string>
namespace chronolog
{
// Native methods are called only by charged RpcRuntime workers. Destruction
// releases std-only state: cleanup must explicitly prove native retirement.
class ClientNativeRuntime
{
public:
    ClientNativeRuntime() = default;
    ClientNativeRuntime(ClientNativeRuntime const&) = delete;
    ~ClientNativeRuntime() = default;
    int initialize(std::string address, int mode);
    void cleanup();
    std::shared_ptr<thallium::engine> engine() const { return engine_; }

private:
    static void construct(void*) noexcept;
    std::string address_;
    int mode_ = MARGO_CLIENT_MODE;
    uint64_t generation_ = 0;
    bool attempted_ = false, constructed_ = false, uncertain_ = false, finalizeCalled_ = false;
    int status_ = 0;
    ABT_pool rpcPool_ = ABT_POOL_NULL;
    ABT_xstream rpcES_ = ABT_XSTREAM_NULL;
    ABT_thread constructor_ = ABT_THREAD_NULL;
    margo_instance_id mid_ = MARGO_INSTANCE_NULL;
    std::shared_ptr<thallium::engine> engine_;
};
} // namespace chronolog
#endif
