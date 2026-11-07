#include "ClientNativeRuntime.h"
namespace chronolog
{
int ClientNativeRuntime::initialize(std::string address, int mode)
{
    return native_.initialize({std::move(address), mode, 1});
}
void ClientNativeRuntime::cleanup() { native_.cleanup(); }
} // namespace chronolog
