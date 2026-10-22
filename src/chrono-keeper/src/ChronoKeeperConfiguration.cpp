#include <cstdint>
#include <iostream>

#include <chronolog_errcode.h>
#include <ConfigurationBlocks.h>
#include <ChronoKeeperConfiguration.h>

namespace chl = chronolog;

namespace
{
// Reads an integer in [min_value, max_value] into an unsigned field.
bool readBounded(json_object* val, char const* name, int64_t min_value, int64_t max_value, uint64_t& out)
{
    if(!json_object_is_type(val, json_type_int))
    {
        std::cerr << "[WalConf] Invalid '" << name << "': expected integer" << std::endl;
        return false;
    }
    int64_t const value = json_object_get_int64(val);
    if(value < min_value || value > max_value)
    {
        std::cerr << "[WalConf] Invalid '" << name << "': must be between " << min_value << " and " << max_value
                  << ", got " << value << std::endl;
        return false;
    }
    out = static_cast<uint64_t>(value);
    return true;
}
} // namespace

int chronolog::WalConf::parseJsonConf(json_object* wal_json_conf)
{
    if(wal_json_conf == nullptr || !json_object_is_type(wal_json_conf, json_type_object))
    {
        std::cerr << "[WalConf] Invalid 'WAL': expected object" << std::endl;
        return chl::CL_ERR_INVALID_CONF;
    }

    constexpr int64_t max_int64 = INT64_MAX;
    uint64_t parsed = 0;

    json_object_object_foreach(wal_json_conf, key, val)
    {
        if(strcmp(key, "enabled") == 0)
        {
            if(!json_object_is_type(val, json_type_boolean))
            {
                std::cerr << "[WalConf] Invalid 'enabled': expected boolean" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            enabled = json_object_get_boolean(val);
        }
        else if(strcmp(key, "dir") == 0)
        {
            if(!json_object_is_type(val, json_type_string))
            {
                std::cerr << "[WalConf] Invalid 'dir': expected string" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            dir = json_object_get_string(val);
        }
        else if(strcmp(key, "segment_bytes") == 0)
        {
            if(!readBounded(val, key, 1, max_int64, parsed))
                return chl::CL_ERR_INVALID_CONF;
            segment_bytes = parsed;
        }
        else if(strcmp(key, "max_bytes") == 0)
        {
            if(!readBounded(val, key, 1, max_int64, parsed))
                return chl::CL_ERR_INVALID_CONF;
            max_bytes = parsed;
        }
        else if(strcmp(key, "group_commit_max_bytes") == 0)
        {
            if(!readBounded(val, key, 1, max_int64, parsed))
                return chl::CL_ERR_INVALID_CONF;
            group_commit_max_bytes = parsed;
        }
        else if(strcmp(key, "group_commit_window_us") == 0)
        {
            if(!readBounded(val, key, 0, MAX_GROUP_COMMIT_WINDOW_US, parsed))
                return chl::CL_ERR_INVALID_CONF;
            group_commit_window_us = static_cast<uint32_t>(parsed);
        }
        else if(strcmp(key, "reserve_min_bytes") == 0)
        {
            if(!readBounded(val, key, 1, max_int64, parsed))
                return chl::CL_ERR_INVALID_CONF;
            reserve_min_bytes = parsed;
        }
        else if(strcmp(key, "reserve_percent") == 0)
        {
            if(!readBounded(val, key, 0, 100, parsed))
                return chl::CL_ERR_INVALID_CONF;
            reserve_percent = static_cast<uint32_t>(parsed);
        }
        else if(strcmp(key, "durable_cap_mb") == 0)
        {
            if(!readBounded(val, key, 1, max_int64, parsed))
                return chl::CL_ERR_INVALID_CONF;
            durable_cap_mb = parsed;
        }
        else
        {
            std::cerr << "[WalConf] Unknown WAL configuration: " << key << std::endl;
        }
    }

    // Checked after the loop so the order of the keys does not matter.
    if(segment_bytes < MIN_SEGMENT_BYTES)
    {
        std::cerr << "[WalConf] Invalid 'segment_bytes': must be at least " << MIN_SEGMENT_BYTES << ", got "
                  << segment_bytes << std::endl;
        return chl::CL_ERR_INVALID_CONF;
    }
    if(enabled && (dir.empty() || dir[0] != '/'))
    {
        std::cerr << "[WalConf] Invalid 'dir': an enabled WAL needs an absolute path, got '" << dir << "'" << std::endl;
        return chl::CL_ERR_INVALID_CONF;
    }
    return chl::CL_SUCCESS;
}

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
                    if(KEEPER_RECORDING_SERVICE_CONF.RPC_CONF.parseJsonConf(val) != chl::CL_SUCCESS)
                        return chl::CL_ERR_INVALID_CONF;
                }
                else if(strcmp(key, "IngestionThreadCount") == 0)
                {
                    if(!json_object_is_type(val, json_type_int))
                    {
                        std::cerr << "[KeeperConfiguration] Invalid 'IngestionThreadCount': expected integer"
                                  << std::endl;
                        return chl::CL_ERR_INVALID_CONF;
                    }
                    int value = json_object_get_int(val);
                    KEEPER_RECORDING_SERVICE_CONF.INGESTION_THREAD_COUNT = (value >= 1 ? value : 1);
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
        else if(strcmp(key, "WAL") == 0)
        {
            if(WAL_CONF.parseJsonConf(val) != chl::CL_SUCCESS)
                return chl::CL_ERR_INVALID_CONF;
        }
        else
        {
            std::cerr << "[ConfigurationManager] [chrono_keeper] Unknown Keeper configuration block: " << key
                      << std::endl;
        }
    }
    return chronolog::CL_SUCCESS;
}
