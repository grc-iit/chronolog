#include "chrono-grapher/server/ArchiveService.h"
#include "chrono-grapher/server/GrapherConfig.h"
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>

int main(int argc, char** argv)
{
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
            std::cerr << "usage: chrono_grapher [--config PATH] [--insecure-bind-all]\n";
            return 2;
        }
    }
    auto config = chronolog::grapher::GrapherConfig::load(path, allow_bind_all);
    if(!config.ok())
    {
        std::cerr << config.status() << '\n';
        return 2;
    }
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    if(pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0)
        return 1;
    auto store = chronolog::FileTierStore::Open(config->archive_root, config->manifest_writer);
    if(!store.ok())
    {
        std::cerr << store.status() << '\n';
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
    builder.SetMaxReceiveMessageSize(static_cast<int>(config->limits.frame_bytes + 4096));
    int port = 0;
    builder.AddListeningPort(config->internal_listen, grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&archive);
    auto server = builder.BuildAndStart();
    if(!server || !port)
    {
        std::cerr << "cannot listen on " << config->internal_listen << '\n';
        return 1;
    }
    auto stub = chronolog::internal::v1::Cluster::NewStub(
            grpc::CreateChannel(config->visor_internal, grpc::InsecureChannelCredentials()));
    std::jthread cluster(
            [&](std::stop_token stop)
            {
                bool registered = false;
                std::mutex mutex;
                std::condition_variable_any pause;
                while(!stop.stop_requested())
                {
                    grpc::ClientContext context;
                    context.set_deadline(std::chrono::system_clock::now() +
                                         std::chrono::milliseconds(config->rpc_timeout_ms));
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
                            std::cout << "grapher registered instance=" << instance << std::endl;
                        if(!registered && !stop.stop_requested())
                            std::cerr << "grapher registration failed\n";
                    }
                    else
                    {
                        chronolog::internal::v1::HeartbeatRequest request;
                        request.set_process_id(config->process_id);
                        request.set_instance(instance);
                        chronolog::internal::v1::HeartbeatResponse response;
                        const auto status = stub->Heartbeat(&context, request, &response);
                        registered = status.ok() && response.status().code() == 0;
                        if(!registered && !stop.stop_requested())
                            std::cerr << "grapher heartbeat failed\n";
                    }
                    std::unique_lock lock(mutex);
                    pause.wait_for(lock,
                                   stop,
                                   std::chrono::milliseconds(config->heartbeat_interval_ms),
                                   [] { return false; });
                }
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
