#ifndef CHRONOLOG_CLIENT_CONFIGURATION_H
#define CHRONOLOG_CLIENT_CONFIGURATION_H

#include <spdlog/common.h>

namespace chronolog
{

typedef struct RPCProviderConf_
{
    std::string PROTO_CONF;
    std::string IP;
    uint16_t BASE_PORT;
    uint16_t SERVICE_PROVIDER_ID;

    [[nodiscard]] std::string to_String() const
    {
        return "[PROTO_CONF: " + PROTO_CONF + ", IP: " + IP + ", BASE_PORT: " + std::to_string(BASE_PORT) + 
            ", SERVICE_PROVIDER_ID: " + std::to_string(SERVICE_PROVIDER_ID) + "]";
    }
} RPCProviderConf;


typedef struct LogConf_
{
    std::string LOGTYPE;
    std::string LOGFILE;
    spdlog::level::level_enum LOGLEVEL;
    std::string LOGNAME;
    size_t LOGFILESIZE;
    size_t LOGFILENUM;
    spdlog::level::level_enum FLUSHLEVEL;

    // Helper function to convert spdlog::level::level_enum to string
    static std::string LevelToString(spdlog::level::level_enum level)
    {
        switch(level)
        {
            case spdlog::level::trace:
                return "TRACE";
            case spdlog::level::debug:
                return "DEBUG";
            case spdlog::level::info:
                return "INFO";
            case spdlog::level::warn:
                return "WARN";
            case spdlog::level::err:
                return "ERROR";
            case spdlog::level::critical:
                return "CRITICAL";
            case spdlog::level::off:
                return "OFF";
            default:
                return "UNKNOWN";
        }
    }

    [[nodiscard]] std::string to_string() const
    {
        return "[TYPE: " + LOGTYPE + ", FILE: " + LOGFILE + ", LEVEL: " + LevelToString(LOGLEVEL) + ", NAME: " +
               LOGNAME + ", LOGFILESIZE: " + std::to_string(LOGFILESIZE) + ", LOGFILENUM: " +
               std::to_string(LOGFILENUM) + ", FLUSH LEVEL: " + LevelToString(FLUSHLEVEL) + "]";
    }
} LogConf;

struct ClientConf
{
    RPCProviderConf VISOR_CLIENT_PORTAL_SERVICE_CONF;
    LogConf CLIENT_LOG_CONF;

    ClientConf()
    {
        VISOR_CLIENT_PORTAL_SERVICE_CONF.PROTO_CONF = "ofi+sockets";
        VISOR_CLIENT_PORTAL_SERVICE_CONF.IP = "127.0.0.1";
        VISOR_CLIENT_PORTAL_SERVICE_CONF.BASE_PORT = 5555;
        VISOR_CLIENT_PORTAL_SERVICE_CONF.SERVICE_PROVIDER_ID = 55;

        CLIENT_LOG_CONF.LOGTYPE = "file";
        CLIENT_LOG_CONF.LOGFILE = "chrono_client.log";
        CLIENT_LOG_CONF.LOGLEVEL = spdlog::level::info;
        CLIENT_LOG_CONF.LOGFILESIZE = 104857600;
        CLIENT_LOG_CONF.LOGFILENUM = 3;
        CLIENT_LOG_CONF.FLUSHLEVEL = spdlog::level::warn;
        
    };

    [[nodiscard]] std::string to_string() const
    {
        return "[VISOR_CLIENT_PORTAL_SERVICE_CONF: " + VISOR_CLIENT_PORTAL_SERVICE_CONF.to_String() +
               ", CLIENT_LOG_CONF:" + CLIENT_LOG_CONF.to_string() + "]";
    }
};

struct ClientPortalServiceConf   
{
    ClientPortalServiceConf( const std::string & protocol, const std::string & service_ip, uint16_t service_port, uint16_t service_provider_id)
        : proto_conf_(protocol)
        , ip_(service_ip)
        , port_(service_port)
        , provider_id_(service_provider_id)
        {}

    std::string const & proto_conf() const { return proto_conf_; }
    std::string const & ip() const { return ip_; }
    uint16_t port() const { return port_; }
    uint16_t provider_id() const { return provider_id_; }

    std::string proto_conf_;
    std::string ip_;
    uint16_t port_;
    uint16_t provider_id_;
};

}
#endif
