#include <chronolog_errcode.h>
#include <ConfigurationBlocks.h>

#include <ChronoVisorConfiguration.h>

namespace chl = chronolog;


int chronolog::VisorConfiguration::parseJsonConf(json_object* json_conf)
{
    if(json_conf == nullptr || !json_object_is_type(json_conf, json_type_object))
    {
        return chl::CL_ERR_INVALID_CONF;
    }

    json_object_object_foreach(json_conf, key, val)
    {
        if(strcmp(key, "VisorClientPortalService") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[VisorConfiguration] Invalid 'VisorClientPortalService': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* visor_client_portal_service_conf =
                    json_object_object_get(json_conf, "VisorClientPortalService");
            json_object_object_foreach(visor_client_portal_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    if(VISOR_CLIENT_PORTAL_SERVICE_CONF.parseJsonConf(val) != chl::CL_SUCCESS)
                        return chl::CL_ERR_INVALID_CONF;
                }
                else
                {
                    std::cerr << "[VisorConfiguration] [chrono_visor] Unknown VisorClientPortalService "
                                 "configuration: "
                              << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "VisorKeeperRegistryService") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[VisorConfiguration] Invalid 'VisorKeeperRegistryService': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* visor_keeper_registry_service_conf =
                    json_object_object_get(json_conf, "VisorKeeperRegistryService");
            json_object_object_foreach(visor_keeper_registry_service_conf, key, val)
            {
                if(strcmp(key, "rpc") == 0)
                {
                    if(VISOR_KEEPER_REGISTRY_SERVICE_CONF.parseJsonConf(val) != chl::CL_SUCCESS)
                        return chl::CL_ERR_INVALID_CONF;
                }
                else
                {
                    std::cerr << "[VisorConfiguration]  Unknown VisorKeeperRegistryService "
                                 "configuration: "
                              << key << std::endl;
                }
            }
        }
        else if(strcmp(key, "Monitoring") == 0)
        {
            if(!json_object_is_type(val, json_type_object))
            {
                std::cerr << "[VisorConfiguration] Invalid 'Monitoring': expected object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* chronovisor_log = json_object_object_get(json_conf, "Monitoring");
            if(VISOR_LOG_CONF.parseJsonConf(chronovisor_log) != chl::CL_SUCCESS)
                return chl::CL_ERR_INVALID_CONF;
        }
        else if(strcmp(key, "delayed_data_admin_exit_in_secs") == 0)
        {
            if(!json_object_is_type(val, json_type_int))
            {
                std::cerr << "[VisorConfiguration] Invalid 'delayed_data_admin_exit_in_secs': expected integer"
                          << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            int delayed_exit_value = json_object_get_int(val);
            DELAYED_DATA_ADMIN_EXIT_IN_SECS =
                    ((0 < delayed_exit_value && delayed_exit_value < 60) ? delayed_exit_value : 5);
        }
        else
        {
            std::cerr << "[VisorConfiguration] Unknown Visor configuration: " << key << std::endl;
        }
    }
    return chronolog::CL_SUCCESS;
}
