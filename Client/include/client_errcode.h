#ifndef CHRONOLOG_CLIENT_ERRCODE_H
#define CHRONOLOG_CLIENT_ERRCODE_H

namespace chronolog {

// A simple enum for client-only errors:
enum ClientErrorCode {
    CL_SUCCESS             = 0,    // Success
    CL_ERR_UNKNOWN         = -1,   // Generic error
    CL_ERR_INVALID_ARG     = -2,   // Invalid input
    CL_ERR_NOT_EXIST       = -3,   // Missing Chronicle, Story, or property
    CL_ERR_ACQUIRED        = -4,   // Already acquired, cannot destroy
    CL_ERR_NOT_ACQUIRED    = -5,   // Not acquired, cannot release
    CL_ERR_CHRONICLE_EXISTS= -6,   // Chronicle already exists
    CL_ERR_NO_KEEPERS      = -7,   // No ChronoKeepers available
    CL_ERR_NO_CONNECTION   = -8,   // No connection to ChronoVisor
    CL_ERR_NOT_AUTHORIZED  = -9,   // Unauthorized operation
    CL_ERR_NO_PLAYERS      = -10,   // No ChronoPlayers available
    CL_ERR_NOT_READER_MODE = -11,  // Client is running in WRITER_MODE
    CL_ERR_QUERY_TIMED_OUT = -12   // Replay query timed out
};

// Convert enum value to its name (for logging)
inline const char* to_string(ClientErrorCode e) {
    switch (e) {
    case CL_SUCCESS:             return "CL_SUCCESS";
    case CL_ERR_UNKNOWN:         return "CL_ERR_UNKNOWN";
    case CL_ERR_INVALID_ARG:     return "CL_ERR_INVALID_ARG";
    case CL_ERR_NOT_EXIST:       return "CL_ERR_NOT_EXIST";
    case CL_ERR_ACQUIRED:        return "CL_ERR_ACQUIRED";
    case CL_ERR_NOT_ACQUIRED:    return "CL_ERR_NOT_ACQUIRED";
    case CL_ERR_CHRONICLE_EXISTS:return "CL_ERR_CHRONICLE_EXISTS";
    case CL_ERR_NO_KEEPERS:      return "CL_ERR_NO_KEEPERS";
    case CL_ERR_NO_CONNECTION:   return "CL_ERR_NO_CONNECTION";
    case CL_ERR_NOT_AUTHORIZED:  return "CL_ERR_NOT_AUTHORIZED";
    case CL_ERR_NO_PLAYERS:      return "CL_ERR_NO_PLAYERS";
    case CL_ERR_NOT_READER_MODE: return "CL_ERR_NOT_READER_MODE";
    case CL_ERR_QUERY_TIMED_OUT: return "CL_ERR_QUERY_TIMED_OUT";
    default:                     return "UnknownClientErrorCode";
    }
}

inline const char* to_string_client(int code) {
    switch (static_cast<chronolog::ClientErrorCode>(code)) {
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
            return to_string(static_cast<chronolog::ClientErrorCode>(code));
        default:
            return "UnknownClientErrorCode";
    }
}

} // namespace chronolog

#endif // CHRONOLOG_CLIENT_ERRCODE_H
