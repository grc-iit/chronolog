#ifndef CHRONOLOG_CONFIGURATIONMANAGER_H
#define CHRONOLOG_CONFIGURATIONMANAGER_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <json-c/json.h>

#include <chronolog_errcode.h>
#include <ConfigurationBlocks.h>
#include "log_level.h"

namespace chronolog
{

class ConfigurationManager
{
public:
    ClockConf CLOCK_CONF;
    AuthConf AUTH_CONF; 
    json_object * VISOR_JSON_CONF;
    json_object * KEEPER_JSON_CONF;
    json_object * GRAPHER_JSON_CONF;
    json_object * PLAYER_JSON_CONF;

    ConfigurationManager()
        : CLOCK_CONF{}
        , AUTH_CONF{}
        , VISOR_JSON_CONF{nullptr}
        , KEEPER_JSON_CONF{nullptr}
        , GRAPHER_JSON_CONF{nullptr}
        , PLAYER_JSON_CONF{nullptr}
    {}

    explicit ConfigurationManager(const std::string& conf_file_path) { LoadConfFromJSONFile(conf_file_path); }

    void LoadConfFromJSONFile(const std::string& conf_file_path)
    {
        json_object* root = json_object_from_file(conf_file_path.c_str());
        if(root == nullptr)
        {
            std::cerr << "[ConfigurationManager] Failed to open configuration file at path: " << conf_file_path.c_str()
                      << ". Exiting..." << std::endl;
            exit(chronolog::CL_ERR_UNKNOWN);
        }

        json_object_object_foreach(root, key, val)
        {
            if(strcmp(key, "clock") == 0)
            {
                json_object* clock_conf = json_object_object_get(root, "clock");
                if(clock_conf == nullptr || !json_object_is_type(clock_conf, json_type_object))
                {
                    std::cerr << "[ConfigurationManager] Error while parsing configuration file "
                              << conf_file_path.c_str() << ". Clock configuration is not found or is not an object."
                              << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
                CLOCK_CONF.parseJsonConf(clock_conf);
            }
            else if(strcmp(key, "authentication") == 0)
            {
                json_object* auth_conf = json_object_object_get(root, "authentication");
                if(auth_conf == nullptr || !json_object_is_type(auth_conf, json_type_object))
                {
                    std::cerr << "[ConfigurationManager] Error while parsing configuration file "
                              << conf_file_path.c_str()
                              << ". Authentication configuration is not found or is not an object." << std::endl;
                    exit(chronolog::CL_ERR_INVALID_CONF);
                }
                AUTH_CONF.parseJsonConf(auth_conf);
            }
            else if(strcmp(key, "chrono_visor") == 0)
            {
                json_object* chrono_visor_conf = json_object_object_get(root, "chrono_visor");
            
                if(chrono_visor_conf != nullptr || json_object_is_type(chrono_visor_conf, json_type_object))
                {
                    VISOR_JSON_CONF = chrono_visor_conf;
                }
            }
            else if(strcmp(key, "chrono_keeper") == 0)
            {
                json_object* chrono_keeper_conf = json_object_object_get(root, "chrono_keeper");
                if(chrono_keeper_conf != nullptr || json_object_is_type(chrono_keeper_conf, json_type_object))
                {
                    KEEPER_JSON_CONF = chrono_keeper_conf;
                }
            }
            else if(strcmp(key, "chrono_grapher") == 0)
            {
                json_object* chrono_grapher_conf = json_object_object_get(root, "chrono_grapher");
                if(chrono_grapher_conf != nullptr || json_object_is_type(chrono_grapher_conf, json_type_object))
                {
                    GRAPHER_JSON_CONF = chrono_grapher_conf;
                }
            }
            else if(strcmp(key, "chrono_player") == 0)
            {
                json_object* chrono_player_conf = json_object_object_get(root, "chrono_player");
                if(chrono_player_conf != nullptr || json_object_is_type(chrono_player_conf, json_type_object))
                {
                    PLAYER_JSON_CONF = chrono_player_conf;
                }
            }
        }
        json_object_put(root);
    }
};
} // namespace chronolog

#endif //CHRONOLOG_CONFIGURATIONMANAGER_H
