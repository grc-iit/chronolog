#include <chrono>
#include <iostream>
#include <stdexcept>
#include <grpcpp/grpcpp.h>
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
#include "chronolog/v1/chronolog.grpc.pb.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"

using Json = nlohmann::json;
namespace pub = chronolog::v1;
namespace internal = chronolog::internal::v1;

Json encode(const google::protobuf::Message& message)
{
    std::string text;
    google::protobuf::util::JsonPrintOptions options;
    options.preserve_proto_field_names = true;
    auto status = google::protobuf::util::MessageToJsonString(message, &text, options);
    if(!status.ok())
        throw std::runtime_error(status.ToString());
    return Json::parse(text);
}

template <class Request, class Response, class Fn>
Json unary(const Json& input, grpc::ClientContext& context, Fn fn)
{
    Request request;
    auto parsed = google::protobuf::util::JsonStringToMessage(input.dump(), &request);
    if(!parsed.ok())
        throw std::runtime_error(parsed.ToString());
    Response response;
    auto status = fn(&context, request, &response);
    Json result = {{"transport", status.error_code()},
                   {"error", status.error_message()},
                   {"response", encode(response)}};
    auto metadata = context.GetServerInitialMetadata();
    auto it = metadata.find("chronolog-raft-leader");
    if(it != metadata.end())
        result["leader"] = std::stoi(std::string(it->second.data(), it->second.length()));
    return result;
}

Json call(const Json& command)
{
    auto channel = grpc::CreateChannel(command.at("endpoint"), grpc::InsecureChannelCredentials());
    auto catalog = pub::Catalog::NewStub(channel);
    auto cluster = internal::Cluster::NewStub(channel);
    auto journal = pub::Journal::NewStub(channel);
    auto replay = pub::Replay::NewStub(channel);
    auto archive = internal::Archive::NewStub(channel);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(command.value("timeout_ms", 3000)));
    std::string op = command.at("op");
    Json input = command.value("request", Json::object());
#define RPC(NAME, SERVICE, NS)                                                                                         \
    if(op == #NAME)                                                                                                    \
        return unary<NS::NAME##Request, NS::NAME##Response>(input,                                                     \
                                                            context,                                                   \
                                                            [&](auto* ctx, const auto& request, auto* response)        \
                                                            { return SERVICE->NAME(ctx, request, response); });
    RPC(CreateChronicle, catalog, pub)
    RPC(CreateStory, catalog, pub)
    RPC(Acquire, catalog, pub)
    RPC(RenewAcquisitions, catalog, pub)
    RPC(Release, catalog, pub)
    RPC(GetStory, catalog, pub)
    RPC(DestroyStory, catalog, pub)
    RPC(DestroyChronicle, catalog, pub)
    RPC(Append, journal, pub)
    RPC(Register, cluster, internal)
    RPC(ExtendCeiling, cluster, internal)
    RPC(DrainKeeper, cluster, internal)
    RPC(JoinKeeper, cluster, internal)
    RPC(AbandonKeeper, cluster, internal)
    RPC(ListMembers, cluster, internal)
#undef RPC
    if(op == "Read")
    {
        pub::ReadRequest request;
        auto parsed = google::protobuf::util::JsonStringToMessage(input.dump(), &request);
        if(!parsed.ok())
            throw std::runtime_error(parsed.ToString());
        auto reader = replay->Read(&context, request);
        pub::ReadResponse response;
        Json frames = Json::array();
        while(reader->Read(&response))
        {
            if(frames.size() >= 1024)
                throw std::runtime_error("unbounded Read");
            frames.push_back(encode(response));
        }
        auto status = reader->Finish();
        return {{"transport", status.error_code()}, {"error", status.error_message()}, {"frames", frames}};
    }
    if(op == "FetchHot")
    {
        internal::FetchHotRequest request;
        auto parsed = google::protobuf::util::JsonStringToMessage(input.dump(), &request);
        if(!parsed.ok())
            throw std::runtime_error(parsed.ToString());
        auto reader = archive->FetchHot(&context, request);
        internal::FetchHotResponse response;
        uint64_t events = 0;
        bool trailer = false;
        while(reader->Read(&response))
        {
            events += response.has_batch() ? response.batch().events_size() : 0;
            trailer = trailer || response.has_trailer();
        }
        auto status = reader->Finish();
        return {{"transport", status.error_code()},
                {"error", status.error_message()},
                {"events", events},
                {"trailer", trailer}};
    }
    throw std::runtime_error("unknown RPC " + op);
}

int main()
{
    std::string line;
    while(std::getline(std::cin, line))
    {
        try
        {
            std::cout << call(Json::parse(line)).dump() << std::endl;
        }
        catch(const std::exception& error)
        {
            std::cout << Json({{"exception", error.what()}}).dump() << std::endl;
        }
    }
}
