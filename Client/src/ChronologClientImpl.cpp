
#include <unistd.h>
#include "ChronologClientImpl.h"
#include "city.h"

int chronolog::ChronologClientImpl::Connect(const std::string &server_uri,
                             std::string const& client_account,
                             int &flags)
{
/*    if (client_id.empty()) {
        char ip[16];
        struct hostent *he = gethostbyname("localhost");
        auto **addr_list = (struct in_addr **) he->h_addr_list;
        strcpy(ip, inet_ntoa(*addr_list[0]));
        std::string addr_str = ip + std::string(",") + std::to_string(getpid());
        uint64_t client_id_hash = CityHash64(addr_str.c_str(), addr_str.size());
        client_id = std::to_string(client_id_hash);
    }
    */
    clientAccount = client_account;
       //TODO: client_id must be assigned by the Visor server
    uint64_t clock_offset = 0; //TODO: use this value with the clocksource...
			       //
    return rpcClient_->Connect(server_uri, clientAccount, flags, clock_offset);
}

int chronolog::ChronologClientImpl::Disconnect( ) //const std::string &client_id, int &flags)
{
  //TODO : release all the acquired stories before asking to disconnect...
    int flags=1;
    return CL_SUCCESS; //rpcClient_->Disconnect(clientAccount , flags);
}
//TODO: client account must be passed in
int chronolog::ChronologClientImpl::CreateChronicle(std::string const& chronicle_name,
                                     const std::unordered_map<std::string, std::string> &attrs,
                                     int &flags) 
{
    return CL_SUCCESS;//rpcClient_->CreateChronicle(chronicle_name, attrs, flags);
}

//TODO: client account must be passed in
int chronolog::ChronologClientImpl::DestroyChronicle(std::string const& chronicle_name)//, int &flags) 
{
    int flags=1;	 
    return CL_SUCCESS; //rpcClient_->DestroyChronicle(chronicle_name, flags);
}

int chronolog::ChronologClientImpl::DestroyStory(std::string const& chronicle_name, std::string const& story_name)//, int &flags)
{
    int flags=1;	 
    return CL_SUCCESS;//rpcClient_->DestroyStory(chronicle_name, story_name);
}

chronolog::StoryHandle * chronolog::ChronologClientImpl::AcquireStory(std::string const& chronicle_name, std::string const& story_name,
                                  const std::unordered_map<std::string, std::string> &attrs, int &flags) 
{
    chronolog::StoryHandle * storyHandle = nullptr;
 //TODO:   return rpcClient_->AcquireStory(clientAccount, chronicle_name, story_name, attrs, flags);
 return storyHandle;
}
// TODO: remove flags ...
int chronolog::ChronologClientImpl::ReleaseStory(std::string const& chronicle_name, std::string const& story_name)
	//, int &flags) 
{
    int flags=1;	 
    return CL_SUCCESS; //rpcClient_->ReleaseStory(clientAccount, chronicle_name, story_name, flags);
}

//TODO: client account must be passed in
int chronolog::ChronologClientImpl::GetChronicleAttr(std::string const& chronicle_name, const std::string &key, std::string &value) {
    return CL_SUCCESS; //rpcClient_->GetChronicleAttr(chronicle_name, key, value);
}
//TODO: client account must be passed in
int chronolog::ChronologClientImpl::EditChronicleAttr(std::string const& chronicle_name, const std::string &key, std::string const& value) {
    return CL_SUCCESS; //rpcClient_->EditChronicleAttr(chronicle_name, key, value);
}

std::vector<std::string> & chronolog::ChronologClientImpl::ShowChronicles( std::vector<std::string> & chronicles )//std::string &client_id) 
{
  //TODO:   return rpcClient_->ShowChronicles(clientAccount);
  return chronicles;
}

std::vector<std::string> & chronolog::ChronologClientImpl::ShowStories( const std::string &chronicle_name, std::vector<std::string> & stories) 
{
   //TODO :  return rpcClient_->ShowStories(clientAccount, chronicles);
   return stories;
}
