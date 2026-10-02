#pragma once

// Test only. Defining getaddrinfo in the test executable makes "visor.test" fail with EAI_AGAIN a set
// number of times and then resolve to loopback, the way aardvark-dns behaves while it reloads.
// Include from exactly one translation unit per test binary.
#include <dlfcn.h>
#include <netdb.h>

#include <atomic>
#include <cstdlib>
#include <string_view>

namespace chronolog::rpc::test
{
inline std::atomic<int> failing_lookups{0};
inline std::atomic<int> lookups{0};
// The native resolver is the one the containers run (entrypoint.sh); it must be chosen before gRPC starts.
inline const int native_resolver = ::setenv("GRPC_DNS_RESOLVER", "native", 1);
} // namespace chronolog::rpc::test

extern "C" int getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result) noexcept
{
    using Real = int (*)(const char*, const char*, const addrinfo*, addrinfo**);
    static const Real real = reinterpret_cast<Real>(::dlsym(RTLD_NEXT, "getaddrinfo"));
    if(node && std::string_view(node) == "visor.test")
    {
        ++chronolog::rpc::test::lookups;
        if(chronolog::rpc::test::failing_lookups.fetch_sub(1) > 0)
            return EAI_AGAIN;
        node = "127.0.0.1";
    }
    return real(node, service, hints, result);
}
