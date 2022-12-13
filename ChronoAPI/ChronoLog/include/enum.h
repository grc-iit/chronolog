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

#endif //CHRONOLOG_ENUM_H
