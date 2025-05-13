#ifndef CHRONOLOG_CHRONOLOG_ERRCODE_H
#define CHRONOLOG_CHRONOLOG_ERRCODE_H

#include "../../../Client/include/client_errcode.h"

namespace chronolog
{
enum ErrorCode
{
    // ---------- Server-only error codes (must be <-100) ----------
    CL_ERR_STORY_EXISTS = -101,                 // Specified Story exists, cannot be created/renamed to
    CL_ERR_Archive_EXISTS = -102,               // Specified Archive exists, cannot be created/renamed to
    CL_ERR_CHRONICLE_PROPERTY_FULL = -103,      // Property list of Chronicle is full, cannot add new property
    CL_ERR_STORY_PROPERTY_FULL = -104,          // Property list of Story is full, cannot add new property
    CL_ERR_CHRONICLE_METADATA_FULL = -105,      // Metadata list of Chronicle is full, cannot add new property
    CL_ERR_STORY_METADATA_FULL = -106,          // Metadata list of Story is full, cannot add new property
    CL_ERR_INVALID_CONF = -107,                 // Invalid configuration, cannot be created
    CL_ERR_STORY_CHUNK_EXISTS = -108,           // Specified Story chunk exists, cannot be created
    CL_ERR_CHRONICLE_DIR_NOT_EXIST = -109,      // Chronicle directory does not exist
    CL_ERR_STORY_FILE_NOT_EXIST = -110,         // Story file does not exist
    CL_ERR_STORY_CHUNK_DSET_NOT_EXIST = -111,   // Story chunk dataset does not exist
    CL_ERR_STORY_CHUNK_EXTRACTION = -112        // Error in extracting Story chunk in ChronoKeeper
};
} 

#endif // CHRONOLOG_CHRONOLOG_ERRCODE_H