#ifndef KEEPER_REGISTRATION_MSG_H
#define KEEPER_REGISTRATION_MSG_H

#include <arpa/inet.h>
#include <iostream>

#include "KeeperIdCard.h"
#include "ServiceId.h"

namespace chronolog
{

class KeeperRegistrationMsg
{

    KeeperIdCard keeperIdCard;
    ServiceId adminServiceId;

public:
    KeeperRegistrationMsg(KeeperIdCard const& keeper_card = KeeperIdCard{},
                          ServiceId const& admin_service_id = ServiceId{})
        : keeperIdCard(keeper_card)
        , adminServiceId(admin_service_id)
    {}

    ~KeeperRegistrationMsg() = default;

    KeeperIdCard const &getKeeperIdCard() const
    { return keeperIdCard; }

    ServiceId const &getAdminServiceId() const
    { return adminServiceId; }

    template <typename SerArchiveT>
    void serialize(SerArchiveT & serT)
    {
        serT & keeperIdCard;
        serT & adminServiceId;
    }

};

inline std::string to_string(chronolog::KeeperRegistrationMsg const & msg)
{
    return std::string("KeeperRegistrationMsg{") + to_string(msg.getKeeperIdCard()) + "}";
        std::string( "}{admin:") + to_string(msg.getAdminServiceId()) + "}";
}


}//namespace

inline std::ostream &operator<<(std::ostream &out, chronolog::KeeperRegistrationMsg const &msg)
{
    out << "KeeperRegistrationMsg{" << msg.getKeeperIdCard() << "}{admin:" << msg.getAdminServiceId() << "}";
    return out;
}

inline std::string & operator+= (std::string &a_string, chronolog::KeeperRegistrationMsg const & msg)
{
    a_string += chronolog::to_string(msg);
    return a_string;
}

#endif
