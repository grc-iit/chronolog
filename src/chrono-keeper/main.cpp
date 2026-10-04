#include <grpcpp/grpcpp.h>
#include <absl/log/globals.h>
#include <absl/log/initialize.h>
#include <absl/log/log.h>

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
#include "adapter/RouteRead.h"
#include "adapter/JournalService.h"
#include "clock/KernelClock.h"
#include "wal/WalJournal.h"
#include "membership/AcquisitionWatcher.h"
#include "membership/ConfigMembership.h"
#include "membership/RouteWatcher.h"
#include "runtime/ClusterClient.h"
#include "rpc/Channel.h"
#include "worker/WorkerPool.h"

namespace
{

constexpr size_t kMaxQueuedRequests = 1024;
constexpr std::chrono::seconds kShutdownDeadline{5};

std::unique_ptr<grpc::Server> startServer(const std::string& address, grpc::Service& service, int& bound_port)
{
    grpc::ServerBuilder builder;
    // SO_REUSEPORT would let two Keepers share one port silently.
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    builder.SetMaxReceiveMessageSize(chronolog::kKeeperAppendReceiveBytes);
    chronolog::rpc::applyServerPolicy(builder);
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
    absl::InitializeLog();
    std::optional<std::string> config_path;
    bool allow_bind_all = false;
    for(int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if(arg == "--config" && i + 1 < argc)
        {
            config_path = argv[++i];
        }
        else if(arg == "--insecure-bind-all")
        {
            allow_bind_all = true;
        }
        else
        {
            LOG(ERROR) << "usage: chrono_keeper [--config PATH] [--insecure-bind-all]; environment overrides: "
                          "CHRONOLOG_KEEPER_<KEY>, for example CHRONOLOG_KEEPER_PROCESS_ID";
            return 2;
        }
    }

    auto config = chronolog::keeper::KeeperConfig::load(
            config_path,
            [](const char* name) { return std::getenv(name); },
            allow_bind_all);
    if(!config.ok())
    {
        LOG(ERROR) << config.status().message();
        return 2;
    }
    const auto severity = config->log_level == "error"     ? absl::LogSeverityAtLeast::kError
                          : config->log_level == "warning" ? absl::LogSeverityAtLeast::kWarning
                                                           : absl::LogSeverityAtLeast::kInfo;
    absl::SetStderrThreshold(severity);
    absl::SetMinLogLevel(severity);

    // Block the termination signals before any thread exists so every thread inherits the
    // mask and only the watcher below consumes them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    using namespace chronolog;
    auto clock = std::make_shared<KernelClock>();
    auto visor = rpc::peerChannel(config->visor_internal);
    const std::string instance = newInstanceId();
    auto catalog = std::shared_ptr<v1::Catalog::Stub>(v1::Catalog::NewStub(visor));
    auto membership = std::make_shared<keeper::ConfigMembership>(config->static_routes,
                                                                 [catalog](StoryId story)
                                                                 { return keeper::readStoryRoute(*catalog, story); });
    RamJournalConfig journal_config;
    journal_config.process_id = config->process_id;
    journal_config.instance = instance;
    journal_config.append_ceiling_wait_ms = config->append_ceiling_wait_ms;
    journal_config.payload_max_bytes = config->payload_max_bytes;
    journal_config.require_catalog_policy = true;
    journal_config.dedupe_window = config->dedupe_window;
    // Recovery uses the compiled D - S; Register must confirm it before admission.
    WalJournalConfig wal_config{
            config->wal_dir,
            config->group_commit_max_bytes,
            static_cast<uint32_t>((PhysicalPolicy{}.hlc_lead_ns - PhysicalPolicy{}.skew_limit_ns) / 1'000'000),
            config->wal_max_bytes,
            config->wal_segment_bytes,
            config->group_commit_window_us,
            config->wal_reserve_bytes,
            {},
            config->deployment_id};
    std::unique_ptr<WalJournal> owned_journal;
    try
    {
        owned_journal = std::make_unique<WalJournal>(clock, membership, journal_config, wal_config);
    }
    catch(const std::exception& error)
    {
        LOG(ERROR) << error.what();
        return 1;
    }
    auto& journal = *owned_journal;
    const auto recovered_instance = journal.recoveredInstance();
    journal.setRouteResolver([membership, &journal](StoryId story)
                             { return membership->resolve(story, [&] { (void)journal.dropStory(story, true); }); });
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

    WorkerPool pool(config->effectiveWorkerThreads(), kMaxQueuedRequests);
    keeper::JournalService journal_service(journal, pool);
    keeper::ArchiveService archive_service(journal, *membership, pool);

    int public_port = 0;
    int internal_port = 0;
    auto public_server = startServer(config->listen, journal_service, public_port);
    if(!public_server || public_port == 0)
    {
        LOG(ERROR) << "cannot listen on " << config->listen;
        return 1;
    }
    auto internal_server = startServer(config->internal_listen, archive_service, internal_port);
    if(!internal_server || internal_port == 0)
    {
        LOG(ERROR) << "cannot listen on " << config->internal_listen;
        public_server->Shutdown();
        return 1;
    }

    keeper::ClusterClient cluster(visor,
                                  {config->process_id,
                                   instance,
                                   config->self_endpoint,
                                   std::chrono::milliseconds(config->heartbeat_interval_ms),
                                   recovered_instance,
                                   config->keeper_failure_timeout_ms,
                                   config->release_fence_timeout_ms,
                                   clock,
                                   {},
                                   config->causal_floor_skew_limit_ns,
                                   config->reserve_ahead_ms},
                                  journal,
                                  *membership,
                                  acquisitions);
    if(auto status = cluster.registerNow(); absl::IsFailedPrecondition(status))
    {
        LOG(ERROR) << "registration refused: " << status;
        internal_server->Shutdown();
        public_server->Shutdown();
        return 1;
    }
    cluster_ptr = &cluster;
    acquisitions.start(visor);
    // The Catalog answers on the Visor internal port in both modes. An unlisted story the snapshot marker does not
    // conclude destroyed is confirmed here (W10.17).
    keeper::RouteWatcher routes(*membership,
                                visor,
                                config->process_id,
                                instance,
                                &journal,
                                [catalog](StoryId story) -> absl::StatusOr<bool>
                                {
                                    grpc::ClientContext context;
                                    rpc::withTimeout(context, std::chrono::seconds(2));
                                    v1::GetStoryRequest request;
                                    request.set_story_id(story);
                                    v1::GetStoryResponse response;
                                    auto status = catalog->GetStory(&context, request, &response);
                                    if(!status.ok())
                                        return absl::Status(static_cast<absl::StatusCode>(status.error_code()),
                                                            status.error_message());
                                    const auto code = static_cast<absl::StatusCode>(response.status().code());
                                    if(code == absl::StatusCode::kNotFound)
                                        return false;
                                    if(code != absl::StatusCode::kOk)
                                        return absl::Status(code, response.status().message());
                                    return response.story().tombstoned();
                                });
    cluster.start();
    keeper::KeeperArchiveConfig archive_config{config->story_chunk_duration_secs,
                                               config->seal_interval_ms,
                                               config->chunk_max_bytes,
                                               config->frame_bytes,
                                               config->watermark_resend_timeout_secs,
                                               config->archive_visibility_delay_secs,
                                               config->retention_cap_mb,
                                               config->chunk_max_events,
                                               config->shutdown_confirm_timeout_secs,
                                               config->admission_cap_mb,
                                               config->admission_resume_mb};
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
                        LOG(INFO) << "chrono_keeper shutting down on signal " << received;
                        const auto stats = journal.commitStats();
                        LOG(INFO) << "WAL group commit totals: syncs=" << stats.syncs << " records=" << stats.records;
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
        LOG(ERROR) << "archive confirmation timed out or sealing failed";
    cluster_ptr = nullptr;
    return 0;
}
