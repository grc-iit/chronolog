#ifndef CHRONOLOG_SERVER_ERRCODE_H
#define CHRONOLOG_SERVER_ERRCODE_H

#include <client_errcode.h>

namespace chronolog {

// A simple enum for server-only errors:
enum ServerErrorCode {
    CL_ERR_STORY_EXISTS                = -101,  // Specified Story exists, cannot be created/renamed to
    CL_ERR_ARCHIVE_EXISTS              = -102,  // Specified Archive exists, cannot be created/renamed to
    CL_ERR_CHRONICLE_PROPERTY_FULL     = -103,  // Property list of Chronicle is full, cannot add new property
    CL_ERR_STORY_PROPERTY_FULL         = -104,  // Property list of Story is full, cannot add new property
    CL_ERR_CHRONICLE_METADATA_FULL     = -105,  // Metadata list of Chronicle is full, cannot add new property
    CL_ERR_STORY_METADATA_FULL         = -106,  // Metadata list of Story is full, cannot add new property
    CL_ERR_INVALID_CONF                = -107,  // Invalid configuration, cannot be created
    CL_ERR_STORY_CHUNK_EXISTS          = -108,  // Specified Story chunk exists, cannot be created
    CL_ERR_CHRONICLE_DIR_NOT_EXIST     = -109,  // Chronicle directory does not exist
    CL_ERR_STORY_FILE_NOT_EXIST        = -110,  // Story file does not exist
    CL_ERR_STORY_CHUNK_DSET_NOT_EXIST  = -111,  // Story chunk dataset does not exist
    CL_ERR_STORY_CHUNK_EXTRACTION      = -112   // Error in extracting Story chunk in ChronoKeeper
};

// Convert enum value to its name (for logging)
inline const char* to_string(ServerErrorCode e) {
    switch (e) {
    case CL_ERR_STORY_EXISTS:               return "CL_ERR_STORY_EXISTS";
    case CL_ERR_ARCHIVE_EXISTS:             return "CL_ERR_ARCHIVE_EXISTS";
    case CL_ERR_CHRONICLE_PROPERTY_FULL:    return "CL_ERR_CHRONICLE_PROPERTY_FULL";
    case CL_ERR_STORY_PROPERTY_FULL:        return "CL_ERR_STORY_PROPERTY_FULL";
    case CL_ERR_CHRONICLE_METADATA_FULL:    return "CL_ERR_CHRONICLE_METADATA_FULL";
    case CL_ERR_STORY_METADATA_FULL:        return "CL_ERR_STORY_METADATA_FULL";
    case CL_ERR_INVALID_CONF:               return "CL_ERR_INVALID_CONF";
    case CL_ERR_STORY_CHUNK_EXISTS:         return "CL_ERR_STORY_CHUNK_EXISTS";
    case CL_ERR_CHRONICLE_DIR_NOT_EXIST:    return "CL_ERR_CHRONICLE_DIR_NOT_EXIST";
    case CL_ERR_STORY_FILE_NOT_EXIST:       return "CL_ERR_STORY_FILE_NOT_EXIST";
    case CL_ERR_STORY_CHUNK_DSET_NOT_EXIST: return "CL_ERR_STORY_CHUNK_DSET_NOT_EXIST";
    case CL_ERR_STORY_CHUNK_EXTRACTION:     return "CL_ERR_STORY_CHUNK_EXTRACTION";
    default:                                return "UnknownServerErrorCode";
    }
}

inline const char* to_string_server(int code) {
    switch (static_cast<chronolog::ServerErrorCode>(code)) {
        case CL_ERR_STORY_EXISTS:
        case CL_ERR_ARCHIVE_EXISTS:
        case CL_ERR_CHRONICLE_PROPERTY_FULL:
        case CL_ERR_STORY_PROPERTY_FULL:
        case CL_ERR_CHRONICLE_METADATA_FULL:
        case CL_ERR_STORY_METADATA_FULL:
        case CL_ERR_INVALID_CONF:
        case CL_ERR_STORY_CHUNK_EXISTS:
        case CL_ERR_CHRONICLE_DIR_NOT_EXIST:
        case CL_ERR_STORY_FILE_NOT_EXIST:
        case CL_ERR_STORY_CHUNK_DSET_NOT_EXIST:
        case CL_ERR_STORY_CHUNK_EXTRACTION:
            return to_string(static_cast<chronolog::ServerErrorCode>(code));
        default:
            return "UnknownServerErrorCode";
    }
}

} // namespace chronolog

#endif // CHRONOLOG_SERVER_ERRCODE_H