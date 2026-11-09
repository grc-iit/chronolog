// Test-only interposer: hold or fail one rename of a daemon's lifecycle status
// record, chosen by the record's "state" value. The daemon's own publication
// code runs unchanged up to this call. No RPC/runtime/thread is added and no
// service signal is sent.
//
// CHRONOLOG_TEST_STATUS_GATE=<prefix> enables the gate. Every status rename
// appends "<pid> <tid> <state>" to <prefix>.renames.
// CHRONOLOG_TEST_STATUS_HOLD_STATE=<state>: the first such rename writes
// <prefix>.entered, waits for <prefix>.release, renames, then writes the
// result to <prefix>.completed.
// CHRONOLOG_TEST_STATUS_FAIL_STATE=<state>: the first such rename writes
// <prefix>.failed and returns -1 with EIO without renaming.
#include <dlfcn.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>

namespace
{
bool endsWith(char const* text, char const* suffix)
{
    auto length = std::strlen(text), tail = std::strlen(suffix);
    return length >= tail && std::strcmp(text + length - tail, suffix) == 0;
}
std::string stateOf(char const* path)
{
    std::string content;
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if(descriptor < 0)
        return {};
    char block[4096];
    ssize_t count;
    while((count = read(descriptor, block, sizeof(block))) > 0) content.append(block, static_cast<size_t>(count));
    close(descriptor);
    static constexpr char key[] = "\"state\":\"";
    auto start = content.find(key);
    if(start == std::string::npos)
        return {};
    start += sizeof(key) - 1;
    auto end = content.find('"', start);
    return end == std::string::npos ? std::string{} : content.substr(start, end - start);
}
void note(std::string const& path, std::string const& line)
{
    int descriptor = open(path.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0600);
    if(descriptor < 0)
        return;
    auto text = line + "\n";
    static_cast<void>(write(descriptor, text.data(), text.size()));
    close(descriptor);
}
std::string caller(std::string const& state)
{
    return std::to_string(getpid()) + " " + std::to_string(syscall(SYS_gettid)) + " " + state;
}
} // namespace

extern "C" int rename(char const* from, char const* to)
{
    using Rename = int (*)(char const*, char const*);
    static auto original = reinterpret_cast<Rename>(dlsym(RTLD_NEXT, "rename"));
    if(!original)
    {
        errno = ENOSYS;
        return -1;
    }
    auto gate = std::getenv("CHRONOLOG_TEST_STATUS_GATE");
    if(!gate || !*gate || !from || !to || !endsWith(to, ".lifecycle.json"))
        return original(from, to);
    std::string prefix(gate);
    auto state = stateOf(from);
    note(prefix + ".renames", caller(state));
    static std::atomic<bool> held{false}, failed{false};
    auto hold = std::getenv("CHRONOLOG_TEST_STATUS_HOLD_STATE");
    if(hold && state == hold && !held.exchange(true))
    {
        note(prefix + ".entered", caller(state));
        auto release = prefix + ".release";
        while(access(release.c_str(), F_OK) != 0) usleep(20000);
        int result = original(from, to);
        int saved = errno;
        note(prefix + ".completed", std::to_string(result));
        errno = saved;
        return result;
    }
    auto fail = std::getenv("CHRONOLOG_TEST_STATUS_FAIL_STATE");
    if(fail && state == fail && !failed.exchange(true))
    {
        note(prefix + ".failed", caller(state));
        errno = EIO;
        return -1;
    }
    return original(from, to);
}
