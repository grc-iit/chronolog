#include <ConfigurationManager.h>


int chronolog::ClockConf::parseJsonConf(json_object *clock_conf)
{
    json_object_object_foreach(clock_conf, key, val)
    {
        if(strcmp(key, "clocksource_type") == 0)
        {
            if(json_object_is_type(val, json_type_string))
            {
                const char *clocksource_type = json_object_get_string(val);
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
                std::cerr
                        << "[ClockConfiguration] Failed to parse configuration file: drift_cal_sleep_sec is not an integer"
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
                std::cerr
                        << "[ConfigurationManager] Failed to parse configuration file: drift_cal_sleep_nsec is not an integer"
                        << std::endl;
                return (chronolog::CL_ERR_INVALID_CONF);
            }
        }
    }
    return 1;
}

int chronolog::AuthConf::parseJsonConf(json_object *auth_conf)
{
    if(auth_conf == nullptr || !json_object_is_type(auth_conf, json_type_object))
    {
        std::cerr
                << "[AuthConfiguration] Error while parsing configuration file. Authentication configuration is not found or is not an object."
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
    return 1;
}

int chronolog::RPCProviderConf::parseJsonConf(json_object *json_conf)
{
    json_object_object_foreach(json_conf, key, val)
    {
        if(strcmp(key, "protocol_conf") == 0)
        {
            assert(json_object_is_type(val, json_type_string));
            PROTO_CONF = json_object_get_string(val);
        }
        else if(strcmp(key, "service_ip") == 0)
        {
            assert(json_object_is_type(val, json_type_string));
            IP = json_object_get_string(val);
        }
        else if(strcmp(key, "service_base_port") == 0)
        {
            assert(json_object_is_type(val, json_type_int));
            BASE_PORT = json_object_get_int(val);
        }
        else if(strcmp(key, "service_provider_id") == 0)
        {
            assert(json_object_is_type(val, json_type_int));
            SERVICE_PROVIDER_ID = json_object_get_int(val);
        }
        else
        {
            std::cerr << "[RPCProviderConf] Unknown client end configuration: " << key << std::endl;
        }
    }
    return 1;
}

int chronolog::LogConf::parseJsonConf(json_object *json_conf)
{

        if(!json_object_is_type(json_conf, json_type_object))
        {
            std::cerr
                    << "[LogConf] Logger configuration is not found or is not an object." << std::endl;
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
                    assert(json_object_is_type(val, json_type_string));
                    LOGTYPE = json_object_get_string(val);
                }
                else if(strcmp(key, "file") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    LOGFILE = json_object_get_string(val);
                }
                else if(strcmp(key, "level") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    parselogLevelConf(val, LOGLEVEL);
                }
                else if(strcmp(key, "name") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    LOGNAME = json_object_get_string(val);
                }
                else if(strcmp(key, "filesize") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    LOGFILESIZE = json_object_get_int(val);
                }
                else if(strcmp(key, "filenum") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    LOGFILENUM = json_object_get_int(val);
                }
                else if(strcmp(key, "flushlevel") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    parseFlushLevelConf(val, FLUSHLEVEL);
                }
                else
                {
                    std::cerr << "[LogConf] Unknown log configuration: " << key << std::endl;
                }
            }
    }
    return 1;
}

int chronolog::VisorConfiguration::parseJsonConf(json_object *json_conf)
{
    json_object_object_foreach(json_conf, key, val)
    {
        if(strcmp(key, "VisorClientPortalService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *visor_client_portal_service_conf = json_object_object_get(json_conf
                                                                                   , "VisorClientPortalService");
            json_object_object_foreach(visor_client_portal_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    VISOR_CLIENT_PORTAL_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[VisorConfiguration] [chrono_visor] Unknown VisorClientPortalService "
                                 "configuration: " << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "VisorKeeperRegistryService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *visor_keeper_registry_service_conf = json_object_object_get(json_conf
                                                                                     , "VisorKeeperRegistryService");
            json_object_object_foreach(visor_keeper_registry_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    VISOR_KEEPER_REGISTRY_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[VisorConfiguration]  Unknown VisorKeeperRegistryService "
                                 "configuration: " << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "Monitoring") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *chronovisor_log = json_object_object_get(json_conf, "Monitoring");
            VISOR_LOG_CONF.parseJsonConf(chronovisor_log);
        }
        else if(strcmp(key, "delayed_data_admin_exit_in_secs") == 0)
        {
            assert(json_object_is_type(val, json_type_int));
            int delayed_exit_value = json_object_get_int(val);
            DELAYED_DATA_ADMIN_EXIT_IN_SECS = ((0 < delayed_exit_value && delayed_exit_value < 60) ? delayed_exit_value
                                                                                                   : 5);
        }
        else
        {
            std::cerr << "[VisorConfiguration] Unknown Visor configuration: " << key << std::endl;
        }
    }
    return 1;
}

