#include <chronolog_errcode.h>
#include <ConfigurationBlocks.h>
#include <ChronoGrapherConfiguration.h>
#include <ExtractionModuleConfiguration.h>

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
            if(!json_object_is_type(val, json_type_int))
            {
                std::cerr << "[GrapherConfiguration] Invalid 'RecordingGroup': expected integer" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            int value = json_object_get_int(val);
            RECORDING_GROUP = (value >= 0 ? value : 0);
        }
        else if(strcmp(key, "KeeperGrapherDrainService") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[GrapherConfiguration] Invalid 'KeeperGrapherDrainService': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* keeper_grapher_drain_service_conf =
                    json_object_object_get(json_conf, "KeeperGrapherDrainService");
            json_object_object_foreach(keeper_grapher_drain_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    if(KEEPER_GRAPHER_DRAIN_SERVICE_CONF.parseJsonConf(val) != chl::CL_SUCCESS)
                        return chl::CL_ERR_INVALID_CONF;
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
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[GrapherConfiguration] Invalid 'DataStoreAdminService': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* data_store_admin_service_conf = json_object_object_get(json_conf, "DataStoreAdminService");
            json_object_object_foreach(data_store_admin_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    if(DATA_STORE_ADMIN_SERVICE_CONF.parseJsonConf(val) != chl::CL_SUCCESS)
                        return chl::CL_ERR_INVALID_CONF;
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
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[GrapherConfiguration] Invalid 'VisorRegistryService': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* visor_keeper_registry_service_conf = json_object_object_get(json_conf, "VisorRegistryService");
            json_object_object_foreach(visor_keeper_registry_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    if(VISOR_REGISTRY_SERVICE_CONF.parseJsonConf(val) != chl::CL_SUCCESS)
                        return chl::CL_ERR_INVALID_CONF;
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
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[GrapherConfiguration] Invalid 'Monitoring': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* grapher_monitor = json_object_object_get(json_conf, "Monitoring");
            if(LOG_CONF.parseJsonConf(grapher_monitor) != chl::CL_SUCCESS)
                return chl::CL_ERR_INVALID_CONF;
        }
        else if(strcmp(key, "DataStoreInternals") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[GrapherConfiguration] Invalid 'DataStoreInternals': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* data_store_conf = json_object_object_get(json_conf, "DataStoreInternals");
            if(DATA_STORE_CONF.parseJsonConf(data_store_conf) != chl::CL_SUCCESS)
                return chl::CL_ERR_INVALID_CONF;
        }
        else if(strcmp(key, "ExtractionModule") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[GrapherConfiguration] Invalid 'ExtractionModule' segment: expected json object"
                          << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* extraction_module_json_object = json_object_object_get(json_conf, "ExtractionModule");
            if(EXTRACTION_MODULE_CONF.parse_json_object(extraction_module_json_object) != chl::CL_SUCCESS)
            {
                return chl::CL_ERR_INVALID_CONF;
                std::cerr << "[GrapherConfiguration] Error parsing ExtractionModule  configuration: " << std::endl;
            }
        }

        else
        {
            std::cerr << "[GrapherConfiguration] Unknown Grapher configuration " << key << std::endl;
        }
    }
    return chronolog::CL_SUCCESS;
}
