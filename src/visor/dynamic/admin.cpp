#include <grpcpp/grpcpp.h>
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "common/rpc/Channel.h"
#include <chrono>
#include <iostream>
int main(int argc, char** argv)
{
    if(argc < 3 || argc > 4)
    {
        std::cerr << "usage: chronolog_admin ENDPOINT list|drain|join|abandon [PROCESS_ID]\n";
        return 2;
    }
    std::string action = argv[2];
    auto stub = chronolog::internal::v1::Cluster::NewStub(chronolog::rpc::peerChannel(argv[1]));
    grpc::ClientContext context;
    chronolog::rpc::withTimeout(context, std::chrono::seconds(5));
    auto finish = [](const grpc::Status& status, const auto& response)
    {
        if(!status.ok())
        {
            std::cerr << status.error_message() << "\n";
            return 1;
        }
        std::cout << response.DebugString();
        return response.status().code() == 0 ? 0 : 1;
    };
    auto keeper = [&](auto request, auto response, auto method)
    {
        request.set_process_id(argv[3]);
        return finish((stub.get()->*method)(&context, request, &response), response);
    };
    namespace iv1 = chronolog::internal::v1;
    if(action == "list" && argc == 3)
    {
        iv1::ListMembersResponse response;
        return finish(stub->ListMembers(&context, iv1::ListMembersRequest(), &response), response);
    }
    if(argc != 4)
        return 2;
    if(action == "drain")
        return keeper(iv1::DrainKeeperRequest(), iv1::DrainKeeperResponse(), &iv1::Cluster::Stub::DrainKeeper);
    if(action == "join")
        return keeper(iv1::JoinKeeperRequest(), iv1::JoinKeeperResponse(), &iv1::Cluster::Stub::JoinKeeper);
    if(action == "abandon")
        return keeper(iv1::AbandonKeeperRequest(), iv1::AbandonKeeperResponse(), &iv1::Cluster::Stub::AbandonKeeper);
    return 2;
}
