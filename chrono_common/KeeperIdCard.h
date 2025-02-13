#ifndef _KEEPER_ID_CARD_H
#define _KEEPER_ID_CARD_H

#include <arpa/inet.h>

#include <iostream>

#include "ServiceId.h"

// this class wrapps ChronoKeeper Process identification 
// that will be used by all the ChronoLog Processes 
// to both identify the Keepr process and create RPC client channels 
// to send the data to the Keeper RecordingService and Keeper DataStoreAdminService

namespace chronolog
{


class KeeperIdCard
{

    RecordingGroupId groupId;
    ServiceId recordingServiceId;

public:


    KeeperIdCard( uint32_t group_id = 0, ServiceId const& service_id = ServiceId{})
        : groupId(group_id)
        , recordingServiceId(service_id)
    {}

    KeeperIdCard( KeeperIdCard const& other)
        : groupId(other.getGroupId())
        , recordingServiceId(other.getRecordingServiceId())
    {}

    ~KeeperIdCard()=default;

    RecordingGroupId getGroupId() const { return groupId; }
    ServiceId const& getRecordingServiceId() const { return recordingServiceId; }


    // serialization function used by thallium RPC providers
    // to serialize/deserialize KeeperIdCard
    template <typename SerArchiveT>
    void serialize( SerArchiveT & serT)
    {
        serT & groupId;
        serT & recordingServiceId;
    }

};

inline std::string to_string(chronolog::KeeperIdCard const& keeper_id_card)
{
    std::string a_string;
    return std::string("KeeperIdCard{Group{") + std::to_string(keeper_id_card.getGroupId()) + "}" 
            + to_string(keeper_id_card.getRecordingServiceId()) + "}";
}

} //namespace chronolog


inline bool operator==(chronolog::KeeperIdCard const& card1, chronolog::KeeperIdCard const& card2)
{
    return (card1.getRecordingServiceId()==card2.getRecordingServiceId());

}

inline std::ostream & operator<< (std::ostream & out , chronolog::KeeperIdCard const & keeper_id_card)
{
    std::string a_string;
    out << "KeeperIdCard{Group{"<<keeper_id_card.getGroupId() <<"}"
       <<keeper_id_card.getRecordingServiceId()<<"}";
    return out;
}

inline std::string & operator+= (std::string & a_string , chronolog::KeeperIdCard const & keeper_id_card)
{
    a_string += chronolog::to_string(keeper_id_card);
    return a_string;
}

#endif
