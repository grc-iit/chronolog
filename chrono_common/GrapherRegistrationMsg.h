#ifndef GRAPHER_REGISTRATION_MSG_H
#define GRAPHER_REGISTRATION_MSG_H

#include <arpa/inet.h>
#include <iostream>

#include "GrapherIdCard.h"

namespace chronolog
{

class GrapherRegistrationMsg
{

    GrapherIdCard grapherIdCard;
    ServiceId adminServiceId;

public:
    GrapherRegistrationMsg(GrapherIdCard const& id_card = GrapherIdCard{},
                           ServiceId const& admin_service_id = ServiceId{})
        : grapherIdCard(id_card)
        , adminServiceId(admin_service_id)

    {}

    ~GrapherRegistrationMsg() = default;

    GrapherIdCard const &getGrapherIdCard() const
    { return grapherIdCard; }

    ServiceId const &getAdminServiceId() const
    { return adminServiceId; }

    template <typename SerArchiveT>
    void serialize(SerArchiveT &serT)
    {
        serT & grapherIdCard;
        serT & adminServiceId;
    }

};

inline std::string to_string(GrapherRegistrationMsg const & msg)
{
    return std::string("GrapherRegistrationMsg{") + to_string(msg.getGrapherIdCard())
        + std::string("}{admin:") + to_string(msg.getAdminServiceId()) + "}";
}

}//namespace

inline std::ostream &operator<<(std::ostream &out, chronolog::GrapherRegistrationMsg const &msg)
{
    out << "GrapherRegistrationMsg{" << msg.getGrapherIdCard() << "}{admin:" << msg.getAdminServiceId() << "}";
    return out;
}

inline std::string & operator+= (std::string & a_string, chronolog::GrapherRegistrationMsg const &msg)
{
    a_string += chronolog::to_string(msg);
    return a_string;
}

#endif
