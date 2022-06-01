//
// Created by kfeng on 11/15/21.
//

#ifndef CEREAL_TEST_MESSAGE_H
#define CEREAL_TEST_MESSAGE_H

#include <ostream>

/** \struct ChronoKeeperInfo
 * Structure to store IP and port of a ChronoKeeper
 */
struct ChronoKeeperInfo {
    std::string ip_;
    int port_;

    template <class Archive>
    void serialize(Archive & ar) {
        ar(ip_, port_);
    }

    ChronoKeeperInfo() : ip_("0.0.0.0"), port_(0) {}

    ChronoKeeperInfo(std::string ip, int port) : ip_(std::move(ip)), port_(port) {}

    std::string toString() const {
        return "(" + ip_ + ":" + std::to_string(port_) + ")";
    }

    friend std::ostream &operator<<(std::ostream &os, const ChronoKeeperInfo &info) {
        os << info.ip_ << ":" << info.port_;
        return os;
    }
};

/**
 * \struct TimeInfo
 * Structure to store clock sent over network, including timestamp and drift rate
 */
struct TimeInfo {
    std::string timestamp_;
    double driftRate_;

    template <class Archive>
    void serialize(Archive & ar) {
        ar(timestamp_, driftRate_);
    }

    TimeInfo() : driftRate_(0) {}

    TimeInfo(std::string timestamp, double driftRate) : timestamp_(std::move(timestamp)), driftRate_(driftRate) {}

    std::string toString() const {
        return "(" + timestamp_ + ", " + std::to_string(driftRate_) + ")";
    }

    friend std::ostream &operator<<(std::ostream &os, const TimeInfo &info) {
        os << "timestamp_: " << info.timestamp_ << ", driftRate_: " << info.driftRate_;
        return os;
    }
};

const int CLIENTMSGTYPE_ADMIN_MASK = 1 << 4;
const int CLIENTMSGTYPE_CHRONICLE_MASK = 1 << 8;
const int CLIENTMSGTYPE_EVENT_MASK = 1 << 12;

/**
 * \struct ClientMessage
 * Structure to store all the info client sends to server
 */
struct ClientMessage {
    enum ClientMessageType {
        UNKNOWN = -1,
        CONNECTION = CLIENTMSGTYPE_ADMIN_MASK + 1,
        DISCONNECTION = CLIENTMSGTYPE_ADMIN_MASK + 2,
        GETTIMEINFO = CLIENTMSGTYPE_ADMIN_MASK + 3,
        GETCHRONOKEEPERINFO = CLIENTMSGTYPE_ADMIN_MASK + 4,
        CREATECHRONICLE = CLIENTMSGTYPE_CHRONICLE_MASK + 1,
        EDITCHRONICLE = CLIENTMSGTYPE_CHRONICLE_MASK + 2,
        DESTROYCHRONICLE = CLIENTMSGTYPE_CHRONICLE_MASK + 3,
        ACQUIRECHRONICLE = CLIENTMSGTYPE_CHRONICLE_MASK + 4,
        RELEASECHRONICLE = CLIENTMSGTYPE_CHRONICLE_MASK + 5,
        RECORDEVENT = CLIENTMSGTYPE_EVENT_MASK + 1,
        PLAYBACKEVENT = CLIENTMSGTYPE_EVENT_MASK + 2,
        REPLAYEVENT = CLIENTMSGTYPE_EVENT_MASK + 3
    };

    ClientMessageType msgType_;                             ///< type
    std::string chronicleName_;
    std::string storyName_;
    uint64_t chronicleId_;
    uint64_t storyId_;

    template <class Archive>
    void serialize(Archive & ar) {
        ar(msgType_, chronicleName_, storyName_, chronicleId_, storyId_);
    }

    ClientMessage() {
        msgType_ = ClientMessageType::UNKNOWN;
        chronicleName_ = {};
        storyName_ = {};
        chronicleId_ = 0;
        storyId_ = 0;
    }

