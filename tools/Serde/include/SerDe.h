//
// Created by kfeng on 12/1/21.
//

#ifndef CHRONOLOG_SERDE_H
#define CHRONOLOG_SERDE_H

#include <functional>
#include <memory>
#include <cereal/archives/binary.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include "MessageSerDe.h"

enum SerDeType
{
    CEREAL = 0, CAPNPROTO = 1, MSGPACK = 2, FLATBUFFERS = 3
};

class SerDe
{
public:
    static SerDe*Create(SerDeType type);

    virtual ~SerDe()
    {};
    /**
     * @name Serde for server message
     */
    ///@{
    virtual std::unique_ptr <std::ostringstream> serializeServerMessage(ServerMessage msg) = 0;

    virtual std::unique_ptr <ServerMessage> deserializeServerMessage(unsigned char*buf, int length) = 0;
    ///@}

    /**
     * @name Serde for client message
     */
    ///@{
    virtual std::unique_ptr <std::ostringstream> serializeClientMessage(ClientMessage msg) = 0;

    virtual std::unique_ptr <ClientMessage> deserializeClientMessage(unsigned char*buf, int length) = 0;
    ///@}
};

class SerDeCereal: public SerDe
{
public:
    std::unique_ptr <std::ostringstream> serializeServerMessage(ServerMessage msg)
    {
        std::unique_ptr <std::ostringstream> oss = std::make_unique <std::ostringstream>();
        {
            cereal::BinaryOutputArchive archive(*oss);
            archive(msg);
        }
        return std::unique_ptr <std::ostringstream>(std::move(oss));
    }

    std::unique_ptr <ServerMessage> deserializeServerMessage(unsigned char*buf, int length)
    {
        std::unique_ptr <ServerMessage> serverMsg = std::make_unique <ServerMessage>();
        {
            std::istringstream iss;
            iss.rdbuf()->pubsetbuf(reinterpret_cast<char*>(const_cast<unsigned char*>(buf)), length);
            cereal::BinaryInputArchive archive(iss);
            archive(*serverMsg);
        }

        return serverMsg;
    }

    std::unique_ptr <std::ostringstream> serializeClientMessage(ClientMessage msg)
    {
        std::unique_ptr <std::ostringstream> oss = std::make_unique <std::ostringstream>();
        {
            cereal::BinaryOutputArchive archive(*oss);
            archive(msg);
        }
        return std::unique_ptr <std::ostringstream>(std::move(oss));
    }

    std::unique_ptr <ClientMessage> deserializeClientMessage(unsigned char*buf, int length)
    {
        std::unique_ptr <ClientMessage> clientMsg = std::make_unique <ClientMessage>();
        {
            std::istringstream iss;
            iss.rdbuf()->pubsetbuf(reinterpret_cast<char*>(const_cast<unsigned char*>(buf)), length);
            cereal::BinaryInputArchive archive(iss);
            archive(*clientMsg);
        }

        return clientMsg;
    }
};

class SerDeFactory
{
public:
    explicit SerDeFactory(SerDeType type)
    {
        serde_ = SerDe::Create(type);
    }

    ~SerDeFactory()
    {
        if(serde_)
        {
            delete[] serde_;
            serde_ = nullptr;
        }
    }

    SerDe*getSerDe()
    { return serde_; };
private:
    SerDe*serde_;
};

#endif //CHRONOLOG_SERDE_H
