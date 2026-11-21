// Test-only interposer: keep one original HDF5 write live until the fixture
// releases it. No RPC/runtime/thread is added and no service signal is sent.
#include <hdf5.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

extern "C" herr_t
H5Dwrite(hid_t dataset, hid_t memoryType, hid_t memorySpace, hid_t fileSpace, hid_t transfer, void const* buffer)
{
    using Write = herr_t (*)(hid_t, hid_t, hid_t, hid_t, hid_t, void const*);
    static auto original = reinterpret_cast<Write>(dlsym(RTLD_NEXT, "H5Dwrite"));
    static std::atomic<bool> claimed{false};
    auto gate = std::getenv("CHRONOLOG_TEST_ARCHIVE_HOLD");
    bool const held = gate && *gate && !claimed.exchange(true);
    if(held)
    {
        auto path = std::string(gate) + ".entered";
        auto descriptor = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if(descriptor >= 0)
        {
            char record[128];
            auto length = snprintf(record,
                                   sizeof(record),
                                   "%ld %ld %lld\n",
                                   static_cast<long>(getpid()),
                                   syscall(SYS_gettid),
                                   static_cast<long long>(dataset));
            if(length > 0)
                static_cast<void>(write(descriptor, record, static_cast<size_t>(length)));
            close(descriptor);
        }
        auto release = std::string(gate) + ".release";
        while(access(release.c_str(), F_OK) != 0) usleep(20000);
    }
    auto result = original ? original(dataset, memoryType, memorySpace, fileSpace, transfer, buffer) : -1;
    if(held)
    {
        auto path = std::string(gate) + ".completed";
        auto descriptor = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if(descriptor >= 0)
        {
            char record[64];
            auto length = snprintf(record, sizeof(record), "%d\n", result);
            if(length > 0)
                static_cast<void>(write(descriptor, record, static_cast<size_t>(length)));
            close(descriptor);
        }
    }
    return result;
}
