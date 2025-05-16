#ifndef CHRONOLOG_CLIENT_ERRORCODE_H
#define CHRONOLOG_CLIENT_ERRORCODE_H

namespace chronolog
{
    enum ClientErrorCode
    {
        CL_SUCCESS = 0,                    // Success
        CL_ERR_UNKNOWN = -1,               // Generic error
        CL_ERR_INVALID_ARG = -2,           // Invalid input
        CL_ERR_NOT_EXIST = -3,             // Missing Chronicle, Story, or property
        CL_ERR_ACQUIRED = -4,              // Already acquired, cannot destroy
        CL_ERR_NOT_ACQUIRED = -5,          // Not acquired, cannot release
        CL_ERR_CHRONICLE_EXISTS = -6,      // Chronicle already exists
        CL_ERR_NO_KEEPERS = -7,            // No ChronoKeepers available
        CL_ERR_NO_CONNECTION = -8,         // No connection to ChronoVisor
        CL_ERR_NOT_AUTHORIZED = -9,        // Unauthorized operation
        CL_ERR_NO_PLAYERS = -10,           // No ChronoPlayers available
        CL_ERR_NOT_READER_MODE = -11,      // Client is running in WRITER_MODE
        CL_ERR_QUERY_TIMED_OUT             // Replay query timed out
    };
}

#endif // CHRONOLOG_CLIENT_ERRORCODE_H
