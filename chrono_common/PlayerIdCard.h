#ifndef PLAYER_ID_CARD_H
#define PLAYER_ID_CARD_H

#include <arpa/inet.h>

#include <iostream>

#include "ServiceId.h"

// this class wraps ChronoPlayer Process identification 
// that will be used by all the ChronoLog Processes 
// to both identofy the process and create RPC client channels 
// to send the data to the RecordingService it contains

namespace chronolog
{

class PlayerIdCard
{
    RecordingGroupId groupId;
    ServiceId   playbackServiceId;

public:

    PlayerIdCard(RecordingGroupId group_id = 0, ServiceId const& service_id= ServiceId{})
        : groupId(group_id)
        , playbackServiceId(service_id)
    {}

    PlayerIdCard(PlayerIdCard const& other)
        : groupId(other.getGroupId())
        , playbackServiceId(other.getPlaybackServiceId())
    {}


    PlayerIdCard & operator=(PlayerIdCard const& other)
    {
        groupId = other.getGroupId();
        playbackServiceId = other.getPlaybackServiceId();
        return *this;
    }    

    ~PlayerIdCard()=default;

    RecordingGroupId getGroupId() const { return groupId; }
    ServiceId const & getPlaybackServiceId() const { return playbackServiceId; }


    // serialization function used by thallium RPC providers
    // to serialize/deserialize 
    template <typename SerArchiveT>
    void serialize( SerArchiveT & serT)
    {
        serT & groupId;
        serT & playbackServiceId;
    }

};

inline std::string to_string(PlayerIdCard const& id_card)
{
    std::string a_string;
    return std::string("PlayerIdCard{Group{") + std::to_string(id_card.getGroupId()) + "}" +
            chronolog::to_string(id_card.getPlaybackServiceId())+"}";
}

} //namespace chronolog

inline bool operator==(chronolog::PlayerIdCard const& card1, chronolog::PlayerIdCard const& card2)
{
    return ((card1.getGroupId() == card2.getGroupId()) && (card1.getPlaybackServiceId() == card2.getPlaybackServiceId()));
}

inline std::ostream & operator<< (std::ostream & out , chronolog::PlayerIdCard const & id_card)
{
    std::string a_string;
    out << "PlayerIdCard{Group{" << id_card.getGroupId() << "}" << id_card.getPlaybackServiceId() << "}";
    return out;
}

inline std::string& operator+= (std::string& a_string, chronolog::PlayerIdCard const& id_card)
{
    a_string += std::string("PlayerIdCard{Group{") + std::to_string(id_card.getGroupId()) + "}" + chronolog::to_string(id_card.getPlaybackServiceId()) + "}";
    return a_string;
}

#endif
