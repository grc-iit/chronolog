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
        else
        {
            std::cerr << "[DataStoreConf] Unknown DataStoreInternals configuration: " << key << std::endl;
        }
    }

    return chronolog::CL_SUCCESS;
}

///////////////////
