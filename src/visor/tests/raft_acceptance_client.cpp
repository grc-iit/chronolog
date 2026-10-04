#include <grpcpp/grpcpp.h>
#include <chrono>
#include <csignal>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <array>
#include <random>
#include "chronolog/v1/chronolog.grpc.pb.h"
using namespace std::chrono_literals;
namespace v1 = chronolog::v1;
template <class Fn>
bool retry(Fn fn)
{
    auto until = std::chrono::steady_clock::now() + 12s;
    while(std::chrono::steady_clock::now() < until)
    {
        if(fn())
            return true;
        std::this_thread::sleep_for(50ms);
    }
    return false;
}
int main(int argc, char** argv)
{
    if(argc != 8)
        return 2;
    std::array<std::unique_ptr<v1::Catalog::Stub>, 3> stubs;
    std::array<int, 3> pids;
    for(size_t i = 0; i < 3; ++i)
    {
        stubs[i] = v1::Catalog::NewStub(grpc::CreateChannel(argv[1 + i], grpc::InsecureChannelCredentials()));
        pids[i] = std::stoi(argv[4 + i]);
    }
    v1::CreateChronicleRequest c;
    c.set_name("failover");
    if(!retry(
               [&]
               {
                   grpc::ClientContext ctx;
                   ctx.set_deadline(std::chrono::system_clock::now() + 1s);
                   v1::CreateChronicleResponse r;
                   auto s = stubs[0]->CreateChronicle(&ctx, c, &r);
                   return s.ok() && (r.status().code() == 0 || r.status().code() == 6);
               }))
        return 3;
    v1::CreateStoryRequest request;
    request.set_chronicle("failover");
    request.set_name("story");
    uint64_t story = 0;
    if(!retry(
               [&]
               {
                   grpc::ClientContext ctx;
                   ctx.set_deadline(std::chrono::system_clock::now() + 1s);
                   v1::CreateStoryResponse r;
                   auto s = stubs[0]->CreateStory(&ctx, request, &r);
                   if(s.ok() && r.status().code() == 0)
                   {
                       story = r.story().story_id();
                       return true;
                   }
                   if(s.ok() && r.status().code() == 6)
                   {
                       grpc::ClientContext list_ctx;
                       list_ctx.set_deadline(std::chrono::system_clock::now() + 1s);
                       v1::ListStoriesRequest q;
                       q.set_chronicle("failover");
                       v1::ListStoriesResponse list;
                       if(stubs[0]->ListStories(&list_ctx, q, &list).ok() && list.stories_size() == 1)
                       {
                           story = list.stories(0).story_id();
                           return true;
                       }
                   }
                   return false;
               }))
        return 4;
    uint64_t incarnation = 0, revision = 0, writer = 0;
    int leader = -1;
    size_t target = 0;
    for(int round = 0; round < 40; ++round)
    {
        if(round == 1)
        {
            if(leader < 1 || leader > 3)
                return 5;
            target = static_cast<size_t>(leader % 3);
        }
        if(round == 20)
        {
            if(kill(pids[static_cast<size_t>(leader - 1)], SIGKILL))
                return 6;
        }
        if(round == 30)
        {
            for(auto pid: pids) kill(pid, SIGKILL);
            std::ofstream trigger(std::string(argv[7]) + "/restart");
            trigger << "restart\n";
            trigger.close();
            if(!retry([&] { return std::filesystem::exists(std::string(argv[7]) + "/restarted"); }))
                return 11;
        }
        // One process-local id per logical round, frozen before the retry loop, so a reply lost to a leader
        // kill or the all-node restart returns the committed grant instead of a HELD refusal.
        std::random_device random;
        v1::AcquireRequest q;
        q.set_story_id(story);
        q.set_writer_identity("writer");
        q.set_acquire_request_id("round-" + std::to_string(round) + "-" + std::to_string(random()) + "-" +
                                 std::to_string(random()) + "-" + std::to_string(random()) + "-" +
                                 std::to_string(random()));
        v1::AcquireResponse acquired;
        if(!retry(
                   [&]
                   {
                       grpc::ClientContext ctx;
                       ctx.set_deadline(std::chrono::system_clock::now() + 1s);
                       auto s = stubs[target]->Acquire(&ctx, q, &acquired);
                       if(!s.ok() || acquired.status().code() != 0)
                           return false;
                       if(round == 0)
                       {
                           auto metadata = ctx.GetServerInitialMetadata();
                           auto it = metadata.find("chronolog-raft-leader");
                           if(it != metadata.end())
                               leader = std::stoi(std::string(it->second.data(), it->second.length()));
                       }
                       return true;
                   }))
            return 7;
        if(acquired.incarnation() <= incarnation || (writer && acquired.writer_id() != writer))
            return 8;
        incarnation = acquired.incarnation();
        writer = acquired.writer_id();
        v1::ReleaseRequest release;
        release.set_story_id(story);
        release.set_writer_id(writer);
        release.set_incarnation(incarnation);
        v1::ReleaseResponse released;
        if(!retry(
                   [&]
                   {
                       grpc::ClientContext ctx;
                       ctx.set_deadline(std::chrono::system_clock::now() + 1s);
                       return stubs[target]->Release(&ctx, release, &released).ok() && released.status().code() == 0;
                   }))
            return 9;
        if(released.revision() <= revision)
            return 10;
        revision = released.revision();
    }
    std::cout
            << "40 follower acquires preserved writer, incarnation and revision through leader and majority SIGKILL\n";
    return 0;
}
