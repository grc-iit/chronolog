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
    return rpcProxy_->Connect(server_uri, client_id, flags, clock_offset);
}

int ChronoLogClient::Disconnect(const std::string &client_id, int &flags) {
    return rpcProxy_->Disconnect(client_id, flags);
}

int ChronoLogClient::CreateChronicle(std::string &name,
                                     const std::unordered_map<std::string, std::string> &attrs,
                                     int &flags) {
    return rpcProxy_->CreateChronicle(name, attrs, flags);
}

int ChronoLogClient::DestroyChronicle(std::string &name, int &flags) {
    return rpcProxy_->DestroyChronicle(name, flags);
}

int ChronoLogClient::AcquireChronicle(std::string &name, int &flags) {
    return rpcProxy_->AcquireChronicle(clientid, name, flags);
}

int ChronoLogClient::ReleaseChronicle(std::string &name, int &flags) {
    return rpcProxy_->ReleaseChronicle(clientid, name, flags);
}

int ChronoLogClient::CreateStory(std::string &chronicle_name,
                                 std::string &story_name,
                                 const std::unordered_map<std::string, std::string> &attrs,
                                 int &flags) {
    return rpcProxy_->CreateStory(chronicle_name, story_name, attrs, flags);
}

int ChronoLogClient::DestroyStory(std::string &chronicle_name, std::string &story_name, int &flags) {
    return rpcProxy_->DestroyStory(chronicle_name, story_name, flags);
}

int ChronoLogClient::AcquireStory(std::string &chronicle_name, std::string &story_name, int &flags) {
    return rpcProxy_->AcquireStory(chronicle_name, story_name, flags);
}

int ChronoLogClient::ReleaseStory(std::string &chronicle_name, std::string &story_name, int &flags) {
    return rpcProxy_->ReleaseStory(chronicle_name, story_name, flags);
}

int ChronoLogClient::GetChronicleAttr(std::string &chronicle_name, const std::string &key, std::string &value) {
    return rpcProxy_->GetChronicleAttr(chronicle_name, key, value);
}

int ChronoLogClient::EditChronicleAttr(std::string &chronicle_name, const std::string &key, const std::string &value) {
    return rpcProxy_->EditChronicleAttr(chronicle_name, key, value);
}
