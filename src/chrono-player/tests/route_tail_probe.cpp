#include <chrono>
#include <iostream>
#include "rpc/Channel.h"
#include "chronolog/v1/chronolog.grpc.pb.h"
int main(int argc, char** argv)
{
    if(argc != 4)
        return 2;
    auto stub = chronolog::v1::Replay::NewStub(chronolog::rpc::peerChannel(argv[1]));
    grpc::ClientContext context;
    chronolog::rpc::withTimeout(context, std::chrono::seconds(std::stoi(argv[3])));
    chronolog::v1::TailRequest request;
    request.set_story_id(std::stoull(argv[2]));
    request.mutable_from()->mutable_id()->set_story_id(request.story_id());
    auto reader = stub->Tail(&context, request);
    chronolog::v1::TailResponse response;
    size_t messages = 0;
    while(reader->Read(&response)) ++messages;
    auto status = reader->Finish();
    std::cout << "tail messages=" << messages << " status=" << status.error_code()
              << " message=" << status.error_message() << '\n';
    return status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ? 0 : 1;
}
