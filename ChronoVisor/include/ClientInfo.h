//
// Created by kfeng on 1/2/23.
//

#ifndef CHRONOLOG_CLIENTINFO_H
#define CHRONOLOG_CLIENTINFO_H

#include <sstream>

class Story;

class ClientInfo
{
public:
    ClientInfo() = default;

    [[nodiscard]] std::string to_string() const
    {
        std::string str = addr_ + ":" + std::to_string(port_) + ", acquiredStories: ";
        std::stringstream ss;
        auto it = acquiredStoryList_.begin();
        ss << it->first;
        for(++it; it != acquiredStoryList_.end(); ++it)
        {
            ss << ", " << it->first;
        }
        return str + ss.str();
    }

    template <typename A>
    friend void serialize(A &ar, ClientInfo &r);

    std::string addr_;
    uint16_t port_;
    std::unordered_map <uint64_t, Story*> acquiredStoryList_;
};

std::ostream &operator<<(std::ostream &out, const ClientInfo &r);

template <typename A>
void serialize(A &ar, ClientInfo &r)
{
    ar&r.addr_;
    ar&r.port_;
    ar&r.acquiredStoryList_;
}

#endif //CHRONOLOG_CLIENTINFO_H
