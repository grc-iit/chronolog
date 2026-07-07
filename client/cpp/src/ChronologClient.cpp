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

// chronologClientImpl is a process-wide singleton shared by every Client, so it
// must not be `delete`d per-Client (that double-frees it and leaves a dangling
// instance for the next Client — the use-after-free in issue #692). Drop this
// Client's reference instead; the impl is destroyed when the last one is released.
chronolog::Client::~Client() { chronolog::ChronologClientImpl::ReleaseClientImplInstance(); }

int chronolog::Client::Connect() { return chronologClientImpl->Connect(); }

int chronolog::Client::Disconnect() { return chronologClientImpl->Disconnect(); }

chronolog::ClientId chronolog::Client::client_id() const { return chronologClientImpl->client_id(); }

int chronolog::Client::CreateChronicle(std::string const& chronicle_name)
{
    return chronologClientImpl->CreateChronicle(chronicle_name);
}

int chronolog::Client::DestroyChronicle(std::string const& chronicle_name)
{
    return chronologClientImpl->DestroyChronicle(chronicle_name);
}

std::pair<int, chronolog::StoryHandle*> chronolog::Client::AcquireStory(std::string const& chronicle_name,
                                                                        std::string const& story_name)
{
    return chronologClientImpl->AcquireStory(chronicle_name, story_name);
}

int chronolog::Client::ReleaseStory(std::string const& chronicle_name, std::string const& story_name)
{
    return chronologClientImpl->ReleaseStory(chronicle_name, story_name);
}

int chronolog::Client::DestroyStory(std::string const& chronicle_name, std::string const& story_name)
{
    return chronologClientImpl->DestroyStory(chronicle_name, story_name);
}

std::pair<int, std::vector<std::string>> chronolog::Client::ShowChronicles()
{
    return chronologClientImpl->ShowChronicles();
}

std::pair<int, std::vector<std::string>> chronolog::Client::ShowStories(std::string const& chronicle_name)
{
    return chronologClientImpl->ShowStories(chronicle_name);
}


int chronolog::Client::ReplayStory(std::string const& chronicle_name,
                                   std::string const& story_name,
                                   uint64_t start_time,
                                   uint64_t end_time,
                                   std::vector<chronolog::Event>& event_series)
{
    return chronologClientImpl->replay_story(chronicle_name, story_name, start_time, end_time, event_series);
}

int chronolog::Client::ReplayStory(std::string const& chronicle_name,
                                   std::string const& story_name,
                                   uint64_t start_time,
                                   uint64_t end_time,
                                   chronolog::Client::EventCallback callback)
{
    return chronologClientImpl->replay_story(chronicle_name, story_name, start_time, end_time, std::move(callback));
}
