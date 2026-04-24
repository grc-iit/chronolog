#ifndef CHRONOLOG_CONFIGURATION_BLOCKS_H
#define CHRONOLOG_CONFIGURATION_BLOCKS_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <cassert>
#include <json-c/json.h>
#include <sstream>

#include <log_level.h>

#include "chronolog_errcode.h"

namespace chronolog
{

enum ClocksourceType
{
    C_STYLE = 0,
    CPP_STYLE = 1,
    TSC = 2
};

inline const char* getClocksourceTypeString(ClocksourceType type)
{
    switch(type)
    {
        case C_STYLE:
            return "C_STYLE";
        case CPP_STYLE:
            return "CPP_STYLE";
        case TSC:
            return "TSC";
        default:
            return "UNKNOWN";
    }
}

struct ClockConf
{
    ClocksourceType CLOCKSOURCE_TYPE;
    uint64_t DRIFT_CAL_SLEEP_SEC;
    uint64_t DRIFT_CAL_SLEEP_NSEC;

    ClockConf()
    {
        /* Clock-related configurations */
        CLOCKSOURCE_TYPE = ClocksourceType::C_STYLE;
        DRIFT_CAL_SLEEP_SEC = 10;
        DRIFT_CAL_SLEEP_NSEC = 0;
    }

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "CLOCKSOURCE_TYPE: " + std::string(getClocksourceTypeString(CLOCKSOURCE_TYPE)) +
               ", DRIFT_CAL_SLEEP_SEC: " + std::to_string(DRIFT_CAL_SLEEP_SEC) +
               ", DRIFT_CAL_SLEEP_NSEC: " + std::to_string(DRIFT_CAL_SLEEP_NSEC);
    }
};

struct AuthConf
{
    std::string AUTH_TYPE;
    std::string MODULE_PATH;

    AuthConf()
    {
        /* Authentication-related configurations */
        AUTH_TYPE = "RBAC";
        MODULE_PATH = "";
    }

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const { return "AUTH_TYPE: " + AUTH_TYPE + ", MODULE_PATH: " + MODULE_PATH; }
};

struct RPCProviderConf
{
    std::string PROTO_CONF;
    std::string IP;
    uint16_t BASE_PORT{};
    uint16_t SERVICE_PROVIDER_ID{};

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[PROTO_CONF: " + PROTO_CONF + ", IP: " + IP + ", BASE_PORT: " + std::to_string(BASE_PORT) +
               ", SERVICE_PROVIDER_ID: " + std::to_string(SERVICE_PROVIDER_ID) + ", PORTS: " + "]";
    }
};

struct LogConf
{
    std::string LOGTYPE;
    std::string LOGFILE;
    LogLevel LOGLEVEL{};
    std::string LOGNAME;
    size_t LOGFILESIZE{};
    size_t LOGFILENUM{};
    LogLevel FLUSHLEVEL{};

    void parselogLevelConf(json_object* json_conf, LogLevel& log_level)
    {
        if(json_object_is_type(json_conf, json_type_string))
        {
            const char* conf_str = json_object_get_string(json_conf);
            if(strcmp(conf_str, "trace") == 0)
            {
                log_level = LogLevel::trace;
            }
            else if(strcmp(conf_str, "info") == 0)
            {
                log_level = LogLevel::info;
            }
            else if(strcmp(conf_str, "debug") == 0)
            {
                log_level = LogLevel::debug;
            }
            else if(strcmp(conf_str, "warning") == 0)
            {
                log_level = LogLevel::warn;
            }
            else if(strcmp(conf_str, "error") == 0)
            {
                log_level = LogLevel::err;
            }
            else if(strcmp(conf_str, "critical") == 0)
            {
                log_level = LogLevel::critical;
            }
            else if(strcmp(conf_str, "off") == 0)
            {
                log_level = LogLevel::off;
            }
            else
            {
                std::cout << "[ConfigurationManager] Unknown log level: " << conf_str << std::endl;
            }
        }
        else
        {
            std::cerr << "[ConfigurationManager] Invalid Log Level implementation configuration" << std::endl;
        }
    }

    void parseFlushLevelConf(json_object* json_conf, LogLevel& flush_level)
    {
        if(json_object_is_type(json_conf, json_type_string))
        {
            const char* conf_str = json_object_get_string(json_conf);
            if(strcmp(conf_str, "trace") == 0)
            {
                flush_level = LogLevel::trace;
            }
            else if(strcmp(conf_str, "info") == 0)
            {
                flush_level = LogLevel::info;
            }
            else if(strcmp(conf_str, "debug") == 0)
            {
                flush_level = LogLevel::debug;
            }
            else if(strcmp(conf_str, "warning") == 0)
            {
                flush_level = LogLevel::warn;
            }
            else if(strcmp(conf_str, "error") == 0)
            {
                flush_level = LogLevel::err;
            }
            else if(strcmp(conf_str, "critical") == 0)
            {
                flush_level = LogLevel::critical;
            }
            else if(strcmp(conf_str, "off") == 0)
            {
                flush_level = LogLevel::off;
            }
            else
            {
                std::cout << "[ConfigurationManager] Unknown flush level: " << conf_str
                          << "Set it to default value: "
                             "Warning"
                          << std::endl;
                flush_level = LogLevel::warn;
            }
        }
        else
        {
            std::cerr << "[ConfigurationManager] Invalid Flush Level implementation configuration" << std::endl;
        }
    }

    static std::string LevelToString(LogLevel level)
    {
        switch(level)
        {
            case LogLevel::trace:
                return "TRACE";
            case LogLevel::debug:
                return "DEBUG";
            case LogLevel::info:
                return "INFO";
            case LogLevel::warn:
                return "WARN";
            case LogLevel::err:
                return "ERROR";
            case LogLevel::critical:
                return "CRITICAL";
            case LogLevel::off:
                return "OFF";
            default:
                return "UNKNOWN";
        }
    }

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[TYPE: " + LOGTYPE + ", FILE: " + LOGFILE + ", LEVEL: " + LevelToString(LOGLEVEL) +
               ", NAME: " + LOGNAME + ", LOGFILESIZE: " + std::to_string(LOGFILESIZE) +
               ", LOGFILENUM: " + std::to_string(LOGFILENUM) + ", FLUSH LEVEL: " + LevelToString(FLUSHLEVEL) + "]";
    }
};

struct DataStoreConf
{
    int max_story_chunk_size = 4096;
    int story_chunk_duration_secs = 30;
    int acceptance_window_secs = 60;
    int inactive_story_delay_secs = 180;

    DataStoreConf() {}

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[DATA_STORE_CONF: max_story_chunk_size: " + std::to_string(max_story_chunk_size) +
               " story_chunk_duration_secs: " + std::to_string(story_chunk_duration_secs) +
               " acceptance_window_secs: " + std::to_string(acceptance_window_secs) +
               " inactive_story_delay_secs: " + std::to_string(inactive_story_delay_secs) + "]";
    }
};

struct ExtractorReaderConf
{
    std::string story_files_dir;

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[EXTRACTOR_READER_CONF: STORY_FILES_DIR: " + story_files_dir + "]";
    }
};

} // namespace chronolog

#endif //CHRONOLOG_CONFIGURATIONMANAGER_H
