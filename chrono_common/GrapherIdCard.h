#ifndef GRAPHER_ID_CARD_H
#define GRAPHER_ID_CARD_H

#include <arpa/inet.h>

#include <iostream>

#include "ServiceId.h"

// this class wrapps ChronoGrapher Process identification 
// that will be used by all the ChronoLog Processes 
// to both identofy the process and create RPC client channels 
// to send the data to the RecordingService it contains

namespace chronolog
{

class GrapherIdCard
{
    RecordingGroupId groupId;
    ServiceId   recordingServiceId;

public:
    GrapherIdCard(RecordingGroupId group_id = 0,  ServiceId const& service_id = ServiceId{})
        : groupId(group_id)
        , recordingServiceId(service_id)
    {}

    GrapherIdCard(GrapherIdCard const& other)
        : groupId(other.getGroupId())
        , recordingServiceId(other.getRecordingServiceId())
    {}

    ~GrapherIdCard()=default;

    RecordingGroupId getGroupId() const { return groupId; }
    ServiceId const& getRecordingServiceId() const { return recordingServiceId; }

    // serialization function used by thallium RPC providers
    // to serialize/deserialize 
    template <typename SerArchiveT>
    void serialize( SerArchiveT & serT)
    {
        serT & groupId;
        serT & recordingServiceId;
    }

};
 
inline std::string to_string( GrapherIdCard const & id_card)
{
    std::string a_string;   
    return std::string("GrapherIdCard{Group{") + std::to_string(id_card.getGroupId()) + "}" +
        chronolog::to_string(id_card.getRecordingServiceId()) + "}";
}

} //namespace chronolog

inline bool operator==(chronolog::GrapherIdCard const& card1, chronolog::GrapherIdCard const& card2)
{
    return ((card1.getGroupId() == card2.getGroupId()) && (card1.getRecordingServiceId() == card2.getRecordingServiceId()));
}

inline std::ostream & operator<< (std::ostream & out , chronolog::GrapherIdCard const & id_card)
{
    std::string a_string;
    out << "GrapherIdCard{Group{" << id_card.getGroupId() << "}" << id_card.getRecordingServiceId()<< "}";
    return out;
}

inline std::string& operator+= (std::string& a_string, chronolog::GrapherIdCard const& id_card)
{
    a_string += std::string("GrapherIdCard{Group{") + std::to_string(id_card.getGroupId()) + "}" +
                chronolog::to_string(id_card.getRecordingServiceId()) + "}";
    return a_string;
}

#endif
