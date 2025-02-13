#ifndef PLAYER_REGISTRATION_MSG_H
#define PLAYER_REGISTRATION_MSG_H

#include <arpa/inet.h>
#include <iostream>

#include "PlayerIdCard.h"

namespace chronolog
{

class PlayerRegistrationMsg
{

    PlayerIdCard playerIdCard;
    ServiceId adminServiceId;

public:
    PlayerRegistrationMsg(PlayerIdCard const& id_card = PlayerIdCard{},
                           ServiceId const& service_id = ServiceId{})
        : playerIdCard(id_card)
        , adminServiceId(service_id)

    {}

    ~PlayerRegistrationMsg() = default;

    PlayerIdCard const & getPlayerIdCard() const
    { return playerIdCard; }

    ServiceId const & getAdminServiceId() const
    { return adminServiceId; }

    template <typename SerArchiveT>
    void serialize(SerArchiveT &serT)
    {
        serT & playerIdCard;
        serT & adminServiceId;
    }

};

inline std::string to_string( PlayerRegistrationMsg const& msg)
{
    return std::string("PlayerRegistrationMsg{") + to_string(msg.getPlayerIdCard())
            + std::string("}{admin:") + to_string(msg.getAdminServiceId()) +"}";
}

}//namespace

inline std::ostream &operator<<(std::ostream & out, chronolog::PlayerRegistrationMsg const &msg)
{
    out << "PlayerRegistrationMsg{" << msg.getPlayerIdCard() << "}{admin:" << msg.getAdminServiceId() << "}";
    return out;
}

inline std::string& operator+= (std::string& a_string, chronolog::PlayerRegistrationMsg const &msg)
{
    a_string += chronolog::to_string(msg);
    return a_string;
}

#endif
