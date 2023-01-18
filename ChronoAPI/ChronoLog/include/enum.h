//
// Created by kfeng on 4/1/22.
//

#ifndef CHRONOLOG_ENUM_H
#define CHRONOLOG_ENUM_H

typedef enum ChronoLogRPCImplementation {
    CHRONOLOG_THALLIUM_SOCKETS = 0,
    CHRONOLOG_THALLIUM_TCP = 1,
    CHRONOLOG_THALLIUM_ROCE = 2
} ChronoLogRPCImplementation;

typedef enum ChronoLogClientRole {
    CHRONOLOG_CLIENT_ADMIN = 0,
    CHRONOLOG_CLIENT_USER_RDONLY = 1,
    CHRONOLOG_CLIENT_USER_RW = 2
} ChronoLogClientRole;

typedef enum ChronoLogVisibility {
     CHRONOLOG_PUBLIC = 0,
     CHRONOLOG_GROUP_RW = 1,
     CHRONOLOG_GROUP_RDONLY = 2,
     CHRONOLOG_PRIVATE = 3
}ChronoLogVisibility;

typedef enum ChronoLogOp {
    CHRONOLOG_READ = 0,
    CHRONOLOG_WRITE = 1
}ChronoLogOp;

#endif //CHRONOLOG_ENUM_H
