//
// Created by kfeng on 5/17/22.
//

#include <unistd.h>
#include "client.h"
#include "city.h"
#include <iostream>

int ChronoLogClient::Connect(const std::string &server_uri,
                             std::string &client_id,
			     std::string &group_id,
			     uint32_t &role,
                             int &flags,
                             uint64_t &clock_offset) {
    if (client_id.empty()) 
    {
        char ip[16];
        struct hostent *he = gethostbyname("localhost");
        auto **addr_list = (struct in_addr **) he->h_addr_list;
        strcpy(ip, inet_ntoa(*addr_list[0]));
        std::string addr_str = ip + std::string(",") + std::to_string(getpid());
        uint64_t client_id_hash = CityHash64(addr_str.c_str(), addr_str.size());
        client_id = std::to_string(client_id_hash);
    }
    SetClientId(client_id);
    SetGroupId(group_id);
    SetClientRole(role);
    return rpcProxy_->Connect(server_uri, client_id, group_id,role,flags, clock_offset);
}

int ChronoLogClient::Disconnect(const std::string &client_id, int &flags) {
    return rpcProxy_->Disconnect(client_id, flags);
}

int ChronoLogClient::CreateChronicle(std::string &name,
                                     const std::unordered_map<std::string, std::string> &attrs,
                                     int &flags) 
{
    return rpcProxy_->CreateChronicle(name,client_id_,attrs,flags);
}

int ChronoLogClient::DestroyChronicle(std::string &name, int &flags) {
    return rpcProxy_->DestroyChronicle(name, client_id_,flags);
}

int ChronoLogClient::AcquireChronicle(std::string &name, int &flags) {
    return rpcProxy_->AcquireChronicle(name, client_id_,flags);
}

int ChronoLogClient::ReleaseChronicle(std::string &name, int &flags) {
    return rpcProxy_->ReleaseChronicle(name, client_id_, flags);
}
int ChronoLogClient::UpdateChroniclePermissions(std::string &name,std::string &perm)
{
    return rpcProxy_->UpdateChroniclePermissions(name,client_id_,perm);
}
int ChronoLogClient::CreateStory(std::string &chronicle_name,
                                 std::string &story_name,
                                 const std::unordered_map<std::string, std::string> &attrs,
                                 int &flags) {
    return rpcProxy_->CreateStory(chronicle_name, story_name, client_id_, attrs, flags);
}

int ChronoLogClient::DestroyStory(std::string &chronicle_name, std::string &story_name, int &flags) {
    return rpcProxy_->DestroyStory(chronicle_name, story_name, client_id_,flags);
}

int ChronoLogClient::AcquireStory(std::string &chronicle_name, std::string &story_name, int &flags) {
    return rpcProxy_->AcquireStory(chronicle_name, story_name, client_id_, flags);
}

int ChronoLogClient::ReleaseStory(std::string &chronicle_name, std::string &story_name, int &flags) {
    return rpcProxy_->ReleaseStory(chronicle_name, story_name, client_id_, flags);
}
int ChronoLogClient::UpdateStoryPermissions(std::string &chronicle_name,std::string &story_name,std::string &perm)
{
    return rpcProxy_->UpdateStoryPermissions(chronicle_name,story_name,client_id_,perm);
}
int ChronoLogClient::GetChronicleAttr(std::string &chronicle_name, const std::string &key, std::string &value) {
    return rpcProxy_->GetChronicleAttr(chronicle_name, key, client_id_, value);
}

