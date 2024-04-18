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
    GrapherRegistrationMsg(GrapherIdCard const& id_card = GrapherIdCard{0, 0, 0},
                           ServiceId const& admin_service_id = ServiceId{0, 0, 0})
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
        serT&grapherIdCard;
        serT&adminServiceId;
    }

};

}//namespace

inline std::ostream &operator<<(std::ostream &out, chronolog::GrapherRegistrationMsg const &msg)
{
    out << "GrapherRegistrationMsg{" << msg.getGrapherIdCard() << "}{admin:" << msg.getAdminServiceId() << "}";
    return out;
}

#endif
