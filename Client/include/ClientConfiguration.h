#ifndef CHRONOLOG_CLIENT_CONFIGURATION_H
#define CHRONOLOG_CLIENT_CONFIGURATION_H

#include <string>
#include <spdlog/common.h>
#include <json-c/json.h>

namespace chronolog {

struct ClientPortalServiceConf {
    std::string PROTO_CONF = "ofi+sockets";
    std::string IP = "127.0.0.1";
    uint16_t PORT = 5555;
    uint16_t PROVIDER_ID = 55;
};

struct ClientQueryServiceConf {
    std::string PROTO_CONF = "ofi+sockets";
    std::string IP = "127.0.0.1";
    uint16_t PORT = 5557;
    uint16_t PROVIDER_ID = 57;
};

struct ClientLogConf {
    std::string LOGTYPE = "file";
    std::string LOGFILE = "chrono_client.log";
    spdlog::level::level_enum LOGLEVEL = spdlog::level::debug;
    std::string LOGNAME = "ChronoClient";
    size_t LOGFILESIZE = 1048576;
    size_t LOGFILENUM = 3;
    spdlog::level::level_enum FLUSHLEVEL = spdlog::level::warn;
};

class ClientConfiguration {
public:
    ClientPortalServiceConf PORTAL_CONF;
    ClientQueryServiceConf QUERY_CONF;
    ClientLogConf LOG_CONF;

    bool load_from_file(const std::string& path);
    void log_configuration() const;

private:
    void parse_rpc(json_object* rpc_obj, std::string& proto, std::string& ip, uint16_t& port, uint16_t& provider_id);
    void parse_log(json_object* log_obj);
    spdlog::level::level_enum parse_log_level(const std::string& level_str);
};

} // namespace chronolog

#endif // CHRONOLOG_CLIENT_CONFIGURATION_H