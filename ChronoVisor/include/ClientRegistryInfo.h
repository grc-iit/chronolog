//
// Created by kfeng on 10/27/21.
//

#ifndef SOCKETPP_CLIENTREGISTRYINFO_H
#define SOCKETPP_CLIENTREGISTRYINFO_H

#include <iostream>
#include <utility>

class ClientRegistryInfo {
public:
    ClientRegistryInfo() : addr_() {}
    explicit ClientRegistryInfo(std::string addr) : addr_(std::move(addr)) {}

    friend std::ostream &operator<<(std::ostream &out, const ClientRegistryInfo &r) {
        return out << "addr: " << r.addr_;
    }

    [[nodiscard]] std::string to_string() const {
        return "addr: " + addr_;
    }

    template<typename A> friend void serialize(A& ar, ClientRegistryInfo& r);

    std::string addr_;
    std::string group_;
    uint32_t role_;

};

template<typename A>
void serialize(A& ar, ClientRegistryInfo& r) {
    ar & r.addr_;
}

#endif //SOCKETPP_CLIENTREGISTRYINFO_H
