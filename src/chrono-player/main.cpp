// ChronoPlayer: serves chronolog.v1.Replay on `listen`, reading hot data from every Keeper in
// the story Route through Archive.FetchHot.

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
#include <unistd.h>
#include "rpc/Channel.h"
#include "chrono-player/PlayerConfig.h"
#include "chrono-player/adapter/ClusterClient.h"
#include "chrono-player/adapter/ReplayService.h"
#include "chrono-player/replay/HotReplay.h"
#include "chrono-player/replay/KeeperHotSource.h"
#include "chrono-player/replay/WriterDirectory.h"

namespace
{

constexpr std::chrono::seconds kShutdownDeadline{5};
constexpr int kRegisterAttempts = 60;

} // namespace

int main(int argc, char** argv)
{
    using namespace chronolog;
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
            std::cerr << "usage: chrono_player [--config PATH]\n"
                      << "environment overrides: CHRONOLOG_PLAYER_<KEY>, for example CHRONOLOG_PLAYER_VISOR\n";
            return 2;
        }
    }
    auto config = player::PlayerConfig::load(config_path);
    if(!config.ok())
    {
        std::cerr << "chrono_player: " << config.status().message() << "\n";
        return 2;
    }

    // Block the termination signals before any thread exists so only the watcher consumes them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    std::shared_ptr<player::RouteSource> routes;
    std::shared_ptr<player::WriterDirectory> writers;
    if(config->static_routes)
    {
        routes = std::make_shared<player::StaticRouteSource>(*config->static_routes);
    }
    else
    {
        auto visor = rpc::peerChannel(config->visor_internal);
        writers = std::make_shared<player::WriterDirectory>(visor);
        Process self{config->player_id,
                     config->player_id + "-" + std::to_string(::getpid()) + "-" +
                             std::to_string(std::chrono::system_clock::now().time_since_epoch().count()),
                     config->advertise.empty() ? config->listen : config->advertise,
                     ProcessRole::Player};
        auto cluster = std::make_shared<player::ClusterClient>(visor,
                                                               self,
                                                               std::chrono::milliseconds(config->keeper_deadline_ms));
        cluster->onRoute(
                [writers](const Route& route)
                {
                    for(const auto& keeper: route.keepers) writers->watch(keeper.process_id);
                });
        absl::Status registered;
        for(int attempt = 0; attempt < kRegisterAttempts; ++attempt)
        {
            registered = cluster->registerSelf();
            if(registered.ok())
                break;
            std::cerr << "chrono_player: register attempt " << attempt + 1 << " failed: " << registered.message()
                      << std::endl;
            const timespec pause = {1, 0};
            if(sigtimedwait(&signals, nullptr, &pause) > 0)
                return 0;
        }
        if(!registered.ok())
        {
            std::cerr << "chrono_player: cannot register with the visor\n";
            return 1;
        }
        routes = cluster;
    }

    std::shared_ptr<const player::StoryCatalog> catalog = std::make_shared<player::AnyStoryCatalog>();
    if(!config->visor.empty())
        catalog = std::make_shared<player::CatalogClient>(rpc::peerChannel(config->visor),
                                                          std::chrono::milliseconds(config->keeper_deadline_ms));

    const player::PlayerConfig& cfg = *config;
    player::KeeperHotSourceOptions source_options;
    source_options.read_max_events = cfg.read_max_events;
    source_options.deadline = std::chrono::milliseconds(cfg.keeper_deadline_ms);
    auto source = std::make_shared<player::KeeperHotSource>(
            routes,
            writers,
            [&cfg](const KeeperRef& keeper) { return cfg.keeperInternal(keeper); },
            source_options);
    for(const auto& [id, address]: cfg.keeper_internal) source->warm(address);
    player::HotReplayOptions replay_options;
    replay_options.read_max_events = cfg.read_max_events;
    replay_options.batch_size = cfg.batch_size;
    replay_options.tail_poll = std::chrono::milliseconds(cfg.tail_poll_ms);
    if(!cfg.archive_root.empty())
    {
        auto archive = FileTierStore::OpenReadOnly(cfg.archive_root, std::chrono::milliseconds(cfg.manifest_poll_ms));
        if(!archive.ok())
        {
            std::cerr << "chrono_player: cannot open archive: " << archive.status().message() << "\n";
            return 1;
        }
        replay_options.archive = std::shared_ptr<FileTierStore>(*std::move(archive));
    }
    auto replay = std::make_shared<player::HotReplay>(source, replay_options);
    player::ReplayService service(replay, catalog);

    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    rpc::applyServerPolicy(builder);
    builder.AddListeningPort(cfg.listen, grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if(!server || port == 0)
    {
        std::cerr << "chrono_player: cannot listen on " << cfg.listen << "\n";
        return 1;
    }
    std::cout << "player ready id=" << cfg.player_id << " listen=" << cfg.listen << std::endl;

    std::atomic<bool> finished{false};
    std::thread watcher(
            [&]
            {
                const timespec poll = {0, 200 * 1000 * 1000};
                while(!finished.load())
                {
                    const int received = sigtimedwait(&signals, nullptr, &poll);
                    if(received > 0)
                    {
                        std::cout << "chrono_player shutting down on signal " << received << std::endl;
                        service.shutdown();
                        server->Shutdown(std::chrono::system_clock::now() + kShutdownDeadline);
                        return;
                    }
                }
            });
    server->Wait();
    finished = true;
    watcher.join();
    if(writers)
        writers->stop();
    return 0;
}
