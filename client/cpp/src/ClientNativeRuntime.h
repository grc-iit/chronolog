#ifndef CHRONOLOG_CLIENT_NATIVE_RUNTIME_H
#define CHRONOLOG_CLIENT_NATIVE_RUNTIME_H
#include <NativeRuntime.h>
namespace chronolog
{
// Compatibility adapter for the actual client's one-MID process generation.
// No process singleton implementation is embedded in the client DSO.
class ClientNativeRuntime
{
public:
    ClientNativeRuntime()
        : native_(std::make_shared<rpc::NativeProcessGeneration>())
    {}
    ClientNativeRuntime(ClientNativeRuntime const&) = delete;
    ~ClientNativeRuntime() = default;
    int initialize(std::string address, int mode);
    void finalize() { native_.finalize(); }
    void awaitApplicationReferences() const { native_.awaitApplicationReferences(); }
    void cleanup();
    std::shared_ptr<thallium::engine> engine() const { return native_.engine(); }

private:
    rpc::NativeMidRuntime native_;
};
} // namespace chronolog
#endif
