#include <grpcpp/grpcpp.h>
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "rpc/Channel.h"
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
    chronolog::internal::v1::MembershipResponse response;
    grpc::Status status;
    if(action == "list" && argc == 3)
    {
        chronolog::internal::v1::ListMembersRequest request;
        status = stub->ListMembers(&context, request, &response);
    }
    else if(argc == 4)
    {
        chronolog::internal::v1::KeeperRequest request;
        request.set_process_id(argv[3]);
        if(action == "drain")
            status = stub->DrainKeeper(&context, request, &response);
        else if(action == "join")
            status = stub->JoinKeeper(&context, request, &response);
        else if(action == "abandon")
            status = stub->AbandonKeeper(&context, request, &response);
        else
            return 2;
    }
    else
        return 2;
    if(!status.ok())
    {
        std::cerr << status.error_message() << "\n";
        return 1;
    }
    std::cout << response.DebugString();
    return response.status().code() == 0 ? 0 : 1;
}
