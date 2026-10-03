#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace
{
// Only delays an explicitly armed syscall; it never changes files, return values or archive data.
void barrier(const char* kind, const char* file)
{
    const char* root = std::getenv("COMPACTION_BARRIER");
    if(!root)
        return;
    char arm[4096], claimed[4096], ready[4096], release[4096], prefix[4096];
    std::snprintf(arm, sizeof arm, "%s/%s.arm", root, kind);
    const int fd = static_cast<int>(::syscall(SYS_openat, AT_FDCWD, arm, O_RDONLY | O_CLOEXEC, 0));
    if(fd < 0)
        return;
    const auto size = ::read(fd, prefix, sizeof prefix - 1);
    ::close(fd);
    if(size <= 0)
        return;
    prefix[size] = '\0';
    if(std::strncmp(file, prefix, static_cast<size_t>(size)) != 0)
        return;
    std::snprintf(claimed, sizeof claimed, "%s/%s.claimed", root, kind);
    if(::rename(arm, claimed) != 0)
        return;
    std::snprintf(ready, sizeof ready, "%s/%s.ready", root, kind);
    const int marker = static_cast<int>(::syscall(SYS_openat, AT_FDCWD, ready, O_CREAT | O_WRONLY, 0600));
    if(marker < 0 || ::write(marker, file, std::strlen(file)) < 0)
        ::_exit(90);
    ::close(marker);
    std::snprintf(release, sizeof release, "%s/%s.release", root, kind);
    // A broken coordinator cannot strand a server worker indefinitely.
    for(int i = 0; i < 2000; ++i)
    {
        if(::access(release, F_OK) == 0)
            return;
        ::poll(nullptr, 0, 10);
    }
    ::_exit(91);
}
} // namespace

extern "C" int open(const char* file, int flags, ...)
{
    mode_t mode = 0;
    if(flags & O_CREAT)
    {
        va_list arguments;
        va_start(arguments, flags);
        mode = static_cast<mode_t>(va_arg(arguments, int));
        va_end(arguments);
    }
    if((flags & O_ACCMODE) == O_RDONLY)
        barrier("read", file);
    return static_cast<int>(::syscall(SYS_openat, AT_FDCWD, file, flags, mode));
}

extern "C" int mkstemp(char* file)
{
    static auto real = reinterpret_cast<int (*)(char*)>(::dlsym(RTLD_NEXT, "mkstemp"));
    const int fd = real(file);
    if(fd >= 0 && std::strstr(file, "/.compact-") != nullptr)
        barrier("compact", file);
    return fd;
}

extern "C" int open64(const char* file, int flags, ...)
{
    mode_t mode = 0;
    if(flags & O_CREAT)
    {
        va_list arguments;
        va_start(arguments, flags);
        mode = static_cast<mode_t>(va_arg(arguments, int));
        va_end(arguments);
    }
    return open(file, flags, mode);
}

extern "C" int mkstemp64(char* file)
{
    static auto real = reinterpret_cast<int (*)(char*)>(::dlsym(RTLD_NEXT, "mkstemp64"));
    const int fd = real(file);
    if(fd >= 0 && std::strstr(file, "/.compact-") != nullptr)
        barrier("compact", file);
    return fd;
}
