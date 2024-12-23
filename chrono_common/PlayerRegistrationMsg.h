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
    ServiceId playbackServiceId;

public:
    PlayerRegistrationMsg(PlayerIdCard const& id_card = PlayerIdCard{0, 0, 0},
                           ServiceId const& service_id = ServiceId{0, 0, 0})
        : playerIdCard(id_card)
        , playbackServiceId(service_id)

    {}

    ~PlayerRegistrationMsg() = default;

    PlayerIdCard const & getPlayerIdCard() const
    { return playerIdCard; }

    ServiceId const & getPlaybackServiceId() const
    { return playbackServiceId; }

    template <typename SerArchiveT>
    void serialize(SerArchiveT &serT)
    {
        serT & playerIdCard;
        serT & playbackServiceId;
    }

};

inline std::string to_string( PlayerRegistrationMsg const& msg)
{
    return std::string("PlayerRegistrationMsg{") + to_string(msg.getPlayerIdCard())
            + std::string("}{playback:") + to_string(msg.getPlaybackServiceId()) +"}";
}

}//namespace

inline std::ostream &operator<<(std::ostream & out, chronolog::PlayerRegistrationMsg const &msg)
{
    out << "PlayerRegistrationMsg{" << msg.getPlayerIdCard() << "}{playback:" << msg.getPlaybackServiceId() << "}";
    return out;
}

inline std::string& operator+= (std::string& a_string, chronolog::PlayerRegistrationMsg const &msg)
{
    a_string += chronolog::to_string(msg);
    return a_string;
}

#endif