int chronolog::KeeperConfiguration::parseJsonConf(json_object *json_conf)
{
    json_object_object_foreach(json_conf, key, val)
    {
        if(strcmp(key, "RecordingGroup") == 0)
        {
            assert(json_object_is_type(val, json_type_int));
            int value = json_object_get_int(val);
            RECORDING_GROUP = (value >= 0 ? value : 0);
        }
        else if(strcmp(key, "KeeperRecordingService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *keeper_recording_service_conf = json_object_object_get(json_conf, "KeeperRecordingService");
            json_object_object_foreach(keeper_recording_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    KEEPER_RECORDING_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[KeeperConfiguration] Unknown KeeperRecordingService "
                                 "configuration: " << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "KeeperDataStoreAdminService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *keeper_data_store_admin_service_conf = json_object_object_get(json_conf
                                                                                       , "KeeperDataStoreAdminService");
            json_object_object_foreach(keeper_data_store_admin_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    DATA_STORE_ADMIN_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[KeeperConfiguration]  Unknown KeeperDataStoreAdminService "
                                 "configuration: " << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "VisorKeeperRegistryService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *visor_keeper_registry_service_conf = json_object_object_get(json_conf
                                                                                     , "VisorKeeperRegistryService");
            json_object_object_foreach(visor_keeper_registry_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    VISOR_REGISTRY_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[KeeperConfiguration] Unknown VisorKeeperRegistryService configuration: " << key
                              << std::endl;
                }
            }
        }
        else if(strcmp(key, "KeeperGrapherDrainService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *keeper_grapher_drain_service_conf = json_object_object_get(json_conf
                                                                                    , "KeeperGrapherDrainService");
            json_object_object_foreach(keeper_grapher_drain_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    KEEPER_GRAPHER_DRAIN_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[KeeperConfiguration] Unknown KeeperGrapherDrainService configuration: " << key
                              << std::endl;
                }
            }
        }
        else if(strcmp(key, "Monitoring") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *keeper_monitor = json_object_object_get(json_conf, "Monitoring");
            LOG_CONF.parseJsonConf(keeper_monitor);
            /* json_object_object_foreach(chronokeeper_log, key, val)
             {
                 if(strcmp(key, "monitor") == 0)
                 {
                     LOG_CONF.parseJsonConf(chronokeeper_log);
                 }
                 else
                 {
                     std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown Monitoring configuration: "
                               << key << std::endl;
                 }
             }*/
        }
        else if(strcmp(key, "DataStoreInternals") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *data_store_conf = json_object_object_get(json_conf, "DataStoreInternals");
            DATA_STORE_CONF.parseJsonConf(data_store_conf);
        }
        else if(strcmp(key, "Extractors") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *extractors = json_object_object_get(json_conf, "Extractors");
            json_object_object_foreach(extractors, key, val)
            {
                if(strcmp(key, "story_files_dir") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    EXTRACTOR_CONF.story_files_dir = json_object_get_string(val);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown Extractors configuration " << key
                              << std::endl;
                }
            }
        }
        else
        {
            std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown Keeper configuration: " << key << std::endl;
        }
    }
    return 1;
}

