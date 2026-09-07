#include <ConfigurationBlocks.h>


namespace chl = chronolog;


int chronolog::ClockConf::parseJsonConf(json_object* clock_conf)
{
    json_object_object_foreach(clock_conf, key, val)
    {
        if(strcmp(key, "clocksource_type") == 0)
        {
            if(json_object_is_type(val, json_type_string))
            {
                const char* clocksource_type = json_object_get_string(val);
                if(strcmp(clocksource_type, "C_STYLE") == 0)
                    CLOCKSOURCE_TYPE = ClocksourceType::C_STYLE;
                else if(strcmp(clocksource_type, "CPP_STYLE") == 0)
                    CLOCKSOURCE_TYPE = ClocksourceType::CPP_STYLE;
                else if(strcmp(clocksource_type, "TSC") == 0)
                    CLOCKSOURCE_TYPE = ClocksourceType::TSC;
                else
                    std::cout << "[ClockConfiguration] Unknown clocksource type: " << clocksource_type << std::endl;
            }
            else
            {
                std::cerr << "[ClockConfiguration] Failed to parse configuration file: clocksource_type is not a string"
                          << std::endl;
                exit(chronolog::CL_ERR_INVALID_CONF);
            }
        }
        else if(strcmp(key, "drift_cal_sleep_sec") == 0)
        {
            if(json_object_is_type(val, json_type_int))
            {
                DRIFT_CAL_SLEEP_SEC = json_object_get_int(val);
            }
            else
            {
                std::cerr << "[ClockConfiguration] Failed to parse configuration file: drift_cal_sleep_sec is not an "
                             "integer"
                          << std::endl;
                return (chronolog::CL_ERR_INVALID_CONF);
            }
        }
        else if(strcmp(key, "drift_cal_sleep_nsec") == 0)
        {
            if(json_object_is_type(val, json_type_int))
            {
                DRIFT_CAL_SLEEP_NSEC = json_object_get_int(val);
            }
            else
            {
                std::cerr << "[ConfigurationManager] Failed to parse configuration file: drift_cal_sleep_nsec is not "
                             "an integer"
                          << std::endl;
                return (chronolog::CL_ERR_INVALID_CONF);
            }
        }
    }
    return chronolog::CL_SUCCESS;
}

int chronolog::AuthConf::parseJsonConf(json_object* auth_conf)
{
    if(auth_conf == nullptr || !json_object_is_type(auth_conf, json_type_object))
    {
        std::cerr << "[AuthConfiguration] Error while parsing configuration file. Authentication configuration is not "
                     "found or is not an object."
                  << std::endl;
        return (chronolog::CL_ERR_INVALID_CONF);
    }
    json_object_object_foreach(auth_conf, key, val)
    {
        if(strcmp(key, "auth_type") == 0)
        {
            if(json_object_is_type(val, json_type_string))
            {
                AUTH_TYPE = json_object_get_string(val);
            }
            else
            {
                std::cerr << "[AuthConfiguration] Failed to parse configuration file: auth_type is not a string"
                          << std::endl;
                return (chronolog::CL_ERR_INVALID_CONF);
            }
        }
        else if(strcmp(key, "module_location") == 0)
        {
            if(json_object_is_type(val, json_type_string))
            {
                MODULE_PATH = json_object_get_string(val);
            }
            else
            {
                std::cerr << "[AuthConfiguration] Failed to parse configuration file: module_location is not a string"
                          << std::endl;
                return (chronolog::CL_ERR_INVALID_CONF);
            }
        }
    }
    return chronolog::CL_SUCCESS;
}

