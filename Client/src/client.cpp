//
// Created by kfeng on 5/17/22.
//

#include <unistd.h>
#include "client.h"
#include "city.h"

int ChronoLogClient::Connect(const std::string &server_uri,
                             std::string &client_id,
                             int &flags,
                             uint64_t &clock_offset) {
    if (client_id.empty()) {
        char ip[16];
        struct hostent *he = gethostbyname("localhost");
        auto **addr_list = (struct in_addr **) he->h_addr_list;
        strcpy(ip, inet_ntoa(*addr_list[0]));
        std::string addr_str = ip + std::string(",") + std::to_string(getpid());
        uint64_t client_id_hash = CityHash64(addr_str.c_str(), addr_str.size());
        client_id = std::to_string(client_id_hash);
    }
    clientid = client_id;
    CSManager->set_myid(clientid);
    return rpcClient_->Connect(server_uri, client_id, flags, clock_offset);
}

int ChronoLogClient::Disconnect(const std::string &client_id, int &flags) {
    return rpcClient_->Disconnect(client_id, flags);
}

int ChronoLogClient::CreateChronicle(std::string &name,
                                     const std::unordered_map<std::string, std::string> &attrs,
                                     int &flags) {
    return rpcClient_->CreateChronicle(name, attrs, flags);
}

int ChronoLogClient::DestroyChronicle(std::string &name, int &flags) {
    return rpcClient_->DestroyChronicle(name, flags);
}

int ChronoLogClient::DestroyStory(std::string &chronicle_name, std::string &story_name, int &flags) {
    return rpcClient_->DestroyStory(chronicle_name, story_name, flags);
}

int ChronoLogClient::AcquireStory(std::string &chronicle_name, std::string &story_name,
                                  const std::unordered_map<std::string, std::string> &attrs, int &flags) {
    return rpcClient_->AcquireStory(clientid, chronicle_name, story_name, attrs, flags);
}

int ChronoLogClient::ReleaseStory(std::string &chronicle_name, std::string &story_name, int &flags) {
    return rpcClient_->ReleaseStory(clientid, chronicle_name, story_name, flags);
}

int ChronoLogClient::GetChronicleAttr(std::string &chronicle_name, const std::string &key, std::string &value) {
    return rpcClient_->GetChronicleAttr(chronicle_name, key, value);
}

int ChronoLogClient::EditChronicleAttr(std::string &chronicle_name, const std::string &key, const std::string &value) {
    return rpcClient_->EditChronicleAttr(chronicle_name, key, value);
}

std::vector<std::string> ChronoLogClient::ShowChronicles(std::string &client_id) {
    return rpcClient_->ShowChronicles(client_id);
}

std::vector<std::string> ChronoLogClient::ShowStories(std::string &client_id, const std::string &chronicle_name) {
    return rpcClient_->ShowStories(client_id, chronicle_name);
}
uint64_t ChronoLogClient::GetTS()
{
	return rpcProxy_->GetTS(clientid);
}
uint64_t ChronoLogClient::LocalTS()
{
	return CSManager->LocalGetTS(clientid);
}

void ChronoLogClient::ComputeClockOffset()
{
	uint64_t t1 = LocalTS();
	uint64_t vt1 = GetTS();

	uint64_t t2 = LocalTS();
	uint64_t vt2 = GetTS();

	int offset1 = vt1-t1;
        int offset2 = vt2-t2;

	int mean_offset = (offset1+offset2)/2;

	uint64_t mean_offset_u;
	bool pos_neg = false;
	if(mean_offset < 0) 
	{
	   mean_offset_u = (uint64_t)(-1*mean_offset);
	   pos_neg = true;
	}
	else mean_offset_u = mean_offset;

	CSManager->set_offset(mean_offset_u,pos_neg);
}

int ChronoLogClient::StoreError()
{
    uint64_t t = CSManager->get_offset();
    return rpcProxy_->StoreError(clientid,t);
}
uint64_t ChronoLogClient::GetMaxError()
{
   uint64_t t = UINT64_MAX;
   
   do
   {
       t = rpcProxy_->GetMaxError(clientid);
       if(t != UINT64_MAX) break;
       usleep(50);
   }while(t == UINT64_MAX);

   return t;
}
