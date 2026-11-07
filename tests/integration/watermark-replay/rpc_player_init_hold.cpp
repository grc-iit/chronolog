// Test-only: retain the actual second Player constructor call until release.
#include <margo.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
namespace
{
void record(std::string const& path, std::string const& value)
{
    int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if(fd >= 0)
    {
        static_cast<void>(write(fd, value.data(), value.size()));
        close(fd);
    }
}
} // namespace
extern "C" margo_instance_id margo_init_ext(char const* address, int mode, margo_init_info const* options)
{
    using Init = margo_instance_id (*)(char const*, int, margo_init_info const*);
    static auto original = reinterpret_cast<Init>(dlsym(RTLD_NEXT, "margo_init_ext"));
    static std::atomic<unsigned> calls{0};
    char executable[4096]{};
    auto length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    std::string path = length > 0 ? std::string(executable, static_cast<size_t>(length)) : std::string();
    auto gate = std::getenv("CHRONOLOG_TEST_PLAYER_INIT_HOLD");
    bool const held = gate && *gate && path.size() >= 14 && path.compare(path.size() - 14, 14, "/chrono-player") == 0 &&
                      ++calls == 2;
    if(held)
    {
        using Self = int (*)(ABT_thread*);
        using Size = int (*)(ABT_thread, size_t*);
        using Print = int (*)(FILE*);
        auto self = reinterpret_cast<Self>(dlsym(RTLD_NEXT, "ABT_self_get_thread"));
        auto size = reinterpret_cast<Size>(dlsym(RTLD_NEXT, "ABT_thread_get_stacksize"));
        auto print = reinterpret_cast<Print>(dlsym(RTLD_NEXT, "ABT_info_print_config"));
        ABT_thread thread = ABT_THREAD_NULL;
        size_t stack = 0;
        bool native = self && size && self(&thread) == ABT_SUCCESS && size(thread, &stack) == ABT_SUCCESS;
        if(print)
            if(FILE* profile = fopen((std::string(gate) + ".abt-config").c_str(), "w"))
            {
                print(profile);
                fclose(profile);
            }
        auto cache = std::getenv("ABT_MEM_MAX_NUM_STACKS");
        record(std::string(gate) + ".entered",
               std::to_string(getpid()) + " " + std::to_string(syscall(SYS_gettid)) + " " + std::to_string(native) +
                       " " + std::to_string(stack) + " " + (cache ? cache : "unset") + "\n");
        auto release = std::string(gate) + ".release";
        while(access(release.c_str(), F_OK) != 0) usleep(10000);
    }
    auto result = original ? original(address, mode, options) : MARGO_INSTANCE_NULL;
    if(held)
        record(std::string(gate) + ".completed", std::to_string(result != MARGO_INSTANCE_NULL) + "\n");
    return result;
}
