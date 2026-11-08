// Test-only passive observation of the ORIGINAL pinned Margo call and lookup.
#include <margo.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <cstdio>
#include <cstdlib>
namespace
{
void record(char const* phase, char const* name, void* mid, void* handle, int status)
{
    auto path = std::getenv("CHRONOLOG_TEST_NATIVE_TRACE");
    if(!path || !*path)
        return;
    timespec tick{};
    clock_gettime(CLOCK_MONOTONIC, &tick);
    char line[1024];
    int size = snprintf(line,
                        sizeof(line),
                        "{\"pid\":%d,\"phase\":\"%s\",\"name\":\"%s\",\"mid\":\"%p\",\"handle\":\"%p\",\"status\":%d,"
                        "\"ns\":%lld}\n",
                        getpid(),
                        phase,
                        name ? name : "",
                        mid,
                        handle,
                        status,
                        static_cast<long long>(tick.tv_sec) * 1000000000LL + tick.tv_nsec);
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if(fd >= 0)
    {
        if(size > 0 && size < static_cast<int>(sizeof(line)))
            static_cast<void>(write(fd, line, size));
        close(fd);
    }
}
} // namespace
extern "C" hg_return_t margo_provider_forward(uint16_t provider, hg_handle_t handle, void* input)
{
    using Forward = hg_return_t (*)(uint16_t, hg_handle_t, void*);
    static auto original = reinterpret_cast<Forward>(dlsym(RTLD_NEXT, "margo_provider_forward"));
    auto name = margo_handle_get_name(handle);
    auto mid = margo_hg_handle_get_instance(handle);
    record("forward_enter", name, mid, handle, 0);
    auto result = original(provider, handle, input);
    record("forward_return", name, mid, handle, result);
    return result;
}
extern "C" hg_return_t margo_addr_lookup(margo_instance_id mid, char const* address, hg_addr_t* result)
{
    using Lookup = hg_return_t (*)(margo_instance_id, char const*, hg_addr_t*);
    static auto original = reinterpret_cast<Lookup>(dlsym(RTLD_NEXT, "margo_addr_lookup"));
    record("lookup_enter", address, mid, nullptr, 0);
    auto status = original(mid, address, result);
    record("lookup_return", address, mid, nullptr, status);
    return status;
}
