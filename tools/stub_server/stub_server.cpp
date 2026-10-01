// Registers the Catalog, Journal and Replay services without overriding any
// method, so every RPC returns UNIMPLEMENTED. Used by the container and compose
// smoke test until real services exist.

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{

struct Options
{
    std::string host = "0.0.0.0";
    std::string port = "50051";
};

bool parse_args(int argc, char** argv, Options& opts)
{
    if(const char* env = std::getenv("CHRONOLOG_PORT")) opts.port = env;
    if(const char* env = std::getenv("CHRONOLOG_LISTEN_HOST")) opts.host = env;
    for(int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if(arg == "--port" && i + 1 < argc) opts.port = argv[++i];
        else if(arg == "--host" && i + 1 < argc) opts.host = argv[++i];
        else return false;
    }
    return !opts.port.empty();
}

} // namespace

int main(int argc, char** argv)
{
    Options opts;
    if(!parse_args(argc, argv, opts))
    {
        std::cerr << "usage: chronolog_stub_server [--host ADDR] [--port PORT]\n"
                  << "environment: CHRONOLOG_LISTEN_HOST, CHRONOLOG_PORT\n";
        return 2;
    }

    // Block the termination signals before any gRPC thread exists so that every
    // thread inherits the mask and only the watcher below consumes them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    chronolog::v1::Catalog::Service catalog;
    chronolog::v1::Journal::Service journal;
    chronolog::v1::Replay::Service replay;

    const std::string address = opts.host + ":" + opts.port;
    int bound_port = 0;
    grpc::ServerBuilder builder;
    // SO_REUSEPORT would let two stub servers share one port silently.
    builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
    builder.AddListeningPort(address, grpc::InsecureServerCredentials(), &bound_port);
    builder.RegisterService(&catalog);
    builder.RegisterService(&journal);
    builder.RegisterService(&replay);

    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if(!server || bound_port == 0)
    {
        std::cerr << "chronolog_stub_server: cannot listen on " << address << "\n";
        return 1;
    }
    std::cout << "chronolog_stub_server listening on " << opts.host << ":" << bound_port << std::endl;

    std::atomic<bool> finished{false};
    std::thread watcher([&]() {
        const timespec poll = {0, 200 * 1000 * 1000};
        while(!finished.load())
        {
            const int received = sigtimedwait(&signals, nullptr, &poll);
            if(received > 0)
            {
                std::cout << "chronolog_stub_server shutting down on signal " << received << std::endl;
                server->Shutdown();
                return;
            }
        }
    });

    server->Wait();
    finished = true;
    watcher.join();
    return 0;
}
