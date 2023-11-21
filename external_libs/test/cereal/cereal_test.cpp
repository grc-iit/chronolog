//
// Created by kfeng on 11/15/21.
//

#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <utility>
#include <cereal/types/vector.hpp>
#include <fstream>
#include <chrono>
#include <cstring>

#define NUM_CHRONOKEEPER 3
#define SERDE_BUF_SIZE (1024 * 1024)

struct ChronoKeeperInfo
{
    std::string ip_;
    int port_;

    template <class Archive>
    void serialize(Archive &ar)
    {
        ar(ip_, port_);
    }

    ChronoKeeperInfo(): ip_("0.0.0.0"), port_(0)
    {}

    ChronoKeeperInfo(std::string ip, int port): ip_(std::move(ip)), port_(port)
    {}

    std::string toString() const
    {
        return "(" + ip_ + ":" + std::to_string(port_) + ")";
    }
};

struct TimeInfo
{
    std::string timestamp_;
    double driftRate_;

    template <class Archive>
    void serialize(Archive &ar)
    {
        ar(timestamp_, driftRate_);
    }

    TimeInfo(): driftRate_(0)
    {}

    TimeInfo(std::string timestamp, double driftRate): timestamp_(std::move(timestamp)), driftRate_(driftRate)
    {}

    std::string toString() const
    {
        return "(" + timestamp_ + ", " + std::to_string(driftRate_) + ")";
    }
};

struct ClientMessage
{
    enum ClientMessageType
    {
        UNKNOWN = -1, CONNECTION = 0, DISCONNECTION = 1, GETTIMEINFO = 2
    };

    ClientMessageType msgType_;

    template <class Archive>
    void serialize(Archive &ar)
    {
        ar(msgType_);
    }

    ClientMessage()
    { msgType_ = ClientMessageType::UNKNOWN; }

    std::string toString() const
    {
        return "Type: " + std::to_string(msgType_);
    }
};

struct ServerMessage
{
    enum ServerMessageType
    {
        UNKNOWN = -1, CONNRESPONSE = 0, DISCONNRESPONSE = 1, GETTIMEINFORESPONSE = 2, GETCHRONOKEEPERRESPONSE = 3
    };

    ServerMessageType msgType_;
    TimeInfo timeInfo_;
    std::vector <ChronoKeeperInfo> chronoKeeperList_;

    template <class Archive>
    void serialize(Archive &ar)
    {
        ar(msgType_, timeInfo_, chronoKeeperList_);
    }

    ServerMessage(): msgType_(ServerMessageType::UNKNOWN), timeInfo_(TimeInfo()), chronoKeeperList_()
    {}

    std::string toString()
    {
        std::string chronoKeeperListStr;
        for(const ChronoKeeperInfo &info: chronoKeeperList_)
        {
            if(!chronoKeeperListStr.empty()) chronoKeeperListStr += ", ";
            chronoKeeperListStr += info.toString();
        }
        return "Type: " + std::to_string(msgType_) + ", timeinfo: " + timeInfo_.toString() + ", ChronoKeeper list: " +
               chronoKeeperListStr;
    }
};

unsigned char*shared_msg_buffer_unsigned = nullptr;
char*shared_msg_buffer_default = nullptr;
std::string shared_msg_string;

