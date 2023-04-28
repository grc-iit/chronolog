
#include "ChronologClientImpl.h"

chronolog::Client::Client( ChronoLog::ConfigurationManager const& confManager) 
{
	//TODO:  the pointer must be protected with the static mutex ...
   chronologClientImpl = new chronolog::ChronologClientImpl( confManager);  
}
 
chronolog::Client::~Client()
{
   delete chronologClientImpl;
}

//TODO: get the client_account & client ip from the process itself ....
int chronolog::Client::Connect(std::string const& server_uri,
                std::string const& client_account,
                int &flags)
{	
    return chronologClientImpl->Connect(server_uri, client_account, flags);  
}

int chronolog::Client::Disconnect()
{   
    return chronologClientImpl->Disconnect(); 
}

int chronolog::Client::CreateChronicle( std::string const& chronicle_name,
                         std::unordered_map<std::string, std::string> const& attrs,
                         int &flags)
{
    return chronologClientImpl->CreateChronicle( chronicle_name, attrs, flags);
}
    
int chronolog::Client::DestroyChronicle(std::string const& chronicle_name)
{
    return chronologClientImpl->DestroyChronicle(chronicle_name);
}

std::pair<int,chronolog::StoryHandle *> chronolog::Client::AcquireStory(std::string const& chronicle_name, std::string const& story_name,
                     const std::unordered_map<std::string, std::string> &attrs, int &flags)
{
    return chronologClientImpl->AcquireStory(chronicle_name, story_name, attrs,flags);
}

int chronolog::Client::ReleaseStory(std::string const& chronicle_name, std::string const& story_name)
{
    return chronologClientImpl->ReleaseStory( chronicle_name, story_name);
}
    
int chronolog::Client::DestroyStory(std::string const& chronicle_name, std::string const& story_name)
{
    return chronologClientImpl->DestroyStory(chronicle_name,story_name);
}

int chronolog::Client::GetChronicleAttr(std::string const& chronicle_name, const std::string &key, std::string &value)
{
    return chronologClientImpl->GetChronicleAttr( chronicle_name, key, value);
}

int chronolog::Client::EditChronicleAttr(std::string const& chronicle_name, const std::string &key, const std::string &value)
{
    return chronologClientImpl->EditChronicleAttr(chronicle_name, key, value);
}

std::vector<std::string> & chronolog::Client::ShowChronicles( std::vector<std::string> & chronicles)
{
    return chronologClientImpl->ShowChronicles(chronicles);
}

std::vector<std::string> & chronolog::Client::ShowStories(std::string const& chronicle_name, std::vector<std::string> & stories )
{
    return chronologClientImpl->ShowStories( chronicle_name, stories);
}


