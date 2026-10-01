// ChronoVisor: hosts chronolog.v1.Catalog on `listen` and chronolog.internal.v1.Cluster
// on `internal_listen`. Two gRPC servers keep the internal service off the public port
// so S14.3 can restrict it to the cluster interface.

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "VisorConfig.h"
#include "adapter/CatalogService.h"
#include "adapter/ClusterService.h"
#include "adapter/WorkerPool.h"
#include "catalog/AcquisitionFeed.h"
#include "catalog/SqliteMetadataStore.h"
#include "raft/RaftMetadataStore.h"
#include "membership/StaticRouteMembership.h"

namespace
{

constexpr size_t kMaxQueuedRequests = 1024;
constexpr std::chrono::seconds kShutdownDeadline{5};

std::unique_ptr<grpc::Server>
startServer(const std::string& address, grpc::Service& service, int& bound_port, grpc::Service* extra = nullptr)
{
    grpc::ServerBuilder builder;
    // SO_REUSEPORT would let two Visors share one port silently.
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &bound_port);
    builder.RegisterService(&service);
    if(extra)
        builder.RegisterService(extra);
    return builder.BuildAndStart();
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
            std::cerr << "usage: chrono_visor [--config PATH]\n"
                      << "environment overrides: CHRONOLOG_VISOR_<KEY>, for example CHRONOLOG_VISOR_DB_PATH\n";
            return 2;
        }
    }

    auto config = chronolog::visor::VisorConfig::load(config_path);
    if(!config.ok())
    {
        std::cerr << "chrono_visor: " << config.status().message() << "\n";
        return 2;
    }

    // Block the termination signals before any thread exists so every thread
    // inherits the mask and only the watcher below consumes them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    chronolog::visor::Topology topology{config->keepers, config->grapher, config->player, config->graphers};
    const auto fence_timeout = std::chrono::milliseconds(config->release_fence_timeout_ms);

    // The store needs the membership's fence wait and the membership needs the store's
    // story lookup, so the store is created first and the waiter reads membership
    // through a pointer that is set before the first request can arrive.
    std::atomic<chronolog::visor::StaticRouteMembership*> membership_ptr{nullptr};
    chronolog::visor::FenceWaiter fence_waiter =
            [&membership_ptr, fence_timeout](const chronolog::KeeperRef& keeper, uint64_t revision)
    {
        auto* membership = membership_ptr.load();
        return membership && membership->waitApplied(keeper.process_id, revision, fence_timeout);
    };
    std::unique_ptr<chronolog::MetadataStore> store;
    chronolog::visor::RaftMetadataStore* raft = nullptr;
    chronolog::visor::SqliteMetadataStore* applied = nullptr;
    if(config->membership_mode == "dynamic")
    {
        auto opened = chronolog::visor::RaftMetadataStore::open(config->db_path, topology, config->raft, fence_waiter);
        if(!opened.ok())
        {
            std::cerr << opened.status() << "\n";
            return 1;
        }
        raft = opened->get();
        applied = &raft->appliedStore();
        store = std::move(*opened);
    }
    else
    {
        auto opened = chronolog::visor::SqliteMetadataStore::open(config->db_path, topology, fence_waiter);
        if(!opened.ok())
        {
            std::cerr << opened.status() << "\n";
            return 1;
        }
        applied = opened->get();
        store = std::move(*opened);
    }
    auto& catalog_store = *store;
    auto& ledger = *applied;

    chronolog::visor::StaticRouteMembership membership(
            topology,
            /*epoch=*/1,
            [&ledger](chronolog::StoryId id)
            {
                auto story = ledger.getStory(id);
                return story.ok() && !story->tombstoned;
            },
            std::chrono::milliseconds(config->heartbeat_timeout_ms));
    membership_ptr = &membership;

    chronolog::visor::AcquisitionFeed feed;
    ledger.setObserver(&feed);

    chronolog::visor::WorkerPool pool(config->worker_threads, kMaxQueuedRequests);
    chronolog::visor::CatalogService catalog(catalog_store, pool, raft);
    chronolog::visor::ClusterService cluster(membership, ledger, ledger, feed, raft, &pool);

    int public_port = 0;
    int internal_port = 0;
    auto public_server = startServer(config->listen, catalog, public_port);
    if(!public_server || public_port == 0)
    {
        std::cerr << "chrono_visor: cannot listen on " << config->listen << "\n";
        return 1;
    }
    auto internal_server = startServer(config->internal_listen, cluster, internal_port, raft ? &catalog : nullptr);
    if(!internal_server || internal_port == 0)
    {
        std::cerr << "chrono_visor: cannot listen on " << config->internal_listen << "\n";
        public_server->Shutdown();
        return 1;
    }

    std::cout << "catalog ready db=" << config->db_path << " keepers=" << config->keepers.size()
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
                        std::cout << "chrono_visor shutting down on signal " << received << std::endl;
                        cluster.shutdown();
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
    ledger.setObserver(nullptr);
    return 0;
}