int ChronoLogClient::EditChronicleAttr(std::string &chronicle_name, const std::string &key, const std::string &value) {
    return rpcProxy_->EditChronicleAttr(chronicle_name, key, client_id_, value);
}
int ChronoLogClient::RequestRoleChange(uint32_t &role)
{
    int ret = CheckClientRole(role);
    if(ret==CL_SUCCESS)
    {
       ret = rpcProxy_->RequestRoleChange(client_id_,role);
       if(ret == CL_SUCCESS)
       {
	   ret = SetClientRole(role);
       }
    }
    return ret;
}
int ChronoLogClient::AddGrouptoChronicle(std::string &chronicle_name,std::string &new_group_id)
{
	return rpcProxy_->AddGrouptoChronicle(chronicle_name,client_id_,new_group_id);
}
int ChronoLogClient::RemoveGroupFromChronicle(std::string &chronicle_name,std::string &new_group_id)
{
	return rpcProxy_->RemoveGroupFromChronicle(chronicle_name,client_id_,new_group_id);
}
int ChronoLogClient::AddGrouptoStory(std::string &chronicle_name,std::string &story_name,std::string &new_group_id)
{
	return rpcProxy_->AddGrouptoStory(chronicle_name,story_name,client_id_,new_group_id);
}
int ChronoLogClient::RemoveGroupFromStory(std::string &chronicle_name,std::string &story_name,std::string &new_group_id)
{
	return rpcProxy_->RemoveGroupFromStory(chronicle_name,story_name,client_id_,new_group_id);
}
int ChronoLogClient::SetClientId(std::string &client_id)
{
    client_id_ = client_id;
    return CL_SUCCESS;
}
std::string& ChronoLogClient::GetClientId()
{
    return client_id_;
}
int ChronoLogClient::CheckClientRole(uint32_t &r)
{
   int ret = CL_ERR_UNKNOWN;
   uint32_t user, group,cluster;
   uint32_t mask = 7;
   user = r & mask;
   mask = mask << 3;
   group = r & mask;
   group = group >> 3;
   mask = mask << 3;
   cluster = r & mask;
   cluster = cluster >> 6;
   if(user >= CHRONOLOG_CLIENT_RO && user <= CHRONOLOG_CLIENT_RWCD &&
      group >= CHRONOLOG_CLIENT_GROUP_ADMIN && group <= CHRONOLOG_CLIENT_GROUP_REG &&
      cluster >= CHRONOLOG_CLIENT_CLUS_ADMIN && cluster <= CHRONOLOG_CLIENT_CLUS_REG)
	  ret = CL_SUCCESS;
  return ret; 
}
int ChronoLogClient::SetClientRole(uint32_t &r)
{
    uint32_t user, group, cluster;
    uint32_t mask = 7;
    user = r & mask;
    mask = mask << 3;
    group = r & mask;
    group = group >> 3;
    mask = mask << 3;
    cluster = r & mask;
    cluster = cluster >> 6;
    CreateClientRole(user,group,cluster);
    return CL_SUCCESS;
}
uint32_t ChronoLogClient::GetClientRole()
{
    return my_role_;
}
int ChronoLogClient::CreateClientRole(uint32_t &user, uint32_t &group, uint32_t &cluster)
{
	if(user < CHRONOLOG_CLIENT_RO || user >= CHRONOLOG_CLIENT_RWCD) user = CHRONOLOG_CLIENT_RO;
	if(group != CHRONOLOG_CLIENT_GROUP_ADMIN && group != CHRONOLOG_CLIENT_GROUP_REG) group = CHRONOLOG_CLIENT_GROUP_REG;
	if(cluster != CHRONOLOG_CLIENT_CLUS_ADMIN && cluster != CHRONOLOG_CLIENT_CLUS_REG) cluster = CHRONOLOG_CLIENT_CLUS_REG;
	my_role_ = 0;
	my_role_ = my_role_ | (cluster << 6);
	my_role_ = my_role_ | (group << 3);
	my_role_ = my_role_ | (user);
	return CL_SUCCESS;
}
uint32_t ChronoLogClient::UserRole()
{
	uint32_t mask = 7;
	return my_role_ & mask;
}
uint32_t ChronoLogClient::GroupRole()
{
	uint32_t mask = 7;
	mask = mask << 3;
	return my_role_ & mask;
}
uint32_t ChronoLogClient::ClusterRole()
{
	uint32_t mask = 7;
	mask = mask << 6;
	return my_role_ & mask;
}
int ChronoLogClient::SetGroupId(std::string &group_id)
{
     group_id_ = group_id;
     return CL_SUCCESS;
}
std::string& ChronoLogClient::GetGroupId()
{
	return group_id_;
}
