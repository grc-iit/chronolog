#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>

#include "KeeperConfig.h"
#include "archive/KeeperArchive.h"
#include "adapter/ArchiveService.h"
#include "adapter/Convert.h"
#include "adapter/JournalService.h"
#include "clock/KernelClock.h"
#include "wal/WalJournal.h"
#include "membership/AcquisitionWatcher.h"
#include "membership/ConfigMembership.h"
#include "membership/RouteWatcher.h"
#include "runtime/ClusterClient.h"
#include "runtime/WorkerPool.h"

namespace
{

constexpr size_t kMaxQueuedRequests = 1024;
constexpr int kMaxReceiveBytes = 64 << 20;
constexpr std::chrono::seconds kShutdownDeadline{5};

std::unique_ptr<grpc::Server> startServer(const std::string& address, grpc::Service& service, int& bound_port)
{
    grpc::ServerBuilder builder;
    // SO_REUSEPORT would let two Keepers share one port silently.
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    builder.SetMaxReceiveMessageSize(kMaxReceiveBytes);
    builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &bound_port);
    builder.RegisterService(&service);
    return builder.BuildAndStart();
}

std::string newInstanceId()
{
    std::random_device random;
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%08x%08x", random(), random());
    return buffer;
}

} // namespace

int main(int argc, char** argv)
{
    std::optional<std::string> config_path;
    for(int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if(arg == "--config" && i + 1 < argc)
        {
            config_path = argv[++i];
        }
        else
        {
            std::cerr << "usage: chrono_keeper [--config PATH]\n"
                      << "environment overrides: CHRONOLOG_KEEPER_<KEY>, for example CHRONOLOG_KEEPER_PROCESS_ID\n";
            return 2;
        }
    }

    auto config = chronolog::keeper::KeeperConfig::load(config_path);
    if(!config.ok())
    {
        std::cerr << "chrono_keeper: " << config.status().message() << "\n";
        return 2;
    }

    // Block the termination signals before any thread exists so every thread inherits the
    // mask and only the watcher below consumes them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    using namespace chronolog;
    auto clock = std::make_shared<KernelClock>();
    grpc::ChannelArguments channel_args;
    channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
    channel_args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
    auto visor = grpc::CreateCustomChannel(config->visor_internal, grpc::InsecureChannelCredentials(), channel_args);
    const std::string instance = newInstanceId();
    auto route_stub = std::shared_ptr<internal::v1::Cluster::Stub>(internal::v1::Cluster::NewStub(visor));
    auto policy_version = std::make_shared<std::atomic<uint64_t>>(0);
    auto membership = std::make_shared<keeper::ConfigMembership>(
            config->static_routes,
            [route_stub, process_id = config->process_id, endpoint = config->self_endpoint, instance, policy_version](
                    StoryId story) -> absl::StatusOr<Route>
            {
                grpc::ClientContext context;
                context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
                internal::v1::RegisterRequest request;
                request.set_policy_version(policy_version->load());
                auto* process = request.mutable_process();
                process->set_process_id(process_id);
                process->set_instance(instance);
                process->set_endpoint(endpoint);
                process->set_role(internal::v1::PROCESS_ROLE_KEEPER);
                internal::v1::RegisterResponse response;
                auto status = route_stub->Register(&context, request, &response);
                if(!status.ok())
                    return absl::Status(static_cast<absl::StatusCode>(status.error_code()), status.error_message());
                if(response.status().code() != 0)
                    return absl::Status(static_cast<absl::StatusCode>(response.status().code()),
                                        response.status().message());
                for(const auto& update: response.routes())
                    if(update.story_id() == story)
                        return keeper::convert::fromProto(update.route());
                return absl::NotFoundError("unknown story");
            });
    RamJournalConfig journal_config;
    journal_config.process_id = config->process_id;
    journal_config.instance = instance;
    journal_config.append_ceiling_wait_ms = config->append_ceiling_wait_ms;
    journal_config.payload_max_bytes = config->payload_max_bytes;
    journal_config.causal_floor_skew_limit_ns = config->causal_floor_skew_limit_ns;
    journal_config.dedupe_window = config->dedupe_window;
    WalJournalConfig wal_config{config->wal_dir,
                                config->group_commit_window_ms,
                                config->group_commit_max_bytes,
                                config->reserve_ahead_ms,
                                config->wal_max_bytes,
                                config->wal_segment_bytes};
    std::unique_ptr<WalJournal> owned_journal;
    try
    {
        owned_journal = std::make_unique<WalJournal>(clock, membership, journal_config, wal_config);
    }
    catch(const std::exception& error)
    {
        std::cerr << "chrono_keeper: " << error.what() << "\n";
        return 1;
    }
    auto& journal = *owned_journal;
    const auto recovered_instance = journal.recoveredInstance();
    if(auto status = journal.recordInstance(instance); !status.ok())
    {
        std::cerr << "chrono_keeper: cannot persist instance: " << status << "\n";
        return 1;
    }
    *policy_version = journal.hasPhysicalPolicy() ? PhysicalPolicy{}.version : 0;
    for(const auto& writer: config->static_writers)
        (void)journal.registerWriter(writer.story_id, writer.writer_id, writer.incarnation);

    std::atomic<keeper::ClusterClient*> cluster_ptr{nullptr};
    keeper::AcquisitionWatcher acquisitions(
            journal,
            config->process_id,
            [&cluster_ptr]
            {
                if(auto* client = cluster_ptr.load())
                    client->kick();
            },
            config->static_writers.empty());

    keeper::WorkerPool pool(config->effectiveWorkerThreads(), kMaxQueuedRequests);
    keeper::JournalService journal_service(journal, pool);
    keeper::ArchiveService archive_service(journal, *membership, pool);

    int public_port = 0;
    int internal_port = 0;
    auto public_server = startServer(config->listen, journal_service, public_port);
    if(!public_server || public_port == 0)
    {
        std::cerr << "chrono_keeper: cannot listen on " << config->listen << "\n";
        return 1;
    }
    auto internal_server = startServer(config->internal_listen, archive_service, internal_port);
    if(!internal_server || internal_port == 0)
    {
        std::cerr << "chrono_keeper: cannot listen on " << config->internal_listen << "\n";
        public_server->Shutdown();
        return 1;
    }

    keeper::ClusterClient cluster(visor,
                                  {config->process_id,
                                   instance,
                                   config->self_endpoint,
                                   std::chrono::milliseconds(config->heartbeat_interval_ms),
                                   recovered_instance},
                                  journal,
                                  *membership,
                                  acquisitions);
    if(auto status = cluster.registerNow(); absl::IsFailedPrecondition(status))
    {
        std::cerr << "chrono_keeper: registration refused: " << status << "\n";
        internal_server->Shutdown();
        public_server->Shutdown();
        return 1;
    }
    cluster_ptr = &cluster;
    acquisitions.start(visor);
    keeper::RouteWatcher routes(*membership, visor, config->process_id, instance, &journal);
    cluster.start();
    keeper::KeeperArchiveConfig archive_config{config->story_chunk_duration_secs,
                                               config->seal_interval_ms,
                                               config->chunk_max_bytes,
                                               config->frame_bytes,
                                               config->watermark_resend_timeout_secs,
                                               config->archive_visibility_delay_secs,
                                               config->retention_cap_mb,
                                               config->chunk_max_events,
                                               config->shutdown_confirm_timeout_secs};
    keeper::KeeperArchive archive(journal, *membership, config->process_id, archive_config);
    archive.start();

    std::cout << "journal ready durability=ACCEPTED,DURABLE process_id=" << config->process_id
              << " listen=" << config->listen << " internal_listen=" << config->internal_listen << std::endl;

    std::atomic<bool> finished{false};
    std::thread watcher(
            [&]()
            {
                const timespec poll = {0, 200 * 1000 * 1000};
                while(!finished.load())
                {
                    const int received = sigtimedwait(&signals, nullptr, &poll);
                    if(received > 0)
                    {
                        std::cout << "chrono_keeper shutting down on signal " << received << std::endl;
                        const auto deadline = std::chrono::system_clock::now() + kShutdownDeadline;
                        internal_server->Shutdown(deadline);
                        public_server->Shutdown(deadline);
                        return;
                    }
                }
            });

    public_server->Wait();
    internal_server->Wait();
    finished = true;
    watcher.join();
    // Queued tasks run to completion before the pool joins, then the clients stop.
    pool.stop();
    if(!archive.shutdown())
        std::cerr << "chrono_keeper: archive confirmation timed out or sealing failed\n";
    cluster_ptr = nullptr;
    return 0;
}
