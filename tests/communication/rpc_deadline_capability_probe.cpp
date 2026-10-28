#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace tl = thallium;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace
{
constexpr auto caller_budget = 200ms;
constexpr auto scheduling_limit = 650ms;
constexpr auto peer_limit = 12s;
constexpr std::size_t work_capacity = 2;
constexpr std::size_t bulk_bytes = 64 * 1024 * 1024;

void require(bool condition, std::string const& message)
{
    if(!condition)
        throw std::runtime_error(message);
}

long elapsed_ms(Clock::time_point since)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

void write_line(int fd, std::string const& line)
{
    std::string bytes = line + "\n";
    std::size_t done = 0;
    while(done != bytes.size())
    {
        auto n = ::write(fd, bytes.data() + done, bytes.size() - done);
        if(n < 0 && errno == EINTR)
            continue;
        require(n > 0, "write to peer failed");
        done += static_cast<std::size_t>(n);
    }
}

std::string read_line(int fd, Clock::time_point deadline)
{
    std::string line;
    for(;;)
    {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        require(left > 0, "peer event deadline expired: " + line);
        pollfd item{fd, POLLIN, 0};
        auto ready = ::poll(&item, 1, static_cast<int>(left));
        if(ready < 0 && errno == EINTR)
            continue;
        require(ready > 0, "peer event missing: " + line);
        char c;
        auto n = ::read(fd, &c, 1);
        if(n < 0 && errno == EINTR)
            continue;
        require(n == 1, "peer closed its event pipe");
        if(c == '\n')
            return line;
        line += c;
    }
}

std::uint64_t checksum(std::vector<char> const& bytes)
{
    std::uint64_t value = 0;
    for(unsigned char c: bytes) value += c;
    return value;
}

int peer_main(std::string const& transport, int command_fd, int event_fd)
{
    tl::engine engine(transport + "://127.0.0.1:0", THALLIUM_SERVER_MODE, true, 2);
    std::atomic<bool> allow_pull{false};
    std::atomic<unsigned> handlers{0};
    std::mutex events;
    auto emit = [&](std::string const& message)
    {
        std::lock_guard<std::mutex> lock(events);
        write_line(event_fd, message);
    };
    auto slow = engine.define("deadline_probe_slow",
                              [&](tl::request const& request, int token, int delay_ms)
                              {
                                  ++handlers;
                                  emit("entered " + std::to_string(token));
                                  margo_thread_sleep(engine.get_margo_instance(), static_cast<double>(delay_ms));
                                  try
                                  {
                                      request.respond(token + 100);
                                  }
                                  catch(std::exception const& error)
                                  {
                                      std::cerr << "late scalar reply: " << error.what() << std::endl;
                                  }
                                  emit("finished " + std::to_string(token));
                                  --handlers;
                              });
    auto pull =
            engine.define("deadline_probe_pull",
                          [&](tl::request const& request, tl::bulk remote, int token, bool delayed)
                          {
                              ++handlers;
                              std::vector<char> target(remote.size(), 0);
                              auto local = engine.expose({{target.data(), target.size()}}, tl::bulk_mode::write_only);
                              emit("bulk_ready " + std::to_string(token));
                              while(delayed && !allow_pull.load()) margo_thread_sleep(engine.get_margo_instance(), 2.0);
                              margo_request transfer = MARGO_REQUEST_NULL;
                              auto rc = margo_bulk_itransfer(engine.get_margo_instance(),
                                                             HG_BULK_PULL,
                                                             request.get_endpoint().get_addr(),
                                                             remote.get_bulk(),
                                                             0,
                                                             local.get_bulk(),
                                                             0,
                                                             target.size(),
                                                             &transfer);
                              require(rc == HG_SUCCESS, "margo_bulk_itransfer failed");
                              int complete = 0;
                              require(margo_test(transfer, &complete) == HG_SUCCESS, "margo_test failed");
                              emit("pull_started " + std::to_string(token) + " pending=" + std::to_string(!complete));
                              if(token == 21)
                                  ::raise(SIGSTOP); // Pause the receiver at the native in-flight barrier.
                              require(margo_wait(transfer) == HG_SUCCESS, "margo_wait bulk failed");
                              auto value = checksum(target);
                              emit("pull_finished " + std::to_string(token) + " checksum=" + std::to_string(value));
                              try
                              {
                                  request.respond(value);
                              }
                              catch(std::exception const& error)
                              {
                                  std::cerr << "late bulk reply: " << error.what() << std::endl;
                              }
                              --handlers;
                          });
    emit("address " + std::string(engine.self()));
    for(;;)
    {
        auto command = read_line(command_fd, Clock::now() + 45s);
        if(command == "pull")
            allow_pull = true;
        else if(command == "reset")
        {
            while(handlers.load() != 0) std::this_thread::sleep_for(2ms);
            allow_pull = false;
            emit("reset");
        }
        else if(command == "stop")
            break;
        else
            throw std::runtime_error("unknown peer command");
    }
    while(handlers.load() != 0) std::this_thread::sleep_for(2ms);
    slow.deregister();
    pull.deregister();
    engine.finalize();
    return 0;
}

class Peer
{
    pid_t pid = -1;
    int commands = -1;
    int events = -1;
    bool paused = false;

public:
    std::string address;

    Peer(std::string const& executable, std::string const& transport)
    {
        int to_peer[2], from_peer[2];
        require(::pipe(to_peer) == 0 && ::pipe(from_peer) == 0, "pipe failed");
        pid = ::fork();
        require(pid >= 0, "fork failed");
        if(pid == 0)
        {
            ::close(to_peer[1]);
            ::close(from_peer[0]);
            auto command_fd = std::to_string(to_peer[0]);
            auto event_fd = std::to_string(from_peer[1]);
            ::execl(executable.c_str(),
                    executable.c_str(),
                    "--peer",
                    transport.c_str(),
                    command_fd.c_str(),
                    event_fd.c_str(),
                    static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(to_peer[0]);
        ::close(from_peer[1]);
        commands = to_peer[1];
        events = from_peer[0];
        auto greeting = next();
        require(greeting.rfind("address ", 0) == 0, "peer did not advertise an address");
        address = greeting.substr(8);
    }

    ~Peer()
    {
        try
        {
            finish();
        }
        catch(std::exception const& error)
        {
            std::cerr << "peer cleanup failed: " << error.what() << std::endl;
        }
    }

    void finish()
    {
        if(pid <= 0)
            return;
        ::kill(pid, SIGCONT);
        write_line(commands, "pull");
        write_line(commands, "stop");
        int status = 0;
        auto result = ::waitpid(pid, &status, 0);
        pid = -1;
        ::close(commands);
        ::close(events);
        require(result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0, "peer exited unsuccessfully");
    }

    std::string next()
    {
        auto event = read_line(events, Clock::now() + peer_limit);
        std::cout << "peer: " << event << std::endl;
        return event;
    }

    void expect(std::string const& prefix)
    {
        auto event = next();
        require(event.rfind(prefix, 0) == 0, "expected " + prefix + ", received " + event);
    }

    void stop_progress()
    {
        require(::kill(pid, SIGSTOP) == 0, "SIGSTOP peer failed");
        int status = 0;
        require(::waitpid(pid, &status, WUNTRACED) == pid && WIFSTOPPED(status), "peer not stopped");
        paused = true;
    }

    void resume()
    {
        require(::kill(pid, SIGCONT) == 0, "SIGCONT peer failed");
        paused = false;
    }

    void allow_pull() { write_line(commands, "pull"); }
    void reset_pull()
    {
        write_line(commands, "reset");
        expect("reset");
    }
};

// This is probe scaffolding, not the production RPC runtime. Two joinable
// workers own at most two continuations, including quarantined ones.
struct WorkResult
{
    std::uint64_t value = 0;
    bool quarantine = false;
    std::string observation;
};

struct Work
{
    std::mutex mutex;
    std::condition_variable ready;
    bool done = false;
    WorkResult result;
    std::exception_ptr error;
    std::function<WorkResult()> run;
};

class OwnedWorkers
{
    std::timed_mutex mutex;
    std::condition_variable_any changed;
    std::deque<std::shared_ptr<Work>> queue;
    std::vector<std::shared_ptr<Work>> quarantine;
    std::vector<std::thread> workers;
    std::size_t admitted = 0;
    std::size_t high_water = 0;
    bool quiesced = false;
    bool stopping = false;
    std::function<void()> teardown_peer;

    void loop()
    {
        for(;;)
        {
            std::shared_ptr<Work> work;
            {
                std::unique_lock<std::timed_mutex> lock(mutex);
                changed.wait(lock, [&] { return stopping || !queue.empty(); });
                if(queue.empty())
                    return;
                work = queue.front();
                queue.pop_front();
            }
            WorkResult result;
            std::exception_ptr error;
            try
            {
                result = work->run();
            }
            catch(...)
            {
                // Unknown transport/ownership state is retained conservatively.
                result.quarantine = true;
                error = std::current_exception();
            }
            if(!result.quarantine)
                work->run = {}; // Release owned endpoints/registrations before capacity.
            {
                std::lock_guard<std::timed_mutex> lock(mutex);
                if(result.quarantine)
                    quarantine.push_back(work);
                else
                    --admitted;
            }
            {
                std::lock_guard<std::mutex> lock(work->mutex);
                work->result = std::move(result);
                work->error = error;
                work->done = true;
            }
            work->ready.notify_all();
            changed.notify_all();
        }
    }

public:
    explicit OwnedWorkers(std::function<void()> teardown)
        : teardown_peer(std::move(teardown))
    {
        for(std::size_t i = 0; i != work_capacity; ++i) workers.emplace_back([this] { loop(); });
    }

    ~OwnedWorkers()
    {
        // Test-harness cleanup only. Production has no peer-exit oracle and
        // must keep this owner alive when drain reports incomplete.
        try
        {
            teardown_peer();
        }
        catch(std::exception const& error)
        {
            std::cerr << "worker cleanup peer fence failed: " << error.what() << std::endl;
            std::terminate(); // Keep quarantined buffers rather than release without the fence.
        }
        {
            std::lock_guard<std::timed_mutex> lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        for(auto& worker: workers) worker.join();
    }

    std::shared_ptr<Work> submit(std::function<WorkResult()> run,
                                 bool finalizer = false,
                                 Clock::time_point deadline = Clock::now() + caller_budget)
    {
        std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
        if(!lock.try_lock_until(deadline) || admitted == work_capacity || (quiesced && !(finalizer && admitted == 0)))
            return {};
        auto work = std::make_shared<Work>();
        work->run = std::move(run);
        ++admitted;
        high_water = std::max(high_water, admitted);
        queue.push_back(work);
        changed.notify_one();
        return work;
    }

    bool drain(Clock::time_point deadline)
    {
        std::unique_lock<std::timed_mutex> lock(mutex, std::defer_lock);
        if(!lock.try_lock_until(deadline))
            return false;
        quiesced = true;
        return changed.wait_until(lock, deadline, [&] { return admitted == 0; });
    }

    std::size_t outstanding()
    {
        std::lock_guard<std::timed_mutex> lock(mutex);
        return admitted;
    }

    std::size_t maximum()
    {
        std::lock_guard<std::timed_mutex> lock(mutex);
        return high_water;
    }

    void harness_peer_exit_fence()
    {
        std::lock_guard<std::timed_mutex> lock(mutex);
        admitted -= quarantine.size();
        for(auto const& work: quarantine) work->run = {};
        quarantine.clear();
        changed.notify_all();
    }
};

bool wait_until(std::shared_ptr<Work> const& work, Clock::time_point deadline)
{
    require(static_cast<bool>(work), "work was not admitted");
    std::unique_lock<std::mutex> lock(work->mutex);
    if(!work->ready.wait_until(lock, deadline, [&] { return work->done; }))
        return false;
    if(work->error)
        std::rethrow_exception(work->error);
    return true;
}

void settled(std::shared_ptr<Work> const& work)
{
    require(wait_until(work, Clock::now() + peer_limit), "owned continuation did not settle");
}

struct Counters
{
    std::atomic<unsigned> registrations{0};
    std::atomic<unsigned> registrations_high_water{0};
    std::atomic<unsigned> deregistrations{0};
    std::atomic<bool> finalized{false};
};

struct EngineOwner
{
    std::shared_ptr<Counters> counters;
    tl::engine engine;
    tl::remote_procedure slow;
    tl::remote_procedure pull;

    EngineOwner(std::string const& transport, std::shared_ptr<Counters> stats)
        : counters(std::move(stats))
        , engine(transport, THALLIUM_CLIENT_MODE, true, 1)
        , slow(engine.define("deadline_probe_slow"))
        , pull(engine.define("deadline_probe_pull"))
    {
        margo_push_finalize_callback(
                engine.get_margo_instance(),
                [](void* data) { static_cast<Counters*>(data)->finalized = true; },
                counters.get());
    }

    void finalize()
    {
        require(counters->registrations == 0, "finalization attempted with live source registrations");
        slow.deregister();
        pull.deregister();
        margo_finalize_and_wait(engine.get_margo_instance());
    }
};

struct Source
{
    std::shared_ptr<EngineOwner> owner;
    std::vector<char> bytes;
    std::optional<tl::bulk> registration;

    Source(std::shared_ptr<EngineOwner> context, std::size_t size)
        : owner(std::move(context))
        , bytes(size, 'Z')
    {
        registration.emplace(owner->engine.expose({{bytes.data(), bytes.size()}}, tl::bulk_mode::read_only));
        auto count = ++owner->counters->registrations;
        auto old = owner->counters->registrations_high_water.load();
        while(old < count && !owner->counters->registrations_high_water.compare_exchange_weak(old, count)) {}
    }

    ~Source()
    {
        registration.reset();
        --owner->counters->registrations;
        ++owner->counters->deregistrations;
    }
};

struct SourceObservation
{
    std::mutex mutex;
    std::weak_ptr<Source> source;

    void observe(std::shared_ptr<Source> const& value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        source = value;
    }

    bool alive()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return !source.expired();
    }
};

std::function<WorkResult()> bulk_work(std::shared_ptr<EngineOwner> owner,
                                      std::string address,
                                      std::shared_ptr<SourceObservation> observation,
                                      int token,
                                      std::size_t size,
                                      bool native_timeout)
{
    return [owner = std::move(owner),
            address = std::move(address),
            observation = std::move(observation),
            token,
            size,
            native_timeout,
            source = std::shared_ptr<Source>()]() mutable
    {
        auto endpoint = owner->engine.lookup(address);
        source = std::make_shared<Source>(owner, size);
        observation->observe(source);
        if(native_timeout)
        {
            try
            {
                auto value = owner->pull.on(endpoint)
                                     .timed(caller_budget, *source->registration, token, true)
                                     .as<std::uint64_t>();
                return WorkResult{value, false, "reply"};
            }
            catch(tl::timeout const&)
            {
                return WorkResult{0, true, "native timeout; remote access remains possible"};
            }
            catch(std::exception const& error)
            {
                return WorkResult{0, true, std::string("transport error; retained: ") + error.what()};
            }
        }
        auto value = owner->pull.on(endpoint)(*source->registration, token, true).as<std::uint64_t>();
        return WorkResult{value, false, "positive reply after pull"};
    };
}

// Native connection setup can outlive the transport's own timed forward.
// This child deliberately remains alive with its owners after reporting the
// bounded caller/drain observations. Only the external test harness terminates
// it, after verifying that it is still alive; production must not do that.
int unreachable_main(std::string const& transport, int event_fd)
{
    auto counters = std::make_shared<Counters>();
    auto owner = std::make_shared<EngineOwner>(transport, counters);
    OwnedWorkers workers([] {});
    unsigned completed = 0;
    for(unsigned i = 0; i != work_capacity; ++i)
    {
        auto start = Clock::now();
        auto work = workers.submit(
                [owner, transport]
                {
                    auto endpoint = owner->engine.lookup(transport + "://192.0.2.1:65534");
                    try
                    {
                        owner->slow.on(endpoint).timed(150ms, 4, 0);
                        throw std::runtime_error("unreachable peer replied");
                    }
                    catch(tl::timeout const&)
                    {
                        return WorkResult{0, true, "native timeout; no remote settlement proof"};
                    }
                    catch(tl::exception const& error)
                    {
                        return WorkResult{0, true, std::string("native transport error: ") + error.what()};
                    }
                });
        if(wait_until(work, start + caller_budget))
            ++completed;
        require(Clock::now() - start <= scheduling_limit, "unreachable caller exceeded scheduling limit");
        std::cout << "PASS owned_unreachable_forward transport=" << transport
                  << " caller_elapsed_ms=" << elapsed_ms(start) << " native_completions=" << completed << std::endl;
    }
    require(workers.outstanding() == work_capacity, "unreachable work lost its fixed capacity");
    for(unsigned i = 0; i != 32; ++i)
        require(!workers.submit([] { return WorkResult{}; }), "unreachable work admitted replacements");
    auto start = Clock::now();
    require(!workers.drain(start + caller_budget) && !counters->finalized, "unreachable engine falsely drained");
    std::cout << "PASS unreachable_shutdown_incomplete elapsed_ms=" << elapsed_ms(start)
              << " outstanding=" << workers.outstanding() << " engine_alive=true process_alive=true" << std::endl;
    write_line(event_fd, "unreachable_observed");
    for(;;) ::pause();
}

class UnreachableObservation
{
    pid_t pid = -1;
    int events = -1;

public:
    UnreachableObservation(std::string const& executable, std::string const& transport)
    {
        int pipe_fds[2];
        require(::pipe(pipe_fds) == 0, "unreachable observation pipe failed");
        pid = ::fork();
        require(pid >= 0, "unreachable observation fork failed");
        if(pid == 0)
        {
            ::close(pipe_fds[0]);
            auto event_fd = std::to_string(pipe_fds[1]);
            ::execl(executable.c_str(),
                    executable.c_str(),
                    "--unreachable",
                    transport.c_str(),
                    event_fd.c_str(),
                    static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(pipe_fds[1]);
        events = pipe_fds[0];
        require(read_line(events, Clock::now() + peer_limit) == "unreachable_observed",
                "unreachable observations missing");
        int status = 0;
        require(::waitpid(pid, &status, WNOHANG) == 0 && ::kill(pid, 0) == 0,
                "incomplete-stop child did not stay alive");
    }

    ~UnreachableObservation()
    {
        if(pid > 0)
        {
            ::kill(pid, SIGTERM); // Test-only cleanup, after the incomplete-stop observations.
            int status = 0;
            ::waitpid(pid, &status, 0);
            ::close(events);
            std::cout << "harness_cleanup unreachable_child_signal=" << (WIFSIGNALED(status) ? WTERMSIG(status) : 0)
                      << " production_settlement=false" << std::endl;
        }
    }
};

int primitives(std::string const& executable, std::string const& transport)
{
    UnreachableObservation unreachable(executable, transport);
    Peer peer(executable, transport); // All fork/exec happens before any local engine/worker.
    auto counters = std::make_shared<Counters>();
    auto owner = std::make_shared<EngineOwner>(transport, counters);
    OwnedWorkers workers([&] { peer.finish(); });
    auto address = peer.address;

    // Both lookup calls are real. Numeric address lookup happens locally on
    // this stack; this is not evidence of a native lookup cancellation API.
    peer.stop_progress();
    for(auto const& candidate: {address, transport + "://192.0.2.1:65534"})
    {
        auto start = Clock::now();
        auto work = workers.submit(
                [owner, candidate]
                {
                    auto endpoint = owner->engine.lookup(candidate);
                    return WorkResult{!endpoint.is_null(), false, std::string(endpoint)};
                });
        require(wait_until(work, start + caller_budget), "numeric lookup did not finish inside caller budget");
        require(work->result.value == 1, "numeric lookup returned null endpoint");
        std::cout << "PASS owned_lookup address=" << candidate << " elapsed_ms=" << elapsed_ms(start) << std::endl;
    }

    auto start = Clock::now();
    auto silent = workers.submit(
            [owner, address]
            {
                auto endpoint = owner->engine.lookup(address);
                auto value = owner->slow.on(endpoint)(3, 5).as<int>();
                return WorkResult{static_cast<std::uint64_t>(value), false, "reply"};
            });
    int caller_output = 41;
    require(!wait_until(silent, start + caller_budget), "stopped peer unexpectedly replied");
    require(Clock::now() - start <= scheduling_limit, "silent forward caller wait exceeded test scheduling limit");
    require(workers.outstanding() == 1 && !counters->finalized, "silent work lost its owner");
    std::cout << "PASS owned_silent_forward caller_elapsed_ms=" << elapsed_ms(start)
              << " retained_contexts=" << workers.outstanding() << std::endl;
    peer.resume();
    peer.expect("entered 3");
    peer.expect("finished 3");
    settled(silent);
    require(silent->result.value == 103 && caller_output == 41, "late result mutated caller output");

    // Local timeout is followed by a real remote handler completion.
    start = Clock::now();
    bool timed_out = false;
    {
        auto endpoint = owner->engine.lookup(address);
        try
        {
            owner->slow.on(endpoint).timed(caller_budget, 5, 800);
        }
        catch(tl::timeout const&)
        {
            timed_out = true;
        }
    }
    require(timed_out && Clock::now() - start <= scheduling_limit, "native forward did not time out");
    std::cout << "PASS native_timeout elapsed_ms=" << elapsed_ms(start) << " remote_cancellation=false" << std::endl;
    peer.expect("entered 5");
    peer.expect("finished 5");

    // Repeated caller timeouts do not admit replacement threads/work.
    std::vector<std::shared_ptr<Work>> held;
    for(int token: {10, 11})
    {
        start = Clock::now();
        auto work = workers.submit(
                [owner, address, token]
                {
                    auto endpoint = owner->engine.lookup(address);
                    return WorkResult{owner->slow.on(endpoint)(token, 1400).as<std::uint64_t>(), false, "reply"};
                });
        require(!wait_until(work, start + caller_budget), "slow capacity work unexpectedly finished");
        held.push_back(work);
    }
    require(workers.outstanding() == work_capacity, "timed-out work did not occupy fixed capacity");
    start = Clock::now();
    for(unsigned i = 0; i != 32; ++i)
        require(!workers.submit([] { return WorkResult{}; }), "capacity exhaustion admitted more work");
    require(Clock::now() - start <= 50ms, "capacity exhaustion was not prompt");
    std::cout << "PASS capacity workers=" << work_capacity << " outstanding=" << workers.outstanding()
              << " rejected=32 elapsed_ms=" << elapsed_ms(start) << std::endl;
    for(unsigned i = 0; i != 4; ++i) peer.next();
    for(auto const& work: held) settled(work);

    // A timeout of the control RPC leaves the handler able to initiate a
    // later real bulk pull. Keep the source even after local margo completion.
    auto quarantined_source = std::make_shared<SourceObservation>();
    start = Clock::now();
    auto ambiguous = workers.submit(bulk_work(owner, address, quarantined_source, 20, 1024 * 1024, true));
    peer.expect("bulk_ready 20");
    settled(ambiguous);
    require(ambiguous->result.quarantine && quarantined_source->alive(), "native timeout released the exposed source");
    require(workers.outstanding() == 1 && counters->registrations == 1, "ambiguous transfer lost quarantine capacity");
    std::cout << "PASS bulk_control_timeout elapsed_ms=" << elapsed_ms(start)
              << " observation=" << ambiguous->result.observation << " source_registered=true" << std::endl;
    peer.allow_pull();
    peer.expect("pull_started 20");
    peer.expect("pull_finished 20 checksum=" + std::to_string(90ULL * 1024 * 1024));
    require(quarantined_source->alive() && counters->registrations == 1,
            "peer progress falsely settled timed-out control");
    std::cout << "PASS late_bulk_pull_after_timeout checksum=" << 90ULL * 1024 * 1024
              << " registration_quarantined=true positive_reply=false" << std::endl;
    peer.reset_pull();

    // Positive reply settlement: pause the receiver only after a real
    // margo_bulk_itransfer has reported an incomplete request.
    auto positive_source = std::make_shared<SourceObservation>();
    start = Clock::now();
    auto bulk = workers.submit(bulk_work(owner, address, positive_source, 21, bulk_bytes, false));
    peer.expect("bulk_ready 21");
    peer.allow_pull();
    peer.expect("pull_started 21 pending=1");
    peer.stop_progress();
    require(!wait_until(bulk, start + caller_budget), "stopped receiver unexpectedly acknowledged bulk");
    require(Clock::now() - start <= scheduling_limit, "bulk caller wait exceeded scheduling limit");
    require(positive_source->alive() && quarantined_source->alive() && counters->registrations == 2,
            "caller expiry released a registered source");
    std::cout << "PASS actual_bulk_pull_paused bytes=" << bulk_bytes << " caller_elapsed_ms=" << elapsed_ms(start)
              << " retained_registrations=" << counters->registrations << std::endl;

    start = Clock::now();
    require(!workers.drain(start + caller_budget), "shutdown falsely completed with live bulk work");
    require(Clock::now() - start <= scheduling_limit && !counters->finalized && positive_source->alive(),
            "incomplete shutdown destroyed live state or exceeded budget");
    require(!workers.submit([] { return WorkResult{}; }), "quiesced runtime admitted new work");
    std::cout << "PASS shutdown_incomplete elapsed_ms=" << elapsed_ms(start)
              << " process_alive=true quiesced=true engine_alive=true" << std::endl;
    peer.resume();
    peer.expect("pull_finished 21 checksum=" + std::to_string(90ULL * bulk_bytes));
    settled(bulk);
    require(bulk->result.value == 90ULL * bulk_bytes, "bulk content corrupted across caller expiry");
    require(!positive_source->alive() && counters->registrations == 1,
            "positive reply did not release its registration");
    require(quarantined_source->alive() && workers.outstanding() == 1, "ambiguous transfer was incorrectly reclaimed");
    start = Clock::now();
    require(!workers.drain(start + caller_budget), "quarantined source falsely reported drained");
    require(!counters->finalized, "engine finalized while quarantine was live");
    std::cout << "PASS permanent_quarantine shutdown_incomplete=true registered_bytes=" << 1024 * 1024 << std::endl;

    // Only the test harness knows its child process exited. This fence is
    // deliberately not claimed as a production settlement primitive.
    peer.finish();
    workers.harness_peer_exit_fence();
    require(!quarantined_source->alive() && counters->registrations == 0, "harness fence did not release registration");
    require(workers.drain(Clock::now() + caller_budget), "work remained after harness teardown fence");
    start = Clock::now();
    auto finalizer = workers.submit(
            [owner]
            {
                owner->finalize();
                return WorkResult{1, false, "finalized after harness peer exit"};
            },
            true);
    bool finalized_in_budget = wait_until(finalizer, start + caller_budget);
    auto finalize_elapsed = elapsed_ms(start);
    if(!finalized_in_budget)
        settled(finalizer); // Observation only; no extra wait is charged to the expired stop attempt.
    require(counters->finalized && counters->deregistrations == 2 && workers.maximum() == work_capacity,
            "engine/registration accounting mismatch");
    std::cout << "PASS engine_finalize after_live_registrations=0 peak_registrations="
              << counters->registrations_high_water << " deregistrations=" << counters->deregistrations
              << " caller_elapsed_ms=" << finalize_elapsed << " finalized_in_budget=" << finalized_in_budget
              << std::endl;
    std::cout << "PRIMITIVES_PASS transport=" << transport << " gate_verdict=see_capability_report" << std::endl;
    return 0;
}

int baseline(std::string const& executable, std::string const& transport)
{
    Peer peer(executable, transport);
    tl::engine engine(transport, THALLIUM_CLIENT_MODE, true, 1);
    auto rpc = engine.define("deadline_probe_slow");
    auto endpoint = engine.lookup(peer.address);
    auto start = Clock::now();
    int value = rpc.on(endpoint)(1, 800);
    auto elapsed = elapsed_ms(start);
    peer.expect("entered 1");
    peer.expect("finished 1");
    require(value == 101, "bad baseline reply");
    std::cout << "RED synchronous_forward caller_budget_ms=" << caller_budget.count() << " elapsed_ms=" << elapsed
              << std::endl;
    rpc.deregister();
    endpoint = tl::endpoint();
    engine.finalize();
    require(elapsed <= caller_budget.count(), "synchronous forward exceeds the caller budget");
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    ::signal(SIGPIPE, SIG_IGN);
    try
    {
        if(argc == 4 && std::string(argv[1]) == "--unreachable")
            return unreachable_main(argv[2], std::stoi(argv[3]));
        if(argc == 5 && std::string(argv[1]) == "--peer")
            return peer_main(argv[2], std::stoi(argv[3]), std::stoi(argv[4]));
        require(argc == 3, "usage: rpc_deadline_capability_probe --baseline|--primitives ofi+sockets|ofi+tcp");
        require(std::string(argv[2]) == "ofi+sockets" || std::string(argv[2]) == "ofi+tcp", "unsupported transport");
        if(std::string(argv[1]) == "--baseline")
            return baseline(argv[0], argv[2]);
        require(std::string(argv[1]) == "--primitives", "unknown probe mode");
        return primitives(argv[0], argv[2]);
    }
    catch(std::exception const& error)
    {
        std::cerr << "FAIL " << error.what() << std::endl;
        return 1;
    }
}
