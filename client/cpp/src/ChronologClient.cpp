#include "ChronologClientImpl.h"

chronolog::Client::Client(chronolog::ClientPortalServiceConf const& visorClientPortalServiceConf)
{
    chronologClientImpl =
            chronolog::ChronologClientImpl::GetClientImplInstance(visorClientPortalServiceConf,
                                                                  chronolog::WRITER_MODE,
                                                                  chronolog::ClientQueryServiceConf{"", "", 0, 0});
}

chronolog::Client::Client(chronolog::ClientPortalServiceConf const& visorClientPortalServiceConf,
                          chronolog::ClientQueryServiceConf const& clientQueryServiceConf)
{
    chronologClientImpl = chronolog::ChronologClientImpl::GetClientImplInstance(visorClientPortalServiceConf,
                                                                                chronolog::READER_MODE,
                                                                                clientQueryServiceConf);
}

chronolog::Client::~Client() = default;

int chronolog::Client::SetRpcTimeoutPolicy(RpcTimeoutPolicy const& policy)
{
    return chronologClientImpl->SetRpcTimeoutPolicy(policy);
}

int chronolog::Client::Connect() { return chronologClientImpl->Connect(chronolog::rpc::RpcDeadline::Clock::now()); }

int chronolog::Client::Disconnect()
{
    return chronologClientImpl->Disconnect(chronolog::rpc::RpcDeadline::Clock::now());
}

chronolog::ClientId chronolog::Client::client_id() const { return chronologClientImpl->client_id(); }

int chronolog::Client::CreateChronicle(std::string const& chronicle_name)
{
    return chronologClientImpl->CreateChronicle(chronicle_name, chronolog::rpc::RpcDeadline::Clock::now());
}

int chronolog::Client::DestroyChronicle(std::string const& chronicle_name)
{
    return chronologClientImpl->DestroyChronicle(chronicle_name, chronolog::rpc::RpcDeadline::Clock::now());
}

std::pair<int, chronolog::StoryHandle*> chronolog::Client::AcquireStory(std::string const& chronicle_name,
                                                                        std::string const& story_name)
{
    return chronologClientImpl->AcquireStory(chronicle_name, story_name, chronolog::rpc::RpcDeadline::Clock::now());
}

int chronolog::Client::ReleaseStory(std::string const& chronicle_name, std::string const& story_name)
{
    return chronologClientImpl->ReleaseStory(chronicle_name, story_name, chronolog::rpc::RpcDeadline::Clock::now());
}

int chronolog::Client::DestroyStory(std::string const& chronicle_name, std::string const& story_name)
{
    return chronologClientImpl->DestroyStory(chronicle_name, story_name, chronolog::rpc::RpcDeadline::Clock::now());
}

std::pair<int, std::vector<std::string>> chronolog::Client::ShowChronicles()
{
    return chronologClientImpl->ShowChronicles(chronolog::rpc::RpcDeadline::Clock::now());
}

std::pair<int, std::vector<std::string>> chronolog::Client::ShowStories(std::string const& chronicle_name)
{
    return chronologClientImpl->ShowStories(chronicle_name, chronolog::rpc::RpcDeadline::Clock::now());
}


int chronolog::Client::ReplayStory(std::string const& chronicle_name,
                                   std::string const& story_name,
                                   uint64_t start_time,
                                   uint64_t end_time,
                                   std::vector<chronolog::Event>& event_series)
{
    auto started = chronolog::rpc::RpcDeadline::Clock::now();
    return chronologClientImpl->replay_story(chronicle_name, story_name, start_time, end_time, event_series, started);
}

int chronolog::Client::ReplayStory(std::string const& chronicle_name,
                                   std::string const& story_name,
                                   uint64_t start_time,
                                   uint64_t end_time,
                                   chronolog::Client::EventCallback callback)
{
    auto started = chronolog::rpc::RpcDeadline::Clock::now();
    return chronologClientImpl
            ->replay_story(chronicle_name, story_name, start_time, end_time, std::move(callback), started);
}
