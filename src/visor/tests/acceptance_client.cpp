// Tiny Catalog client for the binary acceptance script. Prints one value per call.
//   acceptance_client <address> create-chronicle <name>
//   acceptance_client <address> create-story <chronicle> <name>      prints story_id
//   acceptance_client <address> acquire <story_id> <identity> [cas-prior]
//       prints the incarnation, or HELD for a typed refusal of a live holder. Each invocation is one
//       logical Acquire with its own fresh process-local request id; cas-prior asks for an explicit
//       compare-and-swap takeover of that incarnation.
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <iostream>
#include <random>
#include <string>

#include "chronolog/v1/chronolog.grpc.pb.h"

namespace
{

int fail(const std::string& what, const grpc::Status& status, const chronolog::v1::ItemStatus& item)
{
    std::cerr << what << " failed: grpc " << status.error_code() << " " << status.error_message() << " item "
              << item.code() << " " << item.message() << "\n";
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    if(argc < 4)
    {
        std::cerr << "usage: acceptance_client <address> <command> <args...>\n";
        return 2;
    }
    using namespace chronolog::v1;
    auto stub = Catalog::NewStub(grpc::CreateChannel(argv[1], grpc::InsecureChannelCredentials()));
    const std::string command = argv[2];
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    if(command == "create-chronicle")
    {
        CreateChronicleRequest request;
        request.set_name(argv[3]);
        CreateChronicleResponse response;
        auto status = stub->CreateChronicle(&context, request, &response);
        if(!status.ok() || response.status().code() != 0)
            return fail(command, status, response.status());
        std::cout << response.chronicle().name() << "\n";
        return 0;
    }
    if(command == "create-story" && argc >= 5)
    {
        CreateStoryRequest request;
        request.set_chronicle(argv[3]);
        request.set_name(argv[4]);
        CreateStoryResponse response;
        auto status = stub->CreateStory(&context, request, &response);
        if(!status.ok() || response.status().code() != 0)
            return fail(command, status, response.status());
        std::cout << response.story().story_id() << "\n";
        return 0;
    }
    if(command == "acquire" && argc >= 5)
    {
        AcquireRequest request;
        request.set_story_id(std::stoull(argv[3]));
        request.set_writer_identity(argv[4]);
        std::random_device random;
        std::string id;
        for(int i = 0; i < 4; ++i) id += std::to_string(random()) + "-";
        request.set_acquire_request_id(id);
        if(argc >= 6)
        {
            request.set_takeover(true);
            request.set_expected_prior_incarnation(std::stoull(argv[5]));
        }
        AcquireResponse response;
        auto status = stub->Acquire(&context, request, &response);
        if(status.ok() && response.refusal_reason() == ACQUIRE_REFUSAL_REASON_HELD && response.remaining_ns() > 0)
        {
            std::cout << "HELD\n";
            return 0;
        }
        if(!status.ok() || response.status().code() != 0)
            return fail(command, status, response.status());
        std::cout << response.incarnation() << "\n";
        return 0;
    }
    std::cerr << "unknown command " << command << "\n";
    return 2;
}
