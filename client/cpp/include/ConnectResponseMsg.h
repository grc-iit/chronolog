#ifndef CLIENT_CONNECT_RESPONSE_MSG_H
#define CLIENT_CONNECT_RESPONSE_MSG_H

#include <iostream>

#include "chronolog_types.h"
#include "client_errcode.h"
#include "ClockState.h"

namespace chronolog
{

class ConnectResponseMsg
{
    int error_code;
    ClientId clientId;
    // Visor clock state handed to the client on Connect so it can anchor its
    // ChronoTicks to the Visor timeline (only visor_time is Visor-populated; the
    // client derives offset/uncertainty from its own round-trip timing).
    ClockState clockState;

public:
    ConnectResponseMsg()
        : error_code(chronolog::CL_SUCCESS)
        , clientId(0)
        , clockState{}
    {}

    ConnectResponseMsg(int code, ClientId const& client_id, ClockState const& clock_state = ClockState{})
        : error_code(code)
        , clientId(client_id)
        , clockState(clock_state)
    {}

    ~ConnectResponseMsg() = default;

    int getErrorCode() const { return error_code; }

    ClientId const& getClientId() const { return clientId; }

    ClockState const& getClockState() const { return clockState; }

    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & error_code;
        serT & clientId;
        serT & clockState;
    }
};

} // namespace chronolog


inline std::ostream& operator<<(std::ostream& out, chronolog::ConnectResponseMsg const& msg)
{
    out << "ConnectResponseMsg{" << msg.getErrorCode() << "}{client_id:" << msg.getClientId() << "}";
    return out;
}

#endif
