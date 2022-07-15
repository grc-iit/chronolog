//
// Created by kfeng on 4/1/22.
//

#ifndef CHRONOLOG_ENUM_H
#define CHRONOLOG_ENUM_H

typedef enum RPCImplementation {
    THALLIUM_SOCKETS = 0,
    THALLIUM_TCP = 1,
    THALLIUM_ROCE = 2
} RPCImplementation;

#endif //CHRONOLOG_ENUM_H
