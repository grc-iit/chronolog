#include <chronolog_errcode.h>
#include <ConfigurationBlocks.h>
#include <ChronoKeeperConfiguration.h>

namespace chl = chronolog;

int chronolog::KeeperConfiguration::parseJsonConf(json_object* json_conf)
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
                std::cerr << "[KeeperConfiguration] Invalid 'RecordingGroup': expected integer" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            int value = json_object_get_int(val);
            RECORDING_GROUP = (value >= 0 ? value : 0);
        }
        else if(strcmp(key, "KeeperRecordingService") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[KeeperConfiguration] Invalid 'KeeperRecordingService': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* keeper_recording_service_conf = json_object_object_get(json_conf, "KeeperRecordingService");
            json_object_object_foreach(keeper_recording_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    if(KEEPER_RECORDING_SERVICE_CONF.parseJsonConf(val) != chl::CL_SUCCESS)
                        return chl::CL_ERR_INVALID_CONF;
                }
                else
                {
                    std::cerr << "[KeeperConfiguration] Unknown KeeperRecordingService "
                                 "configuration: "
                              << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "KeeperDataStoreAdminService") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[KeeperConfiguration] Invalid 'KeeperDataStoreAdminService': expected object"
                          << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* keeper_data_store_admin_service_conf =
                    json_object_object_get(json_conf, "KeeperDataStoreAdminService");
            json_object_object_foreach(keeper_data_store_admin_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    if(DATA_STORE_ADMIN_SERVICE_CONF.parseJsonConf(val) != chl::CL_SUCCESS)
                        return chl::CL_ERR_INVALID_CONF;
                }
                else
                {
                    std::cerr << "[KeeperConfiguration]  Unknown KeeperDataStoreAdminService "
                                 "configuration: "
                              << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "VisorKeeperRegistryService") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[KeeperConfiguration] Invalid 'VisorKeeperRegistryService': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* visor_keeper_registry_service_conf =
                    json_object_object_get(json_conf, "VisorKeeperRegistryService");
            json_object_object_foreach(visor_keeper_registry_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    if(VISOR_REGISTRY_SERVICE_CONF.parseJsonConf(val) != chl::CL_SUCCESS)
                        return chl::CL_ERR_INVALID_CONF;
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
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[KeeperConfiguration] Invalid 'KeeperGrapherDrainService': expected object" << std::endl;
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
                    std::cerr << "[KeeperConfiguration] Unknown KeeperGrapherDrainService configuration: " << key
                              << std::endl;
                }
            }
        }
        else if(strcmp(key, "Monitoring") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[KeeperConfiguration] Invalid 'Monitoring': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* keeper_monitor = json_object_object_get(json_conf, "Monitoring");
            if(LOG_CONF.parseJsonConf(keeper_monitor) != chl::CL_SUCCESS)
                return chl::CL_ERR_INVALID_CONF;
        }
        else if(strcmp(key, "DataStoreInternals") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[KeeperConfiguration] Invalid 'DataStoreInternals': expected object" << std::endl;
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
                std::cerr << "[KeeperConfiguration] Invalid 'ExtractionModule' segment: expected json object"
                          << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* extraction_module_json_object = json_object_object_get(json_conf, "ExtractionModule");
            if(EXTRACTION_MODULE_CONF.parse_json_object(extraction_module_json_object) != chl::CL_SUCCESS)
            {
                return chl::CL_ERR_INVALID_CONF;
                std::cerr << "[KeeperConfiguration] Error parsing ExtractionModule  configuration: " << std::endl;
            }
        }
        else
        {
            std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown Keeper configuration block: " << key
                      << std::endl;
        }
    }
    return chronolog::CL_SUCCESS;
}
