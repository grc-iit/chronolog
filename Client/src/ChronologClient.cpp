#include "ChronologClientImpl.h"



chronolog::StoryHandle::StoryHandle()
 :storyWritingHandle(nullptr)
{ }

chronolog::StoryHandle::StoryHandle(const std::shared_ptr<StoryWritingHandle>& impl)
    :storyWritingHandle(impl)
{

}

bool chronolog::StoryHandle::is_valid() const
{
    return static_cast<bool>(storyWritingHandle);    
}

int chronolog::StoryHandle::log_event(std::string const & event_string)
{
    if( is_valid())
    {
        return storyWritingHandle->log_event(event_string);
    }
    else
    {
        return CL_ERR_UNKNOWN;
    }
}

chronolog::Client::Client(ChronoLog::ConfigurationManager const &confManager)
{
    chronologClientImpl = chronolog::ChronologClientImpl::GetClientImplInstance(confManager);
}

chronolog::Client::Client(chronolog::ClientPortalServiceConf const &visorClientPortalServiceConf)
{
    chronologClientImpl = chronolog::ChronologClientImpl::GetClientImplInstance(visorClientPortalServiceConf);
}

chronolog::Client::~Client()
{
    delete chronologClientImpl;
}

int chronolog::Client::Connect()
{
    return chronologClientImpl->Connect();
}

int chronolog::Client::Disconnect()
{
    return chronologClientImpl->Disconnect();
}

int chronolog::Client::CreateChronicle(std::string const &chronicle_name
                                       , std::map <std::string, std::string> const &attrs, int &flags)
{
    return chronologClientImpl->CreateChronicle(chronicle_name, attrs, flags);
}

int chronolog::Client::DestroyChronicle(std::string const &chronicle_name)
{
    return chronologClientImpl->DestroyChronicle(chronicle_name);
}

std::pair <int, chronolog::StoryHandle>
chronolog::Client::AcquireStory(std::string const &chronicle_name, std::string const &story_name
                                , const std::map <std::string, std::string> &attrs, int &flags)
{
    return chronologClientImpl->AcquireStory(chronicle_name, story_name, attrs, flags);
}

int chronolog::Client::ReleaseStory(std::string const &chronicle_name, std::string const &story_name)
{
    return chronologClientImpl->ReleaseStory(chronicle_name, story_name);
}

int chronolog::Client::DestroyStory(std::string const &chronicle_name, std::string const &story_name)
{
    return chronologClientImpl->DestroyStory(chronicle_name, story_name);
}

int chronolog::Client::GetChronicleAttr(std::string const &chronicle_name, const std::string &key, std::string &value)
{
    return chronologClientImpl->GetChronicleAttr(chronicle_name, key, value);
}

int chronolog::Client::EditChronicleAttr(std::string const &chronicle_name, const std::string &key
                                         , const std::string &value)
{
    return chronologClientImpl->EditChronicleAttr(chronicle_name, key, value);
}

std::vector <std::string> &chronolog::Client::ShowChronicles(std::vector <std::string> &chronicles)
{
    return chronologClientImpl->ShowChronicles(chronicles);
}

std::vector <std::string> &
chronolog::Client::ShowStories(std::string const &chronicle_name, std::vector <std::string> &stories)
{
    return chronologClientImpl->ShowStories(chronicle_name, stories);
}


