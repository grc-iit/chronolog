
#include <unistd.h>
#include "ChronologClientImpl.h"
#include "StorytellerClient.h"
#include "city.h"

std::mutex chronolog::ChronologClientImpl::chronologClientMutex;
chronolog::ChronologClientImpl * chronolog::ChronologClientImpl::chronologClientImplInstance{nullptr};


chronolog::ChronologClientImpl * chronolog::ChronologClientImpl::GetClientImplInstance(ChronoLog::ConfigurationManager const& confManager)
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);
    if( chronologClientImplInstance == nullptr)
    {
        chronologClientImplInstance = new ChronologClientImpl(confManager);
    }

    return chronologClientImplInstance;
}

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
    // if already connected return success
    // if disconencting return failure....
    if((clientState != UNKNOWN) && (clientState != SHUTTING_DOWN))
    {  return CL_SUCCESS;  }  
    else if (clientState == SHUTTING_DOWN) 
    {  return CL_ERR_INVALID_ARG; }

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

    auto return_code = rpcClient_->Connect(server_uri, clientAccount, flags, clock_offset);

    if(return_code == CL_SUCCESS)
    {    
	clientState = CONNECTED; 
        clientId = 7; //TODO: retain clientId provided by he Visor 
    }
    return return_code;
}

int chronolog::ChronologClientImpl::Disconnect( ) //const std::string &client_id, int &flags)
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {  return CL_ERR_NO_CONNECTION; }

  //TODO : release all the acquired stories before asking to disconnect...

    int flags=1;
    auto return_code = rpcClient_->Disconnect(clientAccount , flags);
    if(return_code == CL_SUCCESS)
    {    
	clientState = SHUTTING_DOWN; 
    }
    return return_code;

}
//TODO: client account must be passed into the rpc call
int chronolog::ChronologClientImpl::CreateChronicle(std::string const& chronicle_name,
                                     const std::unordered_map<std::string, std::string> &attrs,
                                     int &flags) 
{  
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {  return CL_ERR_NO_CONNECTION; }

    return rpcClient_->CreateChronicle(chronicle_name, attrs, flags);
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::DestroyChronicle(std::string const& chronicle_name) 
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {  return CL_ERR_NO_CONNECTION; }

    return rpcClient_->DestroyChronicle(chronicle_name);
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::DestroyStory(std::string const& chronicle_name, std::string const& story_name)
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {  return CL_ERR_NO_CONNECTION; }

    return rpcClient_->DestroyStory(chronicle_name, story_name);
}

std::pair<int,chronolog::StoryHandle*> chronolog::ChronologClientImpl::AcquireStory(std::string const& chronicle_name, std::string const& story_name,
                                  const std::unordered_map<std::string, std::string> &attrs, int &flags) 
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {  return std::pair<int, chronolog::StoryHandle*>(CL_ERR_NO_CONNECTION,nullptr); }

    chronolog::StoryHandle * storyHandle = nullptr;
    //TODO: this function should return StoryId & vector<KeeperIdCard>  that will be used to create StoryWritingHandle

    int ret = rpcClient_->AcquireStory(clientAccount, chronicle_name, story_name, attrs, flags);
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

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {  return CL_ERR_NO_CONNECTION; }

    auto ret = rpcClient_->ReleaseStory(clientAccount, chronicle_name, story_name);
    if (ret == CL_SUCCESS)
    {
    	//storytellerClient->removeAcquireStoryHandle(...)
    }
    return ret;
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::GetChronicleAttr(std::string const& chronicle_name, const std::string &key, std::string &value) {
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {  return CL_ERR_NO_CONNECTION; }

    return rpcClient_->GetChronicleAttr(chronicle_name, key, value);
}

//TODO: client account must be passed into the rpc call 
int chronolog::ChronologClientImpl::EditChronicleAttr(std::string const& chronicle_name, const std::string &key, std::string const& value) {
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {  return CL_ERR_NO_CONNECTION; }

    return rpcClient_->EditChronicleAttr(chronicle_name, key, value);
}

std::vector<std::string> & chronolog::ChronologClientImpl::ShowChronicles( std::vector<std::string> & chronicles ) 
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {  return chronicles; }

    chronicles = rpcClient_->ShowChronicles(clientAccount);
  return chronicles;
}

std::vector<std::string> & chronolog::ChronologClientImpl::ShowStories( std::string const& chronicle_name, std::vector<std::string> & stories) 
{
    std::lock_guard<std::mutex> lock_client(chronologClientMutex);

    if((clientState == UNKNOWN) || (clientState == SHUTTING_DOWN))
    {  return stories; }

    stories = rpcClient_->ShowStories(clientAccount, chronicle_name);
   return stories;
}
