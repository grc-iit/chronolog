#include "chrono-grapher/server/ArchiveService.h"
#include "chrono-grapher/server/GrapherConfig.h"
#include "chrono-grapher/server/TombstoneWatcher.h"
#include "chronolog/v1/chronolog.grpc.pb.h"
#include "rpc/Channel.h"
#include <absl/log/globals.h>
#include <absl/log/initialize.h>
#include <absl/log/log.h>
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <algorithm>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

int main(int argc, char** argv)
{
    const auto process_start = std::chrono::steady_clock::now();
    absl::InitializeLog();
    std::optional<std::string> path;
    bool allow_bind_all = false;
    for(int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i]);
        if(arg == "--config" && i + 1 < argc)
            path = argv[++i];
        else if(arg == "--insecure-bind-all")
            allow_bind_all = true;
        else
        {
            LOG(ERROR) << "usage: chrono_grapher [--config PATH] [--insecure-bind-all]";
            return 2;
        }
    }
    auto config = chronolog::grapher::GrapherConfig::load(path, allow_bind_all);
    if(!config.ok())
    {
        LOG(ERROR) << config.status();
        return 2;
    }
    const auto severity = config->log_level == "error"     ? absl::LogSeverityAtLeast::kError
                          : config->log_level == "warning" ? absl::LogSeverityAtLeast::kWarning
                                                           : absl::LogSeverityAtLeast::kInfo;
    absl::SetStderrThreshold(severity);
    absl::SetMinLogLevel(severity);
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    if(pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0)
        return 1;
    std::shared_ptr<const chronolog::ChunkCodec> codec;
    if(config->archive_codec == "proto")
        codec = std::make_shared<chronolog::ProtoChunkCodec>();
    else
        codec = std::make_shared<chronolog::HDF5ChunkCodec>();
    auto store = chronolog::FileTierStore::Open(config->archive_root, config->manifest_writer, {}, codec);
    if(!store.ok())
    {
        LOG(ERROR) << store.status();
        return 1;
    }
    std::random_device random;
    std::ostringstream identifier;
    identifier << std::hex << std::setfill('0');
    for(int i = 0; i < 4; ++i) identifier << std::setw(8) << random();
    const std::string instance = identifier.str();
    chronolog::grapher::ArchiveService archive(**store, instance, config->limits);
    grpc::ServerBuilder builder;
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    chronolog::rpc::applyServerPolicy(builder);
    builder.SetMaxReceiveMessageSize(static_cast<int>(config->limits.frame_bytes + 4096));
    int port = 0;
    builder.AddListeningPort(config->internal_listen, grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&archive);
    auto server = builder.BuildAndStart();
    if(!server || !port)
    {
        LOG(ERROR) << "cannot listen on " << config->internal_listen;
        return 1;
    }
    const auto visor = chronolog::rpc::peerChannel(config->visor_internal);
    auto stub = chronolog::internal::v1::Cluster::NewStub(visor);
    std::jthread cluster(
            [&](std::stop_token stop)
            {
                bool registered = false;
                size_t policy_cursor = 0;
                std::mutex mutex;
                std::condition_variable_any pause;
                while(!stop.stop_requested())
                {
                    grpc::ClientContext context;
                    chronolog::rpc::withTimeout(context, std::chrono::milliseconds(config->rpc_timeout_ms));
                    std::stop_callback cancelled(stop, [&] { context.TryCancel(); });
                    if(!registered)
                    {
                        chronolog::internal::v1::RegisterRequest request;
                        auto* process = request.mutable_process();
                        process->set_process_id(config->process_id);
                        process->set_instance(instance);
                        process->set_endpoint(config->self_endpoint);
                        process->set_role(chronolog::internal::v1::PROCESS_ROLE_GRAPHER);
                        chronolog::internal::v1::RegisterResponse response;
                        const auto status = stub->Register(&context, request, &response);
                        registered = status.ok() && response.status().code() == 0;
                        if(registered)
                        {
                            LOG(INFO) << "grapher registered "
                                      << std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - process_start)
                                                 .count()
                                      << " ms after start";
                            std::cout << "grapher registered instance=" << instance << std::endl;
                        }
                        if(!registered && !stop.stop_requested())
                            LOG(WARNING) << "grapher registration failed";
                    }
                    else
                    {
                        chronolog::internal::v1::HeartbeatRequest request;
                        request.set_process_id(config->process_id);
                        request.set_instance(instance);
                        auto legacy = (*store)->storiesWithoutPhysicalPolicy();
                        if(legacy.ok() && !legacy->empty())
                        {
                            policy_cursor %= legacy->size();
                            const auto count = std::min<size_t>(65536, legacy->size() - policy_cursor);
                            for(size_t n = 0; n < count; ++n)
                                request.add_stories_without_physical_policy((*legacy)[policy_cursor + n]);
                            policy_cursor += count;
                        }
                        chronolog::internal::v1::HeartbeatResponse response;
                        const auto status = stub->Heartbeat(&context, request, &response);
                        registered = status.ok() && response.status().code() == 0;
                        if(!registered && !stop.stop_requested())
                            LOG_EVERY_N_SEC(WARNING, 5) << "grapher heartbeat failed";
                    }
                    std::unique_lock lock(mutex);
                    pause.wait_for(lock,
                                   stop,
                                   std::chrono::milliseconds(config->heartbeat_interval_ms),
                                   [] { return false; });
                }
            });
    // The Catalog answers on the Visor internal port in both modes. A story the snapshot did not list is confirmed
    // here, never inferred from absence (W10.17).
    auto catalog = std::shared_ptr<chronolog::v1::Catalog::Stub>(chronolog::v1::Catalog::NewStub(visor));
    chronolog::grapher::TombstoneWatcher routes(
            archive,
            visor,
            config->process_id,
            instance,
            [catalog, timeout = config->rpc_timeout_ms](chronolog::StoryId story) -> absl::StatusOr<bool>
            {
                grpc::ClientContext context;
                chronolog::rpc::withTimeout(context, std::chrono::milliseconds(timeout));
                chronolog::v1::GetStoryRequest request;
                request.set_story_id(story);
                chronolog::v1::GetStoryResponse response;
                const auto status = catalog->GetStory(&context, request, &response);
                if(!status.ok())
                    return absl::Status(static_cast<absl::StatusCode>(status.error_code()), status.error_message());
                const auto code = static_cast<absl::StatusCode>(response.status().code());
                if(code == absl::StatusCode::kNotFound)
                    return false;
                if(code != absl::StatusCode::kOk)
                    return absl::Status(code, response.status().message());
                return response.story().tombstoned();
            });
    std::cout << "grapher ready instance=" << instance << " internal_listen=" << config->internal_listen << std::endl;
    int signal = 0;
    const int error = sigwait(&signals, &signal);
    cluster.request_stop();
    archive.shutdown();
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::milliseconds(config->drain_timeout_ms));
    server->Wait();
    cluster.join();
    return error == 0 ? 0 : 1;
}
