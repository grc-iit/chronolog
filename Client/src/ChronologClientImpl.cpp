
#include <unistd.h>
#include "ChronologClientImpl.h"
#include "StorytellerClient.h"
#include "city.h"


chronolog::ChronologClientImpl::~ChronologClientImpl()
{
	//TODO: implement
	delete storyteller;
// shared_ptr..	delete rpcClient_;
}

int chronolog::ChronologClientImpl::Connect(const std::string &server_uri,
                             std::string const& client_account,
                             int &flags)
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

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
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);
  //TODO : release all the acquired stories before asking to disconnect...
    int flags=1;
    return rpcClient_->Disconnect(clientAccount , flags);
}
//TODO: client account must be passed into the rpc call
int chronolog::ChronologClientImpl::CreateChronicle(std::string const& chronicle_name,
                                     const std::unordered_map<std::string, std::string> &attrs,
                                     int &flags) 
{  
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);
    std::string chronicle(chronicle_name);
    return rpcClient_->CreateChronicle(chronicle, attrs, flags);
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::DestroyChronicle(std::string const& chronicle_name)//, int &flags) 
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);
    int flags=1;	 
    std::string chronicle(chronicle_name);
    return rpcClient_->DestroyChronicle(chronicle, flags);
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::DestroyStory(std::string const& chronicle_name, std::string const& story_name)
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);
    ChronicleName chronicle(chronicle_name);
    StoryName story(story_name);
    int flags=1;	 
    return rpcClient_->DestroyStory(chronicle, story, flags);
}

std::pair<int,chronolog::StoryHandle*> chronolog::ChronologClientImpl::AcquireStory(std::string const& chronicle_name, std::string const& story_name,
                                  const std::unordered_map<std::string, std::string> &attrs, int &flags) 
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);
    chronolog::StoryHandle * storyHandle = nullptr;
    //TODO: this function should return StoryId & vector<KeeperIdCard>  that will be used to create StoryWritingHandle

    ChronicleName chronicle(chronicle_name);
    StoryName story(story_name);
    int ret = rpcClient_->AcquireStory(clientAccount, chronicle, story, attrs, flags);
    if(ret == CL_SUCCESS)
    {
    	// storyHandle = storytellerClient->initializeStoryWritingHandle(...)
    }

 return std::pair<int,StoryHandle*>(ret,storyHandle);
}

// TODO: remove flags ...
int chronolog::ChronologClientImpl::ReleaseStory(std::string const& chronicle_name, std::string const& story_name)
	//, int &flags) 
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);
    int flags=1;	 
    ChronicleName chronicle(chronicle_name);
    StoryName story(story_name);
    auto ret = rpcClient_->ReleaseStory(clientAccount, chronicle, story, flags);
    if (ret == CL_SUCCESS)
    {
    	//storytellerClient->removeAcquireStoryHandle(...)
    }
    return ret;
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::GetChronicleAttr(std::string const& chronicle_name, const std::string &key, std::string &value) {
    ChronicleName chronicle(chronicle_name);
    return rpcClient_->GetChronicleAttr(chronicle, key, value);
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::EditChronicleAttr(std::string const& chronicle_name, const std::string &key, std::string const& value) {
    ChronicleName chronicle(chronicle_name);
    return rpcClient_->EditChronicleAttr(chronicle, key, value);
}

std::vector<std::string> & chronolog::ChronologClientImpl::ShowChronicles( std::vector<std::string> & chronicles ) 
{
  //TODO:   return rpcClient_->ShowChronicles(clientAccount);
  return chronicles;
}

std::vector<std::string> & chronolog::ChronologClientImpl::ShowStories( const std::string &chronicle_name, std::vector<std::string> & stories) 
{
   //TODO :  return rpcClient_->ShowStories(clientAccount, chronicle_name);
   return stories;
}
