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
    CHRONOLOG_CLIENT_RO = 0,
    CHRONOLOG_CLIENT_RW = 1,
    CHRONOLOG_CLIENT_RWCD = 2,
    CHRONOLOG_CLIENT_CLUS_ADMIN = 3,
    CHRONOLOG_CLIENT_CLUS_REG = 4,
    CHRONOLOG_CLIENT_GROUP_ADMIN = 5,
    CHRONOLOG_CLIENT_GROUP_REG = 6
} ChronoLogClientRole;

typedef enum ChronoLogVisibility {
     CHRONOLOG_RONLY = 0,
     CHRONOLOG_RW = 1,
     CHRONOLOG_RWC = 2,
     CHRONOLOG_RWD = 3,
     CHRONOLOG_RWCD = 4
}ChronoLogVisibility;

typedef enum ChronoLogOp {
    CHRONOLOG_READ = 0,
    CHRONOLOG_WRITE = 1,
    CHRONOLOG_FILEOP = 2
}ChronoLogOp;

#endif //CHRONOLOG_ENUM_H
