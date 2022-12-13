//
// Created by kfeng on 5/17/22.
//

#include <sys/types.h>
#include <unistd.h>
#include "client.h"
#include "city.h"

bool ChronoLogClient::Connect(const std::string &server_uri, std::string &client_id) {
    if (client_id.empty()) {
        char ip[16];
        struct hostent *he = gethostbyname("localhost");
        in_addr **addr_list = (struct in_addr **) he->h_addr_list;
        strcpy(ip, inet_ntoa(*addr_list[0]));
        std::string addr_str = ip + std::string(",") + std::to_string(getpid());
        uint64_t client_id_hash = CityHash64(addr_str.c_str(), addr_str.size());
        client_id = std::to_string(client_id_hash);
    }
    return adminRpcProxy_->Connect(server_uri, client_id);
}

bool ChronoLogClient::Disconnect(const std::string &client_id, int &flags) {
    return adminRpcProxy_->Disconnect(client_id, flags);
}

bool ChronoLogClient::CreateChronicle(std::string &name, const std::unordered_map<std::string, std::string> &attrs) {
    return metadataRpcProxy_->CreateChronicle(name, attrs);
}

bool ChronoLogClient::DestroyChronicle(std::string &name, const int &flags) {
    return metadataRpcProxy_->DestroyChronicle(name, flags);
}

bool ChronoLogClient::AcquireChronicle(std::string &name, const int &flags) {
    return metadataRpcProxy_->AcquireChronicle(name, flags);
}

bool ChronoLogClient::ReleaseChronicle(std::string &name, const int &flags) {
    return metadataRpcProxy_->ReleaseChronicle(name, flags);
}

bool ChronoLogClient::CreateStory(std::string &chronicle_name, std::string &story_name, const std::unordered_map<std::string, std::string> &attrs) {
    return metadataRpcProxy_->CreateStory(chronicle_name, story_name, attrs);
}

bool ChronoLogClient::DestroyStory(std::string &chronicle_name, std::string &story_name, const int &flags) {
    return metadataRpcProxy_->DestroyStory(chronicle_name, story_name, flags);
}

bool ChronoLogClient::AcquireStory(std::string &chronicle_name, std::string &story_name, const int &flags) {
    return metadataRpcProxy_->AcquireStory(chronicle_name, story_name, flags);
}

bool ChronoLogClient::ReleaseStory(std::string &chronicle_name, std::string &story_name, const int &flags) {
    return metadataRpcProxy_->ReleaseStory(chronicle_name, story_name, flags);
}

std::string ChronoLogClient::GetChronicleAttr(std::string &chronicle_name, const std::string &key) {
    return metadataRpcProxy_->GetChronicleAttr(chronicle_name, key);
}

bool ChronoLogClient::EditChronicleAttr(std::string &chronicle_name, const std::string &key, const std::string &value) {
    return metadataRpcProxy_->EditChronicleAttr(chronicle_name, key, value);
}