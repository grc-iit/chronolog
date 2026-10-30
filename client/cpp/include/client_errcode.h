#ifndef CHRONOLOG_CLIENT_ERRCODE_H
#define CHRONOLOG_CLIENT_ERRCODE_H

namespace chronolog
{

// A simple enum for client-only errors:
enum ClientErrorCode
{
    CL_SUCCESS = 0,                         // Success
    CL_ERR_UNKNOWN = -1,                    // Generic error
    CL_ERR_INVALID_ARG = -2,                // Invalid input
    CL_ERR_NOT_EXIST = -3,                  // Missing Chronicle, Story, or property
    CL_ERR_ACQUIRED = -4,                   // Already acquired; cannot be destroyed; unsafe to disconnect
    CL_ERR_NOT_ACQUIRED = -5,               // Not acquired; cannot be released
    CL_ERR_CHRONICLE_EXISTS = -6,           // Chronicle already exists
    CL_ERR_NO_KEEPERS = -7,                 // No ChronoKeepers available
    CL_ERR_NO_CONNECTION = -8,              // No connection to ChronoVisor
    CL_ERR_NOT_AUTHORIZED = -9,             // Unauthorized operation
    CL_ERR_NO_PLAYERS = -10,                // No ChronoPlayers available
    CL_ERR_NOT_READER_MODE = -11,           // Client is running in WRITER_MODE
    CL_ERR_QUERY_TIMED_OUT = -12,           // Replay query timed out
    CL_ERR_PROTOCOL_VERSION_MISMATCH = -13, // Client and server disagree on the wire-protocol version
    // -14..-17 are deliberately skipped: the ingest acceptance-window and
    // collision-policy work being developed in parallel already assigns them, and
    // reusing a value here would silently change what a code means once the two
    // meet. Leaving the gap keeps both sets stable across that merge.
    CL_ERR_PARTIAL_RESULT = -18, // Query returned fewer records than it had promised
    // -19..-21 are reserved for the parallel keeper-WAL work.
    CL_ERR_DEADLINE_EXCEEDED = -22,         // The local operation budget expired
    CL_ERR_OUTCOME_UNCONFIRMED = -23,       // A dispatched operation has no confirmed outcome
    CL_ERR_RPC_CAPACITY = -24,              // Owned RPC work or policy admission is unavailable
    CL_ERR_CLOCK_UNAVAILABLE = -25,         // Event time could not be read
    CL_ERR_TIMESTAMP_EXHAUSTED = -26,       // No larger local timestamp can be assigned
    CL_ERR_SHUTDOWN_INCOMPLETE = -27,       // Shutdown expired while owned work remains
    CL_ERR_NATIVE_RUNTIME_UNSUPPORTED = -28 // Preexisting native runtime cannot be adopted
};

// Convert enum value to its name (for logging)
inline const char* to_string(ClientErrorCode e)
{
    switch(e)
    {
        case CL_SUCCESS:
            return "CL_SUCCESS";
        case CL_ERR_UNKNOWN:
            return "CL_ERR_UNKNOWN";
        case CL_ERR_INVALID_ARG:
            return "CL_ERR_INVALID_ARG";
        case CL_ERR_NOT_EXIST:
            return "CL_ERR_NOT_EXIST";
        case CL_ERR_ACQUIRED:
            return "CL_ERR_ACQUIRED";
        case CL_ERR_NOT_ACQUIRED:
            return "CL_ERR_NOT_ACQUIRED";
        case CL_ERR_CHRONICLE_EXISTS:
            return "CL_ERR_CHRONICLE_EXISTS";
        case CL_ERR_NO_KEEPERS:
            return "CL_ERR_NO_KEEPERS";
        case CL_ERR_NO_CONNECTION:
            return "CL_ERR_NO_CONNECTION";
        case CL_ERR_NOT_AUTHORIZED:
            return "CL_ERR_NOT_AUTHORIZED";
        case CL_ERR_NO_PLAYERS:
            return "CL_ERR_NO_PLAYERS";
        case CL_ERR_NOT_READER_MODE:
            return "CL_ERR_NOT_READER_MODE";
        case CL_ERR_QUERY_TIMED_OUT:
            return "CL_ERR_QUERY_TIMED_OUT";
        case CL_ERR_PROTOCOL_VERSION_MISMATCH:
            return "CL_ERR_PROTOCOL_VERSION_MISMATCH";
        case CL_ERR_PARTIAL_RESULT:
            return "CL_ERR_PARTIAL_RESULT";
        case CL_ERR_DEADLINE_EXCEEDED:
            return "CL_ERR_DEADLINE_EXCEEDED";
        case CL_ERR_OUTCOME_UNCONFIRMED:
            return "CL_ERR_OUTCOME_UNCONFIRMED";
        case CL_ERR_RPC_CAPACITY:
            return "CL_ERR_RPC_CAPACITY";
        case CL_ERR_CLOCK_UNAVAILABLE:
            return "CL_ERR_CLOCK_UNAVAILABLE";
        case CL_ERR_TIMESTAMP_EXHAUSTED:
            return "CL_ERR_TIMESTAMP_EXHAUSTED";
        case CL_ERR_SHUTDOWN_INCOMPLETE:
            return "CL_ERR_SHUTDOWN_INCOMPLETE";
        case CL_ERR_NATIVE_RUNTIME_UNSUPPORTED:
            return "CL_ERR_NATIVE_RUNTIME_UNSUPPORTED";
        default:
            return "UnknownClientErrorCode";
    }
}

inline const char* to_string_client(int code)
{
    switch(static_cast<chronolog::ClientErrorCode>(code))
    {
        case CL_SUCCESS:
        case CL_ERR_UNKNOWN:
        case CL_ERR_INVALID_ARG:
        case CL_ERR_NOT_EXIST:
        case CL_ERR_ACQUIRED:
        case CL_ERR_NOT_ACQUIRED:
        case CL_ERR_CHRONICLE_EXISTS:
        case CL_ERR_NO_KEEPERS:
        case CL_ERR_NO_CONNECTION:
        case CL_ERR_NOT_AUTHORIZED:
        case CL_ERR_NO_PLAYERS:
        case CL_ERR_NOT_READER_MODE:
        case CL_ERR_QUERY_TIMED_OUT:
        case CL_ERR_PROTOCOL_VERSION_MISMATCH:
        case CL_ERR_PARTIAL_RESULT:
        case CL_ERR_DEADLINE_EXCEEDED:
        case CL_ERR_OUTCOME_UNCONFIRMED:
        case CL_ERR_RPC_CAPACITY:
        case CL_ERR_CLOCK_UNAVAILABLE:
        case CL_ERR_TIMESTAMP_EXHAUSTED:
        case CL_ERR_SHUTDOWN_INCOMPLETE:
        case CL_ERR_NATIVE_RUNTIME_UNSUPPORTED:
            return to_string(static_cast<chronolog::ClientErrorCode>(code));
        default:
            return "UnknownClientErrorCode";
    }
}

} // namespace chronolog

#endif // CHRONOLOG_CLIENT_ERRCODE_H
