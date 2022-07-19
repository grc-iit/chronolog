//
// Created by kfeng on 10/27/21.
//

#ifndef SOCKETPP_CLIENTREGISTRYRECORD_H
#define SOCKETPP_CLIENTREGISTRYRECORD_H

#include <iostream>
#include <utility>

class ClientRegistryRecord {
public:
    ClientRegistryRecord() : addr_() {}
    explicit ClientRegistryRecord(std::string addr) : addr_(std::move(addr)) {}

    friend std::ostream &operator<<(std::ostream &out, const ClientRegistryRecord &r) {
        return out << "addr: " << r.addr_;
    }

    [[nodiscard]] std::string to_string() const {
        return "addr: " + addr_;
    }

    template<typename A> friend void serialize(A& ar, ClientRegistryRecord& r);

    std::string addr_;
};

template<typename A>
void serialize(A& ar, ClientRegistryRecord& r) {
    ar & r.addr_;
}

#endif //SOCKETPP_CLIENTREGISTRYRECORD_H