int chronolog::GrapherConfiguration::parseJsonConf(json_object *json_conf)
{
    json_object_object_foreach(json_conf, key, val)
    {
        if(strcmp(key, "RecordingGroup") == 0)
        {
            assert(json_object_is_type(val, json_type_int));
            int value = json_object_get_int(val);
            RECORDING_GROUP = (value >= 0 ? value : 0);
        }
        else if(strcmp(key, "KeeperGrapherDrainService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *keeper_grapher_drain_service_conf = json_object_object_get(json_conf
                                                                                    , "KeeperGrapherDrainService");
            json_object_object_foreach(keeper_grapher_drain_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    KEEPER_GRAPHER_DRAIN_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr
                            << "[GrapherConfiguration] Unknown KeeperGrapherDrainService configuration: "
                            << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "DataStoreAdminService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object* data_store_admin_service_conf = json_object_object_get(json_conf
                                                                                , "DataStoreAdminService");
            json_object_object_foreach(data_store_admin_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    DATA_STORE_ADMIN_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[GrapherConfiguration] Unknown DataStoreAdminService configuration: "
                              << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "VisorRegistryService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object*visor_keeper_registry_service_conf = json_object_object_get(json_conf, "VisorRegistryService");
            json_object_object_foreach(visor_keeper_registry_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    VISOR_REGISTRY_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[GrapherConfiguration] Unknown VisorRegistryService configuration: "
                              << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "Monitoring") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *grapher_monitor = json_object_object_get(json_conf, "Monitoring");
            LOG_CONF.parseJsonConf(grapher_monitor);
            /*
            json_object_object_foreach(chrono_logging, key, val)
            {
                if(strcmp(key, "monitor") == 0)
                {
                    LOG_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[GrapherConf] Unknown Monitoring configuration: " << key
                              << std::endl;
                }
            }*/
        }
        else if(strcmp(key, "DataStoreInternals") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object*data_store_conf = json_object_object_get(json_conf, "DataStoreInternals");
            DATA_STORE_CONF.parseJsonConf(data_store_conf);
        }
        else if(strcmp(key, "Extractors") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object*extractors = json_object_object_get(json_conf, "Extractors");
            json_object_object_foreach(extractors, key, val)
            {
                if(strcmp(key, "story_files_dir") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    EXTRACTOR_CONF.story_files_dir = json_object_get_string(val);
                }
                else
                {
                    std::cerr << "[GrapherConfiguration] Unknown Extractors configuration " << key
                              << std::endl;
                }
            }
        }
        else
        {
            std::cerr << "[GrapherConfiguration] Unknown Grapher configuration " << key << std::endl;
        }
    }
    return 1;
}

int chronolog::PlayerConfiguration::parseJsonConf(json_object *json_conf)
{
    json_object_object_foreach(json_conf, key, val)
    {
        if(strcmp(key, "RecordingGroup") == 0)
        {
            assert(json_object_is_type(val, json_type_int));
            int value = json_object_get_int(val);
            RECORDING_GROUP = (value >= 0 ? value : 0);
        }
        else if(strcmp(key, "PlayerStoreAdminService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object* data_store_admin_service_conf = json_object_object_get(json_conf
                                                                                , "PlayerStoreAdminService");
            json_object_object_foreach(data_store_admin_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    DATA_STORE_ADMIN_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_player] Unknown PlayerStoreAdminService configuration: "
                              << key << std::endl;
                    std::cerr
                            << "[ConfigurationManager] [chrono_player] Unknown PlayerStoreAdminService configuration: "
                            << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "PlaybackQueryService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object* data_store_admin_service_conf = json_object_object_get(json_conf
                                                                                , "PlaybackQueryService");
            json_object_object_foreach(data_store_admin_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    PLAYBACK_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_player] Unknown PlaybackQueryService configuration: "
                              << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "VisorRegistryService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *visor_keeper_registry_service_conf = json_object_object_get(json_conf, "VisorRegistryService");
            json_object_object_foreach(visor_keeper_registry_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    VISOR_REGISTRY_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_player] Unknown VisorRegistryService configuration: "
                              << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "Monitoring") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *player_monitor = json_object_object_get(json_conf, "Monitoring");
            LOG_CONF.parseJsonConf(player_monitor);
            /*json_object_object_foreach(chrono_logging, key, val)
            {
                if(strcmp(key, "monitor") == 0)
                {
                    LOG_CONF.parseJsonConf(palyer_monitor);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_player] Unknown Monitoring configuration: " << key
                              << std::endl;
                }
            }*/
        }
        else if(strcmp(key, "DataStoreInternals") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *data_store_conf = json_object_object_get(json_conf, "DataStoreInternals");
            DATA_STORE_CONF.parseJsonConf(data_store_conf);//, key, val)
        }
        else if(strcmp(key, "ArchiveReaders") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object *archive_readers = json_object_object_get(json_conf, "ArchiveReaders");
            json_object_object_foreach(archive_readers, key, val)
            {
                if(strcmp(key, "story_files_dir") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    READER_CONF.story_files_dir = json_object_get_string(val);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_player] Unknown ArchiveReaders configuration " << key
                              << std::endl;
                }
            }
        }
        else
        {
            std::cerr << "[ConfigurationManager][chrono_player] Unknown Player configuration " << key << std::endl;
        }
    }
    return 1;
}

int chronolog::DataStoreConf::parseJsonConf(json_object *data_store_json_conf)
{
    json_object_object_foreach(data_store_json_conf, key, val)
    {
        if(strcmp(key, "max_story_chunk_size") == 0)
        {
            assert(json_object_is_type(val, json_type_int));
            max_story_chunk_size = json_object_get_int(val);
        }
        else if(strcmp(key, "story_chunk_duration_secs") == 0)
        {
            assert(json_object_is_type(val, json_type_int));
            story_chunk_duration_secs = json_object_get_int(val);
        }
        else if(strcmp(key, "acceptance_window_secs") == 0)
        {
            assert(json_object_is_type(val, json_type_int));
            acceptance_window_secs = json_object_get_int(val);
        }
        else if(strcmp(key, "inactive_story_delay_secs") == 0)
        {
            assert(json_object_is_type(val, json_type_int));
            inactive_story_delay_secs = json_object_get_int(val);
        }
        else
        {
            std::cerr << "[DataStoreConf] Unknown DataStoreInternals configuration: " << key << std::endl;
        }
    }

    return 1;
}
