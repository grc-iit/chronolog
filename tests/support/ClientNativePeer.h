#ifndef CHRONOLOG_TEST_CLIENT_NATIVE_PEER_H
#define CHRONOLOG_TEST_CLIENT_NATIVE_PEER_H
#include <ClientConfiguration.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
namespace chronolog::test
{
struct PeerEvent
{
    uint64_t time = 0;
    uint64_t storyId = 0;
    uint64_t clientId = 0, size = 0;
    char last = 0;
    uint32_t eventIndex = 0;
    unsigned route = 0;
    char record[128]{};
};
struct PeerState
{
    std::atomic<bool> stop{false}, delay{false}, roster{false}, blockRecords{false};
    std::atomic<bool> delayConnect{false}, delayDisconnect{false}, dropRecordReply{false};
    std::atomic<int> acquireStatus{0}, recordStatus{0}, recordDelayMs{0};
    std::atomic<unsigned> connects{0}, disconnects{0}, acquisitions{0}, destroys{0}, records{0};
    std::atomic<unsigned> storyGeneration{1}, destroyedGeneration{0}, eventCount{0};
    std::atomic_flag eventWriter = ATOMIC_FLAG_INIT;
    PeerEvent events[16];
};
static_assert(std::atomic<bool>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free &&
              std::atomic<int>::is_always_lock_free);
inline bool until(std::function<bool()> condition)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(!condition())
    {
        if(std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
// The peer owns its cold native runtime in a separate process. The parent
// shares only fixed-size controls; it never initializes ABT to host a fixture.
class NativePeer
{
public:
    explicit NativePeer(std::string const& protocol, bool clock = false)
    {
        char path[] = "/tmp/chronolog-native-peer-XXXXXX";
        int fd = mkstemp(path);
        if(fd < 0)
            throw std::runtime_error("peer shared file failed");
        unlink(path);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if(ftruncate(fd, sizeof(PeerState)) != 0)
        {
            close(fd);
            throw std::runtime_error("peer shared file size failed");
        }
        state_ = static_cast<PeerState*>(mmap(nullptr, sizeof(PeerState), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if(state_ == MAP_FAILED)
        {
            state_ = nullptr;
            close(fd);
            throw std::runtime_error("peer shared mapping failed");
        }
        new(state_) PeerState;
        int ready[2];
        if(pipe2(ready, O_CLOEXEC) != 0)
        {
            close(fd);
            releaseMapping();
            throw std::runtime_error("peer ready pipe failed");
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fd, 198);
        posix_spawn_file_actions_adddup2(&actions, ready[1], 199);
        std::string mode = clock ? "clock" : "metadata";
        char* arguments[] = {const_cast<char*>(CHRONOLOG_NATIVE_PEER_PATH),
                             const_cast<char*>(protocol.c_str()),
                             mode.data(),
                             nullptr};
        int status = posix_spawn(&pid_, CHRONOLOG_NATIVE_PEER_PATH, &actions, nullptr, arguments, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(fd);
        close(ready[1]);
        uint16_t port = 0;
        pollfd watched{ready[0], POLLIN, 0};
        bool announced = status == 0 && poll(&watched, 1, 5000) == 1 &&
                         read(ready[0], &port, sizeof(port)) == sizeof(port) && port != 0;
        close(ready[0]);
        if(!announced)
        {
            if(status == 0)
            {
                state_->stop = true;
                waitpid(pid_, nullptr, 0);
            }
            releaseMapping();
            throw std::runtime_error("peer did not announce address");
        }
        portal.PROTO_CONF = protocol;
        portal.PORT = port;
        portal.PROVIDER_ID = clock ? 51 : 73;
        if(!clock)
            portal.RPC_POLICY.control_ms = 40;
    }
    NativePeer(NativePeer const&) = delete;
    ~NativePeer()
    {
        if(!state_)
            return;
        state_->blockRecords = false;
        state_->stop = true;
        // Test watchdog bounds a broken peer; normal retirement is a real wait,
        // not a signal or process-exit substitute for client cleanup proof.
        int status = 0;
        if(waitpid(pid_, &status, 0) != pid_ || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            std::fprintf(stderr, "native child peer failed to retire cleanly\n");
            std::abort();
        }
        releaseMapping();
    }
    PeerState& state() { return *state_; }
    bool until(std::function<bool()> condition) { return test::until(std::move(condition)); }
    ClientPortalServiceConf portal;

private:
    void releaseMapping()
    {
        if(state_)
        {
            state_->~PeerState();
            munmap(state_, sizeof(PeerState));
            state_ = nullptr;
        }
    }
    PeerState* state_ = nullptr;
    pid_t pid_ = -1;
};
} // namespace chronolog::test
#endif
