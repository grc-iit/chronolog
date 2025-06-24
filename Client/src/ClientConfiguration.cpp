#include "ClientConfiguration.h"
#include <json-c/json.h>
#include <iostream>

namespace chronolog {

bool ClientConfiguration::load_from_file(const std::string& path) {
    json_object* root = json_object_from_file(path.c_str());
    if (!root) {
        std::cerr << "[ClientConfiguration] Failed to read config file: " << path << std::endl;
        return false;
    }

    json_object* chrono_client;
    if (!json_object_object_get_ex(root, "chrono_client", &chrono_client)) {
        std::cerr << "[ClientConfiguration] Missing 'chrono_client' section." << std::endl;
        return false;
    }

    json_object* portal_service;
    if (json_object_object_get_ex(chrono_client, "VisorClientPortalService", &portal_service)) {
        json_object* rpc;
        if (json_object_object_get_ex(portal_service, "rpc", &rpc)) {
            parse_rpc(rpc, PORTAL_CONF.PROTO_CONF, PORTAL_CONF.IP, PORTAL_CONF.PORT, PORTAL_CONF.PROVIDER_ID);
        }
    }

    json_object* query_service;
    if (json_object_object_get_ex(chrono_client, "ClientQueryService", &query_service)) {
        json_object* rpc;
        if (json_object_object_get_ex(query_service, "rpc", &rpc)) {
            parse_rpc(rpc, QUERY_CONF.PROTO_CONF, QUERY_CONF.IP, QUERY_CONF.PORT, QUERY_CONF.PROVIDER_ID);
        }
    }

    json_object* monitoring;
    if (json_object_object_get_ex(chrono_client, "Monitoring", &monitoring)) {
        json_object* monitor;
        if (json_object_object_get_ex(monitoring, "monitor", &monitor)) {
            parse_log(monitor);
        }
    }

    json_object_put(root);
    return true;
}

void ClientConfiguration::parse_rpc(json_object* rpc_obj, std::string& proto, std::string& ip, uint16_t& port, uint16_t& provider_id) {
    json_object* value;

    if (json_object_object_get_ex(rpc_obj, "protocol_conf", &value))
        proto = json_object_get_string(value);

    if (json_object_object_get_ex(rpc_obj, "service_ip", &value))
        ip = json_object_get_string(value);

    if (json_object_object_get_ex(rpc_obj, "service_base_port", &value))
        port = static_cast<uint16_t>(json_object_get_int(value));

    if (json_object_object_get_ex(rpc_obj, "service_provider_id", &value))
        provider_id = static_cast<uint16_t>(json_object_get_int(value));
}

void ClientConfiguration::parse_log(json_object* log_obj) {
    json_object* value;

    if (json_object_object_get_ex(log_obj, "type", &value))
        LOG_CONF.LOGTYPE = json_object_get_string(value);

    if (json_object_object_get_ex(log_obj, "file", &value))
        LOG_CONF.LOGFILE = json_object_get_string(value);

    if (json_object_object_get_ex(log_obj, "level", &value))
        LOG_CONF.LOGLEVEL = parse_log_level(json_object_get_string(value));

    if (json_object_object_get_ex(log_obj, "name", &value))
        LOG_CONF.LOGNAME = json_object_get_string(value);

    if (json_object_object_get_ex(log_obj, "filesize", &value))
        LOG_CONF.LOGFILESIZE = static_cast<size_t>(json_object_get_int(value));

    if (json_object_object_get_ex(log_obj, "filenum", &value))
        LOG_CONF.LOGFILENUM = static_cast<size_t>(json_object_get_int(value));

    if (json_object_object_get_ex(log_obj, "flushlevel", &value))
        LOG_CONF.FLUSHLEVEL = parse_log_level(json_object_get_string(value));
}

spdlog::level::level_enum ClientConfiguration::parse_log_level(const std::string& level_str) {
    if (level_str == "trace") return spdlog::level::trace;
    if (level_str == "debug") return spdlog::level::debug;
    if (level_str == "info") return spdlog::level::info;
    if (level_str == "warning") return spdlog::level::warn;
    if (level_str == "error") return spdlog::level::err;
    if (level_str == "critical") return spdlog::level::critical;
    if (level_str == "off") return spdlog::level::off;
    return spdlog::level::warn;
}

void ClientConfiguration::log_configuration() const {
    std::cout << "===== ChronoLog Client Configuration =====" << std::endl;

    std::cout << "[PORTAL_CONF]" << std::endl;
    std::cout << "  protocol: " << PORTAL_CONF.PROTO_CONF << std::endl;
    std::cout << "  IP: " << PORTAL_CONF.IP << std::endl;
    std::cout << "  port: " << PORTAL_CONF.PORT << std::endl;
    std::cout << "  provider ID: " << PORTAL_CONF.PROVIDER_ID << std::endl;

    std::cout << "[QUERY_CONF]" << std::endl;
    std::cout << "  protocol: " << QUERY_CONF.PROTO_CONF << std::endl;
    std::cout << "  IP: " << QUERY_CONF.IP << std::endl;
    std::cout << "  port: " << QUERY_CONF.PORT << std::endl;
    std::cout << "  provider ID: " << QUERY_CONF.PROVIDER_ID << std::endl;

    std::cout << "[LOG_CONF]" << std::endl;
    std::cout << "  type: " << LOG_CONF.LOGTYPE << std::endl;
    std::cout << "  file: " << LOG_CONF.LOGFILE << std::endl;
    std::cout << "  level: " << spdlog::level::to_string_view(LOG_CONF.LOGLEVEL).data() << std::endl;
    std::cout << "  name: " << LOG_CONF.LOGNAME << std::endl;
    std::cout << "  filesize: " << LOG_CONF.LOGFILESIZE << std::endl;
    std::cout << "  filenum: " << LOG_CONF.LOGFILENUM << std::endl;
    std::cout << "  flushlevel: " << spdlog::level::to_string_view(LOG_CONF.FLUSHLEVEL).data() << std::endl;
}

}