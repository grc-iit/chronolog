//
// Created by kfeng on 5/17/22.
//

#include "client.h"

bool ChronoLogClient::Connect(const std::string &server_uri, std::string &client_id) {
    return adminRpcProxy_.Connect(server_uri, client_id);
}

bool ChronoLogClient::Disconnect(const std::string &client_id, int &flags) {
    return adminRpcProxy_.Disconnect(client_id, flags);
}

bool ChronoLogClient::CreateChronicle(std::string &name, const std::unordered_map<std::string, std::string> &attrs) {
    return metadataRpcProxy_.CreateChronicle(name, attrs);
}

bool ChronoLogClient::DestroyChronicle(std::string &name, const int &flags) {
    return metadataRpcProxy_.DestroyChronicle(name, flags);
}

bool ChronoLogClient::AcquireChronicle(std::string &name, const int &flags) {
    return metadataRpcProxy_.AcquireChronicle(name, flags);
}

bool ChronoLogClient::ReleaseChronicle(uint64_t &cid, const int &flags) {
    return metadataRpcProxy_.ReleaseChronicle(cid, flags);
}

bool ChronoLogClient::CreateStory(uint64_t &cid, std::string &name, const std::unordered_map<std::string, std::string> &attrs) {
    return metadataRpcProxy_.CreateStory(cid, name, attrs);
}

bool ChronoLogClient::DestroyStory(uint64_t &cid, std::string &name, const int &flags) {
    return metadataRpcProxy_.DestroyStory(cid, name, flags);
}

bool ChronoLogClient::AcquireStory(uint64_t &cid, std::string &name, const int &flags) {
    return metadataRpcProxy_.AcquireStory(cid, name, flags);
}

bool ChronoLogClient::ReleaseStory(uint64_t &sid, const int &flags) {
    return metadataRpcProxy_.ReleaseStory(sid, flags);
}

std::string ChronoLogClient::GetChronicleAttr(uint64_t &cid, const std::string &key) {
    return metadataRpcProxy_.GetChronicleAttr(cid, key);
}

bool ChronoLogClient::EditChronicleAttr(uint64_t &cid, const std::string &key, const std::string &value) {
    return metadataRpcProxy_.EditChronicleAttr(cid, key, value);
}