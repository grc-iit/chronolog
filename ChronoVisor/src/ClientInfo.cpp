//
// Created by kfeng on 1/2/23.
//

#include <ClientInfo.h>

std::ostream &operator<<(std::ostream &out, const ClientInfo &r)
{
    std::stringstream ss;
    auto it = r.acquiredStoryList_.begin();
    ss << it->first;
    for(++it; it != r.acquiredStoryList_.end(); ++it)
    {
        ss << ", " << it->first;
    }
    return out << r.addr_ << ":" << r.port_ << ", acquiredStories: " << ss.str();
}