int chronolog::RPCProviderConf::parseJsonConf(json_object* json_conf)
{
    json_object_object_foreach(json_conf, key, val)
    {
        if(strcmp(key, "protocol_conf") == 0)
        {
            if(!json_object_is_type(val, json_type_string))
            {
                std::cerr << "[RPCProviderConf] Invalid 'protocol_conf': expected string" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            PROTO_CONF = json_object_get_string(val);
        }
        else if(strcmp(key, "service_ip") == 0)
        {
            if(!json_object_is_type(val, json_type_string))
            {
                std::cerr << "[RPCProviderConf] Invalid 'service_ip': expected string" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            IP = json_object_get_string(val);
        }
        else if(strcmp(key, "service_base_port") == 0)
        {
            if(!json_object_is_type(val, json_type_int))
            {
                std::cerr << "[RPCProviderConf] Invalid 'service_base_port': expected integer" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            BASE_PORT = json_object_get_int(val);
        }
        else if(strcmp(key, "service_provider_id") == 0)
        {
            if(!json_object_is_type(val, json_type_int))
            {
                std::cerr << "[RPCProviderConf] Invalid 'service_provider_id': expected integer" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            SERVICE_PROVIDER_ID = json_object_get_int(val);
        }
        else
        {
            std::cerr << "[RPCProviderConf] Unknown client end configuration: " << key << std::endl;
        }
    }
    return chronolog::CL_SUCCESS;
}

int chronolog::LogConf::parseJsonConf(json_object* json_conf)
{

    if(!json_object_is_type(json_conf, json_type_object))
    {
        std::cerr << "[LogConf] Logger configuration is not found or is not an object." << std::endl;
        return (chronolog::CL_ERR_INVALID_CONF);
    }
    json_object_object_foreach(json_conf, key, json_val)
    {
        if(strcmp(key, "monitor") != 0)
        {
            std::cerr << "[LogConf] Unknown Log configuration key : " << key << std::endl;
            return (chronolog::CL_ERR_INVALID_CONF);
        }

        json_object_object_foreach(json_val, key, val)
        {
            if(strcmp(key, "type") == 0)
            {
                if(!json_object_is_type(val, json_type_string))
                {
                    std::cerr << "[LogConf] Invalid 'type': expected string" << std::endl;
                    return chl::CL_ERR_INVALID_CONF;
                }
                LOGTYPE = json_object_get_string(val);
            }
            else if(strcmp(key, "file") == 0)
            {
                if(!json_object_is_type(val, json_type_string))
                {
                    std::cerr << "[LogConf] Invalid 'file': expected string" << std::endl;
                    return chl::CL_ERR_INVALID_CONF;
                }
                LOGFILE = json_object_get_string(val);
            }
            else if(strcmp(key, "level") == 0)
            {
                if(!json_object_is_type(val, json_type_string))
                {
                    std::cerr << "[LogConf] Invalid 'level': expected string" << std::endl;
                    return chl::CL_ERR_INVALID_CONF;
                }
                parselogLevelConf(val, LOGLEVEL);
            }
            else if(strcmp(key, "name") == 0)
            {
                if(!json_object_is_type(val, json_type_string))
                {
                    std::cerr << "[LogConf] Invalid 'name': expected string" << std::endl;
                    return chl::CL_ERR_INVALID_CONF;
                }
                LOGNAME = json_object_get_string(val);
            }
            else if(strcmp(key, "filesize") == 0)
            {
                if(!json_object_is_type(val, json_type_int))
                {
                    std::cerr << "[LogConf] Invalid 'filesize': expected integer" << std::endl;
                    return chl::CL_ERR_INVALID_CONF;
                }
                LOGFILESIZE = json_object_get_int(val);
            }
            else if(strcmp(key, "filenum") == 0)
            {
                if(!json_object_is_type(val, json_type_int))
                {
                    std::cerr << "[LogConf] Invalid 'filenum': expected integer" << std::endl;
                    return chl::CL_ERR_INVALID_CONF;
                }
                LOGFILENUM = json_object_get_int(val);
            }
            else if(strcmp(key, "flushlevel") == 0)
            {
                if(!json_object_is_type(val, json_type_string))
                {
                    std::cerr << "[LogConf] Invalid 'flushlevel': expected string" << std::endl;
                    return chl::CL_ERR_INVALID_CONF;
                }
                parseFlushLevelConf(val, FLUSHLEVEL);
            }
            else
            {
                std::cerr << "[LogConf] Unknown log configuration: " << key << std::endl;
            }
        }
    }
    return chronolog::CL_SUCCESS;
}


int chronolog::DataStoreConf::parseJsonConf(json_object* data_store_json_conf)
{
    json_object_object_foreach(data_store_json_conf, key, val)
    {
        if(strcmp(key, "max_story_chunk_size") == 0)
        {
            if(!json_object_is_type(val, json_type_int))
            {
                std::cerr << "[DataStoreConf] Invalid 'max_story_chunk_size': expected integer" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            max_story_chunk_size = json_object_get_int(val);
        }
        else if(strcmp(key, "story_chunk_duration_secs") == 0)
        {
            if(!json_object_is_type(val, json_type_int))
            {
                std::cerr << "[DataStoreConf] Invalid 'story_chunk_duration_secs': expected integer" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            story_chunk_duration_secs = json_object_get_int(val);
        }
        else if(strcmp(key, "acceptance_window_secs") == 0)
        {
            if(!json_object_is_type(val, json_type_int))
            {
                std::cerr << "[DataStoreConf] Invalid 'acceptance_window_secs': expected integer" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            acceptance_window_secs = json_object_get_int(val);
        }
        else if(strcmp(key, "inactive_story_delay_secs") == 0)
        {
            if(!json_object_is_type(val, json_type_int))
            {
                std::cerr << "[DataStoreConf] Invalid 'inactive_story_delay_secs': expected integer" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            inactive_story_delay_secs = json_object_get_int(val);
        }
        else if(strcmp(key, "tail_capacity") == 0)
        {
            if(!json_object_is_type(val, json_type_int))
            {
                std::cerr << "[DataStoreConf] Invalid 'tail_capacity': expected integer" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            // Range-checked because the keeper widens this to std::size_t: a negative
            // value would wrap (-1 becomes SIZE_MAX), enforceCapacity would never
            // fire, and the tail would grow until the keeper is OOM-killed. 0 is
            // rejected too -- it evicts every event as soon as a chunk is ingested,
            // so every tail read returns empty with CL_SUCCESS and callers poll
            // forever with nothing to show for it.
            int const parsed_tail_capacity = json_object_get_int(val);
            if(parsed_tail_capacity <= 0)
            {
                std::cerr << "[DataStoreConf] Invalid 'tail_capacity': must be greater than 0, got "
                          << parsed_tail_capacity << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            tail_capacity = parsed_tail_capacity;
        }
        else if(strcmp(key, "live_tail_read") == 0)
        {
            if(!json_object_is_type(val, json_type_boolean))
            {
                std::cerr << "[DataStoreConf] Invalid 'live_tail_read': expected boolean" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            live_tail_read = json_object_get_boolean(val);
        }
        else if(strcmp(key, "tail_retention_secs") == 0)
        {
            if(!json_object_is_type(val, json_type_int))
            {
                std::cerr << "[DataStoreConf] Invalid 'tail_retention_secs': expected integer" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            // Range-checked because the keeper widens this to uint64_t and scales it
            // to nanoseconds: a negative value would wrap into a garbage retention
            // window. 0 is valid and documented -- it disables age-out, leaving
            // capacity eviction and the shutdown flush as the archival paths.
            int const parsed_tail_retention = json_object_get_int(val);
            if(parsed_tail_retention < 0)
            {
                std::cerr << "[DataStoreConf] Invalid 'tail_retention_secs': must not be negative, got "
                          << parsed_tail_retention << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            tail_retention_secs = parsed_tail_retention;
        }
        else
        {
            std::cerr << "[DataStoreConf] Unknown DataStoreInternals configuration: " << key << std::endl;
        }
    }

    // Cross-field check: these two knobs jointly decide how long a sealed chunk is
    // readable. A chunk only ENTERS the tail once it decays, at
    // end_time + acceptance_window, and LEAVES it at end_time + tail_retention --
    // so the readable window is the difference, not tail_retention itself. When
    // acceptance_window >= tail_retention a chunk is evicted on the same
    // maintenance tick that admits it and the sealed tail is permanently empty:
    // playback() then returns 0 events with CL_SUCCESS forever, which is
    // indistinguishable from a story that simply has nothing yet. Warn rather than
    // reject, since a deployment that never issues tail reads is unaffected.
    if(tail_retention_secs > 0 && acceptance_window_secs >= tail_retention_secs)
    {
        std::cerr << "[DataStoreConf] WARNING: tail_retention_secs (" << tail_retention_secs
                  << ") <= acceptance_window_secs (" << acceptance_window_secs
                  << "): sealed chunks are evicted as soon as they enter the tail, so playback() will always "
                     "return 0 events. Set tail_retention_secs above acceptance_window_secs by the tail depth "
                     "you want (readable window = tail_retention_secs - acceptance_window_secs)."
                  << std::endl;
    }

    return chronolog::CL_SUCCESS;
}

///////////////////
