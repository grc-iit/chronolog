#ifndef CHRONOLOG_CONFIGURATION_BLOCKS_H
#define CHRONOLOG_CONFIGURATION_BLOCKS_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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
    // Max most-recent sealed events retained per story in the keeper tail for
    // last-N playback before they age out to the extraction/archive path.
    // Keeper-only knob; ignored by grapher/player.
    int tail_capacity = 65536;
    // Soft cap (MB) on the keeper's retained sealed-chunk memory. On exceed
    // the retention store WARNs (the grapher's persisted watermark is lagging,
    // e.g. an outage) but keeps retaining — unpersisted data is never dropped.
    // 0 disables the warning. Keeper-only knob; ignored by grapher/player.
    int retention_cap_mb = 4096;
    // Re-send a retained chunk whose grapher ack or covering watermark report
    // has not arrived after this long. Must exceed the grapher's
    // story_chunk_duration + acceptance_window, which is how long a healthy
    // chunk waits to be written, or healthy chunks are sent and written twice.
    // The template's grapher windows total 90 s, so this is a little over 3x.
    // Keeper-only knob.
    int watermark_resend_timeout_secs = 300;
    // After the grapher reports a chunk written, the keeper keeps the chunk
    // and serves its events as unconfirmed for this long, so a replay does not
    // depend on the player already seeing the new archive file. A replay now
    // looks a missing window's file up by name rather than waiting for the
    // player's next directory listing (ArchiveReaders.archive_window_secs), so
    // this only has to cover the write-to-report round trip -- provided the
    // players' archive mount does not cache failed lookups. NFS does by
    // default (lookupcache=all): a name a player asked for just before the file
    // appeared stays "not found" until the client revalidates the directory,
    // anywhere from acdirmin to acdirmax (30-60 s by default, longer where a
    // site raises them to spare its server). Mount the archive with
    // lookupcache=positive (see the multi-node deployment docs), or raise this
    // above acdirmax. 0 frees on the report. Keeper-only knob.
    int archive_visibility_delay_secs = 10;
    // How long a keeper stopped with SIGTERM waits for the grapher to confirm
    // every chunk it holds written, sending unacked chunks again meanwhile.
    // Cover the grapher's story_chunk_duration + acceptance_window (the
    // template's 30+60). 0 exits without waiting. Keeper-only knob.
    int shutdown_confirm_timeout_secs = 150;
    // How often the grapher pushes dirty per-story persisted watermarks to
    // the contributing keepers. Grapher-only knob; ignored by keeper/player.
    int watermark_report_interval_secs = 1;
    // When true, playback()/tail reads also serve events from the active
    // (unsealed) timeline in addition to sealed chunks, cutting write-to-visible
    // latency from the seal window (chunk_duration + acceptance_window) down to
    // ~the ingestion tick. Reads inside the acceptance window are provisional
    // (a late arrival may sort behind an already-seen event). Keeper-only knob;
    // defaults false to preserve the sealed-only, final-result semantics.
    bool live_tail_read = false;

    DataStoreConf() {}

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[DATA_STORE_CONF: max_story_chunk_size: " + std::to_string(max_story_chunk_size) +
               " story_chunk_duration_secs: " + std::to_string(story_chunk_duration_secs) +
               " acceptance_window_secs: " + std::to_string(acceptance_window_secs) +
               " inactive_story_delay_secs: " + std::to_string(inactive_story_delay_secs) +
               " tail_capacity: " + std::to_string(tail_capacity) +
               " retention_cap_mb: " + std::to_string(retention_cap_mb) +
               " watermark_resend_timeout_secs: " + std::to_string(watermark_resend_timeout_secs) +
               " archive_visibility_delay_secs: " + std::to_string(archive_visibility_delay_secs) +
               " shutdown_confirm_timeout_secs: " + std::to_string(shutdown_confirm_timeout_secs) +
               " watermark_report_interval_secs: " + std::to_string(watermark_report_interval_secs) +
               " live_tail_read: " + (live_tail_read ? "true" : "false") + "]";
    }
};

struct ExtractorReaderConf
{
    std::string story_files_dir;
    // How often the player lists the archive directory to find new files. On
    // NFS a listing is served from the client's directory cache, so it can be
    // as old as acdirmax whatever this interval is; mount the archive with a
    // small acdirmin/acdirmax to keep it close. Player-only knob.
    int archive_scan_interval_secs = 5;
    // The grapher's story_chunk_duration_secs: the time range of one archive
    // file, and so the step between the names a replay probes for files the
    // last listing did not show. Set it to the grapher's value. 0 turns
    // probing off and leaves a replay with whatever the listing has.
    // Player-only knob.
    int archive_window_secs = 30;

    int parseJsonConf(json_object*);

    [[nodiscard]] std::string to_String() const
    {
        return "[EXTRACTOR_READER_CONF: STORY_FILES_DIR: " + story_files_dir +
               " archive_scan_interval_secs: " + std::to_string(archive_scan_interval_secs) +
               " archive_window_secs: " + std::to_string(archive_window_secs) + "]";
    }
};

} // namespace chronolog

#endif //CHRONOLOG_CONFIGURATIONMANAGER_H
