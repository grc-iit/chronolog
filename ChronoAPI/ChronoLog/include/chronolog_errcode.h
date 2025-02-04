//
// Created by kfeng on 12/19/22.
//

#ifndef CHRONOLOG_CHRONOLOG_ERRCODE_H
#define CHRONOLOG_CHRONOLOG_ERRCODE_H
namespace chronolog
{
enum ErrorCode
{
    CL_SUCCESS = 0,                         // Success
    CL_ERR_UNKNOWN = -1,                    // Error does not belong to any categories below
    CL_ERR_INVALID_ARG = -2,                // Invalid arguments
    CL_ERR_NOT_EXIST = -3,                  // Specified Chronicle or Story or property does not exist
    CL_ERR_ACQUIRED = -4,                   // Specified Chronicle or Story is acquired, cannot be destroyed
    CL_ERR_NOT_ACQUIRED = -5,               // Specified Chronicle or Story is not acquired, cannot be released
    CL_ERR_CHRONICLE_EXISTS = -6,           // Specified Chronicle exists, cannot be created/renamed to
    CL_ERR_STORY_EXISTS = -7,               // Specified Story exists, cannot be created/renamed to
    CL_ERR_Archive_EXISTS = -8,             // Specified Archive exists, cannot be created/renamed to
    CL_ERR_CHRONICLE_PROPERTY_FULL = -9,    // Property list of Chronicle is full, cannot add new property
    CL_ERR_STORY_PROPERTY_FULL = -10,       // Property list of Story is full, cannot add new property
    CL_ERR_CHRONICLE_METADATA_FULL = -11,   // Metadata list of Chronicle is full, cannot add new property
    CL_ERR_STORY_METADATA_FULL = -12,       // Metadata list of Story is full, cannot add new property
    CL_ERR_INVALID_CONF = -13,              // Content of configuration file is invalid
    CL_ERR_NO_KEEPERS = -14,                // No ChronoKeepers are available
    CL_ERR_NO_CONNECTION = -15,             // No valid connection to ChronoVisor
    CL_ERR_NOT_AUTHORIZED = -16,            // Client is not authorized for operation
    CL_ERR_STORY_CHUNK_EXISTS = -17,        // Specified Story chunk exists, cannot be created
    CL_ERR_CHRONICLE_DIR_NOT_EXIST = -18,   // Chronicle directory does not exist
    CL_ERR_STORY_FILE_NOT_EXIST = -19,      // Story file does not exist
    CL_ERR_STORY_CHUNK_DSET_NOT_EXIST = -20,// Story chunk dataset does not exist
    CL_ERR_STORY_CHUNK_EXTRACTION = -21,    // Error in extracting Story chunk in ChronoKeeper
    CL_ERR_NO_PLAYERS = -22,                // No ChronoPlayers are available for story playback
};
}

#endif //CHRONOLOG_CHRONOLOG_ERRCODE_H
