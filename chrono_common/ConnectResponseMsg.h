#ifndef CLIENT_CONNECT_RESPONSE_MSG_H
#define CLIENT_CONNECT_RESPONSE_MSG_H

#include <iostream>
#include "chronolog_types.h"

namespace chronolog
{

class ConnectResponseMsg
{
    int error_code;
    ClientId clientId;

public:

    ConnectResponseMsg(): error_code(chronolog::CL_SUCCESS), clientId(0)
    {}

    ConnectResponseMsg(int code, ClientId const &client_id): error_code(code), clientId(client_id)
    {}

    ~ConnectResponseMsg() = default;

    int getErrorCode() const
    { return error_code; }

    ClientId const &getClientId() const
    { return clientId; }

    template <typename SerArchiveT>
    void serialize(SerArchiveT &serT)
    {
        serT&error_code;
        serT&clientId;
    }

};

}//namespace


inline std::ostream &operator<<(std::ostream &out, chronolog::ConnectResponseMsg const &msg)
{
    out << "ConnectResponseMsg{" << msg.getErrorCode() << "}{client_id:" << msg.getClientId() << "}";
    return out;
}

#endif