int main()
{
    std::string clientMsgFile = "/tmp/client_msg";
    std::string serverMsgFile = "/tmp/server_msg";
    std::chrono::high_resolution_clock::time_point t1, t2;
    std::chrono::duration <double> duration{};

    //
    // file-based serialization
    //
    std::cout << "*** File-based serialization ***" << std::endl;
    // client ====>>>>
    t1 = std::chrono::high_resolution_clock::now();
    {
        std::ofstream os(clientMsgFile, std::ios::binary|std::ios::trunc);
        cereal::BinaryOutputArchive archive(os);

        ClientMessage clientMsg;
        clientMsg.msgType_ = ClientMessage::CONNECTION;
        archive(clientMsg);

        std::cout << "Msg to server: " << clientMsg.toString() << std::endl;
    }
    t2 = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast <std::chrono::duration <double>>(t2 - t1);
    std::cout << "Time used to serialize: " << duration.count() * 1000 << " ms" << std::endl;

    // server <<<<====
    t1 = std::chrono::high_resolution_clock::now();
    {
        std::ifstream is(clientMsgFile, std::ios::binary);
        cereal::BinaryInputArchive archive(is);

        ClientMessage clientMsg;
        archive(clientMsg);

        std::cout << "Msg from client: " << clientMsg.toString() << std::endl;
    }
    t2 = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast <std::chrono::duration <double>>(t2 - t1);
    std::cout << "Time used to deserialize: " << duration.count() * 1000 << " ms" << std::endl;

    // server ====>>>>
    t1 = std::chrono::high_resolution_clock::now();
    {
        std::ofstream os(serverMsgFile, std::ios::binary|std::ios::trunc);
        cereal::BinaryOutputArchive archive(os);

        ServerMessage serverMsg;
        TimeInfo timeInfo(std::string("1234567890"), 0.00001111);
        std::vector <ChronoKeeperInfo> chronoKeeperList;
        for(int i = 0; i < NUM_CHRONOKEEPER; i++)
        {
            ChronoKeeperInfo chronoKeeperInfo(std::string("172.25.101.") + std::to_string(i + 1), 8000 + i);
            chronoKeeperList.push_back(chronoKeeperInfo);
        }
        serverMsg.msgType_ = ServerMessage::CONNRESPONSE;
        serverMsg.timeInfo_ = timeInfo;
        serverMsg.chronoKeeperList_ = chronoKeeperList;
        archive(serverMsg);

        std::cout << "Msg to client: " << serverMsg.toString() << std::endl;
    }
    t2 = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast <std::chrono::duration <double>>(t2 - t1);
    std::cout << "Time used to serialize: " << duration.count() * 1000 << " ms" << std::endl;

    // client <<<<====
    t1 = std::chrono::high_resolution_clock::now();
    {
        std::ifstream is(serverMsgFile, std::ios::binary);
        cereal::BinaryInputArchive archive(is);

        ServerMessage serverMsg;
        archive(serverMsg);

        std::cout << "Msg from server: " << serverMsg.toString() << std::endl;
    }
    t2 = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast <std::chrono::duration <double>>(t2 - t1);
    std::cout << "Time used to deserialize: " << duration.count() * 1000 << " ms" << std::endl;

    //
    // memory-based serialization
    //
    std::cout << "*** Memory-based serialization ***" << std::endl;
    shared_msg_buffer_unsigned = (unsigned char*)malloc(SERDE_BUF_SIZE);
    // client ====>>>>
    t1 = std::chrono::high_resolution_clock::now();
    {
        std::ostringstream oss;
        cereal::BinaryOutputArchive archive(oss);

        ClientMessage clientMsg;
        clientMsg.msgType_ = ClientMessage::CONNECTION;
        archive(clientMsg);

        std::cout << "Serialized msg to server: " << clientMsg.toString() << std::endl;
        memset(shared_msg_buffer_unsigned, 0, SERDE_BUF_SIZE);
        memcpy(shared_msg_buffer_unsigned, oss.str().c_str(), oss.str().length());
    }
    t2 = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast <std::chrono::duration <double>>(t2 - t1);
    std::cout << "Time used to serialize: " << duration.count() * 1000 << " ms" << std::endl;

    // server <<<<====
    t1 = std::chrono::high_resolution_clock::now();
    {
        std::istringstream iss;
        iss.rdbuf()->pubsetbuf(reinterpret_cast<char*>(const_cast<unsigned char*>(shared_msg_buffer_unsigned))
                               , SERDE_BUF_SIZE);
        cereal::BinaryInputArchive archive(iss);

        ClientMessage clientMsg;
        archive(clientMsg);

        std::cout << "Msg from client: " << clientMsg.toString() << std::endl;
    }
    t2 = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast <std::chrono::duration <double>>(t2 - t1);
    std::cout << "Time used to deserialize: " << duration.count() * 1000 << " ms" << std::endl;

    // server ====>>>>
    t1 = std::chrono::high_resolution_clock::now();
    {
        std::ostringstream oss;
        cereal::BinaryOutputArchive archive(oss);

        ServerMessage serverMsg;
        TimeInfo timeInfo(std::string("1234567890"), 0.00001111);
        std::vector <ChronoKeeperInfo> chronoKeeperList;
        for(int i = 0; i < NUM_CHRONOKEEPER; i++)
        {
            ChronoKeeperInfo chronoKeeperInfo(std::string("172.25.101.") + std::to_string(i + 1), 8000 + i);
            chronoKeeperList.push_back(chronoKeeperInfo);
        }
        serverMsg.msgType_ = ServerMessage::CONNRESPONSE;
        serverMsg.timeInfo_ = timeInfo;
        serverMsg.chronoKeeperList_ = chronoKeeperList;
        archive(serverMsg);

        std::cout << "Msg to client: " << serverMsg.toString() << std::endl;
        shared_msg_string.assign(oss.str().c_str());
        memcpy(shared_msg_buffer_default, oss.str().c_str(), oss.str().length());
    }
    t2 = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast <std::chrono::duration <double>>(t2 - t1);
    std::cout << "Time used to serialize: " << duration.count() * 1000 << " ms" << std::endl;

    // client <<<<====
    t1 = std::chrono::high_resolution_clock::now();
    {
        std::istringstream iss;
        iss.rdbuf()->pubsetbuf(reinterpret_cast<char*>(const_cast<char*>(shared_msg_buffer_default)), SERDE_BUF_SIZE);
        cereal::BinaryInputArchive archive(iss);

        ServerMessage serverMsg;
        archive(serverMsg);

        std::cout << "Msg from server: " << serverMsg.toString() << std::endl;
    }
    t2 = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast <std::chrono::duration <double>>(t2 - t1);
    std::cout << "Time used to deserialize: " << duration.count() * 1000 << " ms" << std::endl;

    free(shared_msg_buffer_unsigned);

    return 0;
}