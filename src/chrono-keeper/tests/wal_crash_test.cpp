#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <csignal>
#include <fstream>
#include <thread>
#include <random>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "chronolog/v1/chronolog.grpc.pb.h"
#include "wal_harness.h"

namespace chronolog::test
{
namespace
{
using namespace std::chrono_literals;

class StaticPolicyCluster final: public internal::v1::Cluster::Service
{
public:
    ~StaticPolicyCluster()
    {
        if(server)
            server->Shutdown(std::chrono::system_clock::now());
    }
    grpc::Status Register(grpc::ServerContext*,
                          const internal::v1::RegisterRequest*,
                          internal::v1::RegisterResponse* response) override
    {
        const PhysicalPolicy expected;
        auto* policy = response->mutable_policy();
        policy->set_version(expected.version);
        policy->set_acceptance_window_ns(expected.acceptance_window_ns);
        policy->set_skew_limit_ns(expected.skew_limit_ns);
        policy->set_hlc_lead_ns(expected.hlc_lead_ns);
        policy->set_uncertainty_cap_ns(expected.uncertainty_cap_ns);
        return grpc::Status::OK;
    }
    std::unique_ptr<grpc::Server> server;
};

TEST(WalCrash, DurableGrpcAckSurvivesKillAndRestart)
{
    StaticPolicyCluster cluster;
    grpc::ServerBuilder builder;
    int visor_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &visor_port);
    builder.RegisterService(&cluster);
    cluster.server = builder.BuildAndStart();
    ASSERT_NE(cluster.server, nullptr);
    const auto visor_endpoint = "127.0.0.1:" + std::to_string(visor_port);
    auto control = std::make_shared<WalControl>();
    std::string endpoint, archive_endpoint;
    const auto config_path = control->directory + "/keeper.json";
    std::mt19937 random(std::random_device{}());
    auto addresses = [&]
    {
        const unsigned port = 10000 + (random() % 4400) * 5;
        endpoint = "127.0.0.1:" + std::to_string(port);
        archive_endpoint = "127.0.0.1:" + std::to_string(port + 1);
        std::ofstream(config_path)
                << "{\"listen\":\"" << endpoint << "\",\"internal_listen\":\"" << archive_endpoint
                << "\",\"visor_internal\":\"" << visor_endpoint << "\",\"wal_dir\":\"" << control->directory
                << "\",\"worker_threads\":2,\"static_routes\":[{\"story_id\":1,\"epoch\":7,\"keepers\":["
                   "{\"process_id\":\"keeper-1\",\"endpoint\":\""
                << endpoint << "\"}]}],\"static_writers\":[{\"story_id\":1,\"writer_id\":2,\"incarnation\":3}]}";
    };
    struct Child
    {
        pid_t pid{-1};
        ~Child() { kill(); }
        void kill()
        {
            if(pid > 0)
            {
                ::kill(pid, SIGKILL);
                while(::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
                pid = -1;
            }
        }
    } child;
    auto start = [&]
    {
        const auto deadline = std::chrono::system_clock::now() + 30s;
        for(int attempt = 0; attempt < 8 && std::chrono::system_clock::now() < deadline; ++attempt)
        {
            addresses();
            int ready_pipe[2];
            if(::pipe2(ready_pipe, O_CLOEXEC) != 0)
                return false;
            child.pid = ::fork();
            if(child.pid == 0)
            {
                ::dup2(ready_pipe[1], STDOUT_FILENO);
                ::close(ready_pipe[0]);
                ::close(ready_pipe[1]);
                ::execl(CHRONOLOG_KEEPER_BINARY, CHRONOLOG_KEEPER_BINARY, "--config", config_path.c_str(), nullptr);
                ::_exit(127);
            }
            ::close(ready_pipe[1]);
            struct ReadPipe
            {
                int fd;
                ~ReadPipe() { ::close(fd); }
            } ready{ready_pipe[0]};
            if(child.pid < 0)
                return false;
            auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
            auto internal = grpc::CreateChannel(archive_endpoint, grpc::InsecureChannelCredentials());
            while(std::chrono::system_clock::now() < deadline)
            {
                if(channel->WaitForConnected(std::min(deadline, std::chrono::system_clock::now() + 100ms)) &&
                   internal->WaitForConnected(std::min(deadline, std::chrono::system_clock::now() + 100ms)))
                {
                    std::string output;
                    while(std::chrono::system_clock::now() < deadline)
                    {
                        pollfd fd{ready.fd, POLLIN, 0};
                        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       deadline - std::chrono::system_clock::now())
                                                       .count();
                        if(::poll(&fd, 1, static_cast<int>(std::max<int64_t>(remaining, 0))) <= 0)
                            break;
                        char buffer[256];
                        const auto count = ::read(ready.fd, buffer, sizeof(buffer));
                        if(count <= 0)
                            break;
                        output.append(buffer, static_cast<size_t>(count));
                        if(output.find("journal ready durability=") != std::string::npos)
                            return true;
                    }
                    break;
                }
                if(::waitpid(child.pid, nullptr, WNOHANG) == child.pid)
                {
                    child.pid = -1;
                    break;
                }
            }
            child.kill();
        }
        return false;
    };
    constexpr int count = 16;
    std::vector<v1::Hlc> hlcs;
    ASSERT_TRUE(start());
    auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
    ASSERT_TRUE(channel->WaitForConnected(std::chrono::system_clock::now() + 30s));
    auto journal = v1::Journal::NewStub(channel);
    for(int sequence = 1; sequence <= count; ++sequence)
    {
        v1::AppendRequest request;
        request.set_story_id(1);
        request.set_epoch(7);
        request.set_durability(v1::DURABILITY_DURABLE);
        auto* item = request.add_items();
        item->set_writer_id(2);
        item->set_incarnation(3);
        item->set_sequence(sequence);
        item->mutable_physical()->set_physical_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                          std::chrono::system_clock::now().time_since_epoch())
                                                          .count());
        item->mutable_physical()->set_status(v1::CLOCK_STATUS_UNSYNCED);
        item->mutable_envelope()->set_payload("crash event " + std::to_string(sequence));
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 5s);
        v1::AppendResponse response;
        ASSERT_TRUE(journal->Append(&context, request, &response).ok());
        ASSERT_EQ(response.results_size(), 1);
        ASSERT_EQ(response.results(0).status().code(), 0);
        ASSERT_EQ(response.results(0).achieved_durability(), v1::DURABILITY_DURABLE);
        hlcs.push_back(response.results(0).assigned_hlc());
    }
    child.kill();
    ASSERT_TRUE(start());
    auto archive_channel = grpc::CreateChannel(archive_endpoint, grpc::InsecureChannelCredentials());
    ASSERT_TRUE(archive_channel->WaitForConnected(std::chrono::system_clock::now() + 30s));
    auto archive = internal::v1::Archive::NewStub(archive_channel);
    internal::v1::FetchHotRequest request;
    request.set_story_id(1);
    request.mutable_hlc()->mutable_start();
    request.mutable_hlc()->mutable_end()->set_physical_ns(INT64_MAX);
    request.set_max_events(count + 1);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + 5s);
    auto reader = archive->FetchHot(&context, request);
    internal::v1::FetchHotResponse response;
    std::vector<v1::Event> events;
    bool trailer = false;
    // At most 16 single-event batches plus a trailer.
    for(int messages = 0; messages <= count && reader->Read(&response); ++messages)
    {
        if(response.has_trailer())
        {
            trailer = true;
            EXPECT_FALSE(response.trailer().truncated());
        }
        for(const auto& event: response.batch().events())
        {
            ASSERT_LT(events.size(), static_cast<size_t>(count + 1));
            events.push_back(event);
        }
    }
    EXPECT_TRUE(reader->Finish().ok());
    EXPECT_TRUE(trailer);
    ASSERT_EQ(events.size(), static_cast<size_t>(count));
    for(int i = 0; i < count; ++i)
    {
        EXPECT_EQ(events[i].id().sequence(), static_cast<uint64_t>(i + 1));
        EXPECT_EQ(events[i].hlc().physical_ns(), hlcs[i].physical_ns());
        EXPECT_EQ(events[i].hlc().logical(), hlcs[i].logical());
        EXPECT_EQ(events[i].envelope().payload(), "crash event " + std::to_string(i + 1));
        EXPECT_EQ(events[i].durability(), v1::DURABILITY_DURABLE);
    }
}

} // namespace
} // namespace chronolog::test
