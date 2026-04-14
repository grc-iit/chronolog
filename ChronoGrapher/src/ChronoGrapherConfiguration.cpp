#include <chronolog_errcode.h>
#include <ConfigurationBlocks.h>
#include <ChronoGrapherConfiguration.h>


namespace chl = chronolog;

int chronolog::GrapherConfiguration::parseJsonConf(json_object* json_conf)
{
    if(json_conf == nullptr || !json_object_is_type(json_conf, json_type_object))
    {
        return chl::CL_ERR_INVALID_CONF;
    }

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
            json_object* keeper_grapher_drain_service_conf =
                    json_object_object_get(json_conf, "KeeperGrapherDrainService");
            json_object_object_foreach(keeper_grapher_drain_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    KEEPER_GRAPHER_DRAIN_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[GrapherConfiguration] Unknown KeeperGrapherDrainService configuration: " << key
                              << std::endl;
                }
            }
        }
        else if(strcmp(key, "DataStoreAdminService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object* data_store_admin_service_conf = json_object_object_get(json_conf, "DataStoreAdminService");
            json_object_object_foreach(data_store_admin_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    DATA_STORE_ADMIN_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[GrapherConfiguration] Unknown DataStoreAdminService configuration: " << key
                              << std::endl;
                }
            }
        }
        else if(strcmp(key, "VisorRegistryService") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object* visor_keeper_registry_service_conf = json_object_object_get(json_conf, "VisorRegistryService");
            json_object_object_foreach(visor_keeper_registry_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    VISOR_REGISTRY_SERVICE_CONF.parseJsonConf(val);
                }
                else
                {
                    std::cerr << "[GrapherConfiguration] Unknown VisorRegistryService configuration: " << key
                              << std::endl;
                }
            }
        }
        else if(strcmp(key, "Monitoring") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object* grapher_monitor = json_object_object_get(json_conf, "Monitoring");
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
            json_object* data_store_conf = json_object_object_get(json_conf, "DataStoreInternals");
            DATA_STORE_CONF.parseJsonConf(data_store_conf);
        }
        else if(strcmp(key, "Extractors") == 0)
        {
            assert(json_object_is_type(val, json_type_object));
            json_object* extractors = json_object_object_get(json_conf, "Extractors");
            json_object_object_foreach(extractors, key, val)
            {
                if(strcmp(key, "story_files_dir") == 0)
                {
                    assert(json_object_is_type(val, json_type_string));
                    EXTRACTOR_CONF.story_files_dir = json_object_get_string(val);
                }
                else
                {
                    std::cerr << "[GrapherConfiguration] Unknown Extractors configuration " << key << std::endl;
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
