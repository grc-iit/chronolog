//
// Created by kfeng on 12/19/22.
//

#ifndef CHRONOLOG_ERRCODE_H
#define CHRONOLOG_ERRCODE_H

#define CL_SUCCESS                          0       /* Success */
#define CL_ERR_UNKNOWN                      -1      /* Error does not belong to any categories below */
#define CL_ERR_INVALID_ARG                  -2      /* Invalid arguments */
#define CL_ERR_NOT_EXIST                    -3      /* Specified Chronicle or Story does not exist */
#define CL_ERR_ACQUIRED                     -4      /* Specified Chronicle or Story is acquired, cannot be destroyed */
#define CL_ERR_CHRONICLE_EXISTS             -5      /* Specified Chronicle exists, cannot be created/renamed to */
#define CL_ERR_STORY_EXISTS                 -6      /* Specified Story exists, cannot be created/renamed to */
#define CL_ERR_Archive_EXISTS               -7      /* Specified Archive exists, cannot be created/renamed to */
#define CL_ERR_CHRONICLE_PROPERTY_FULL      -8      /* Property list of Chronicle is full, cannot add new property */
#define CL_ERR_STORY_PROPERTY_FULL          -9      /* Property list of Story is full, cannot add new property */
#define CL_ERR_CHRONICLE_METADATA_FULL      -10     /* Metadata list of Chronicle is full, cannot add new property */
#define CL_ERR_STORY_METADATA_FULL          -11     /* Metadata list of Story is full, cannot add new property */
#define CL_ERR_INVALID_CONF                 -12     /* Content of configuration file is invalid */

#endif //CHRONOLOG_ERRCODE_H
