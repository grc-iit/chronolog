#include "ConfigurationManager.h"


void ChronoLog::ConfigurationManager::parseGrapherConf(json_object*json_conf)
{
    json_object_object_foreach(json_conf, key, val)
    {
        if(strcmp(key, "KeeperGrapherDrainService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object*keeper_grapher_drain_service_conf = json_object_object_get(json_conf
                                                                                   , "KeeperGrapherDrainService");
            json_object_object_foreach(keeper_grapher_drain_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    parseRPCProviderConf(val, GRAPHER_CONF.KEEPER_GRAPHER_DRAIN_SERVICE_CONF);
                }
                else
                {
                    std::cerr
                            << "[ConfigurationManager] [chrono_grapher] Unknown KeeperGrapherDrainService configuration: "
                            << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "DataStoreAdminService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object*keeper_data_store_admin_service_conf = json_object_object_get(json_conf
                                                                                      , "DataStoreAdminService");
            json_object_object_foreach(keeper_data_store_admin_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    parseRPCProviderConf(val, GRAPHER_CONF.DATA_STORE_ADMIN_SERVICE_CONF);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_grapher] Unknown DataStoreAdminService configuration: "
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
                    parseRPCProviderConf(val, GRAPHER_CONF.VISOR_REGISTRY_SERVICE_CONF);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_grapher] Unknown VisorRegistryService configuration: "
                              << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "Logging") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object*chrono_logging = json_object_object_get(json_conf, "Logging");
            json_object_object_foreach(chrono_logging, key, val)
            {
                if(strcmp(key, "log") == 0)
                {
                    parseLogConf(val, GRAPHER_CONF.LOG_CONF);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_grapher] Unknown Logging configuration: " << key
                              << std::endl;
                }
            }
        }
        else if(strcmp(key, "DataStoreInternals") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object*data_store_conf = json_object_object_get(json_conf, "DataStoreInternals");
            json_object_object_foreach(data_store_conf, key, val)
            {
                if(strcmp(key, "max_story_chunk_size") == 0)
                {
                    assert(json_object_is_type(val, json_type_int));
                    GRAPHER_CONF.DATA_STORE_CONF.max_story_chunk_size = json_object_get_int(val);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_grapher] Unknown configuration: " << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "Extractors") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object*extractors = json_object_object_get(json_conf, "Extractors");
            json_object_object_foreach(extractors, key, val)
            {
                if(strcmp(key, "story_files_dir") == 0)
                {
//                    std::cerr << "reading story_files_dir" << std::endl;
                    assert(json_object_is_type(val, json_type_string));
                    GRAPHER_CONF.EXTRACTOR_CONF.story_files_dir = json_object_get_string(val);
                }
                else
                {
                    std::cerr << "[ConfigurationManager] [chrono_grapher] Unknown extractor configuration " << key
                              << std::endl;
                }
            }
        }
        else
        {
            std::cerr << "[ConfigurationManager][chrono_grapher] Unknown configuration " << key << std::endl;
        }
    }
}