    std::string toString() const {
        return "Type: " + std::to_string(msgType_)
               + ", chronicle name: " + chronicleName_
               + ", story name: " + storyName_
               + ", chronicle id: " + std::to_string(chronicleId_)
               + ", story id: " + std::to_string(storyId_);
    }

    friend std::ostream &operator<<(std::ostream &os, const ClientMessage &message) {
        os << "msgType_: " << message.msgType_
           << ", chronicleName_: " << message.chronicleName_
           << ", storyName_: " << message.storyName_
           << ", chronicleId_: " << message.chronicleId_
           << ", storyId_: " << message.storyId_;
        return os;
    }
};

const int SERVERMSGTYPE_ADMIN_MASK = 1 << 4;
const int SERVERMSGTYPE_CHRONICLE_MASK = 1 << 8;
const int SERVERMSGTYPE_EVENT_MASK = 1 << 12;

/**
 * \struct ServerMessage
 * Structure to store all the info server sends to client
 */
struct ServerMessage {
    enum ServerMessageType {
        UNKNOWN = -1,
        CONNRESPONSE = SERVERMSGTYPE_ADMIN_MASK + 1,
        DISCONNRESPONSE = SERVERMSGTYPE_ADMIN_MASK + 2,
        GETTIMEINFORESPONSE = SERVERMSGTYPE_ADMIN_MASK + 3,
        GETCHRONOKEEPERINFORESPONSE = SERVERMSGTYPE_ADMIN_MASK + 4,
        CREATECHRONICLERESPONSE = SERVERMSGTYPE_CHRONICLE_MASK + 1,
        EDITCHRONICLERESPONSE = SERVERMSGTYPE_CHRONICLE_MASK + 2,
        DESTROYCHRONICLERESPONSE = SERVERMSGTYPE_CHRONICLE_MASK + 3,
        ACQUIRECHRONICLERESPONSE = SERVERMSGTYPE_CHRONICLE_MASK + 4,
        RELEASECHRONICLERESPONSE = SERVERMSGTYPE_CHRONICLE_MASK + 5
    };

    ServerMessageType msgType_;                             ///< type
    TimeInfo timeInfo_;                                     ///< timestamp and drift rate
    std::vector<ChronoKeeperInfo> chronoKeeperList_;        ///< a list of ChronoKeeperInfo for clients to access
    uint64_t chronicleId_;
    uint64_t storyId_;

    template <class Archive>
    void serialize(Archive & ar) {
        ar(msgType_, timeInfo_, chronoKeeperList_, chronicleId_, storyId_);
    }

    ServerMessage() : msgType_(ServerMessageType::UNKNOWN),
                      timeInfo_(TimeInfo()),
                      chronoKeeperList_(),
                      chronicleId_(0),
                      storyId_(0) {}

    std::string toString() {
        std::string chronoKeeperListStr;
        for (const ChronoKeeperInfo& info : chronoKeeperList_) {
            if (!chronoKeeperListStr.empty()) chronoKeeperListStr += ", ";
            chronoKeeperListStr += info.toString();
        }
        return "Type: " + std::to_string(msgType_)
               + ", timeinfo: " + timeInfo_.toString()
               + ", ChronoKeeper list: " + chronoKeeperListStr
               + ", Chronicle ID: " + std::to_string(chronicleId_)
               + ", Story ID: " + std::to_string(storyId_);
    }

    friend std::ostream &operator<<(std::ostream &os, const ServerMessage &message) {
        os << "msgType_: " << message.msgType_ << ", timeInfo_: " << message.timeInfo_ << ", chronoKeeperList_: ";
        for (ChronoKeeperInfo chronoKeeperInfo : message.chronoKeeperList_)
           os << chronoKeeperInfo << ", ";
        os << ", Chronicle ID: " << message.chronicleId_ << ", Story ID: " << message.storyId_;
        return os;
    }
};

#endif //CEREAL_TEST_MESSAGE_H
