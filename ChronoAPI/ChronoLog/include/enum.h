//
// Created by kfeng on 4/1/22.
//

#ifndef CHRONOLOG_ENUM_H
#define CHRONOLOG_ENUM_H

inline const char* getRPCImplString(ChronoLogRPCImplementation impl) {
    switch (impl) {
        case CHRONOLOG_THALLIUM_SOCKETS: return "CHRONOLOG_THALLIUM_SOCKETS";
        case CHRONOLOG_THALLIUM_TCP: return "CHRONOLOG_THALLIUM_TCP";
        case CHRONOLOG_THALLIUM_ROCE: return "CHRONOLOG_THALLIUM_ROCE";
        default: return "UNKNOWN";
    }
}

typedef enum ChronoLogServiceRole {
    CHRONOLOG_UNKNOWN = 0,
    CHRONOLOG_VISOR = 1,
    CHRONOLOG_CLIENT = 2,
    CHRONOLOG_KEEPER = 3
} ChronoLogServiceRole;

inline const char* getServiceRoleString(ChronoLogServiceRole role) {
    switch (role) {
        case CHRONOLOG_UNKNOWN: return "CHRONOLOG_UNKNOWN";
        case CHRONOLOG_VISOR: return "CHRONOLOG_VISOR";
        case CHRONOLOG_CLIENT: return "CHRONOLOG_CLIENT";
        case CHRONOLOG_KEEPER: return "CHRONOLOG_KEEPER";
        default: return "CHRONOLOG_UNKNOWN";
    }
}

enum ClocksourceType {
    C_STYLE = 0,
    CPP_STYLE = 1,
    TSC = 2
};

inline const char* getClocksourceTypeString(ClocksourceType type) {
    switch (type) {
        case C_STYLE: return "C_STYLE";
        case CPP_STYLE: return "CPP_STYLE";
        case TSC: return "TSC";
        default: return "UNKNOWN";
    }
}

#endif //CHRONOLOG_ENUM_H
