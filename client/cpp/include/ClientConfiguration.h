#ifndef CHRONOLOG_CLIENT_CONFIGURATION_H
#define CHRONOLOG_CLIENT_CONFIGURATION_H

#include <ostream>
#include <string>

#include <json-c/json.h>

#include "log_level.h"
#include "RpcPolicy.h"

namespace chronolog
{

// Parse one RpcPolicy object atomically. Returns CL_ERR_INVALID_ARG on invalid input.
inline int parseRpcTimeoutPolicy(json_object* object, RpcTimeoutPolicy& policy) noexcept
{
    if(!object || !json_object_is_type(object, json_type_object))
        return CL_ERR_INVALID_ARG;

    RpcTimeoutPolicy candidate;
    struct Field
    {
        char const* name;
        uint64_t RpcTimeoutPolicy::*member;
    };
    constexpr Field fields[] = {{"append_ms", &RpcTimeoutPolicy::append_ms},
                                {"control_ms", &RpcTimeoutPolicy::control_ms},
                                {"tail_ms", &RpcTimeoutPolicy::tail_ms},
                                {"replay_ms", &RpcTimeoutPolicy::replay_ms},
                                {"attempt_ms", &RpcTimeoutPolicy::attempt_ms},
                                {"startup_ms", &RpcTimeoutPolicy::startup_ms},
                                {"shutdown_ms", &RpcTimeoutPolicy::shutdown_ms},
                                {"transfer_ms", &RpcTimeoutPolicy::transfer_ms}};
    for(auto const& field: fields)
    {
        json_object* value = nullptr;
        if(json_object_object_get_ex(object, field.name, &value))
        {
            if(!value || !json_object_is_type(value, json_type_int) || json_object_get_int64(value) <= 0)
                return CL_ERR_INVALID_ARG;
            candidate.*(field.member) = json_object_get_uint64(value);
        }
    }
    if(validateRpcTimeoutPolicy(candidate) != CL_SUCCESS)
        return CL_ERR_INVALID_ARG;
    policy = candidate;
    return CL_SUCCESS;
}

struct ClientPortalServiceConf
{
    std::string PROTO_CONF = "ofi+sockets";
    std::string IP = "127.0.0.1";
    uint16_t PORT = 5555;
    uint16_t PROVIDER_ID = 55;
    RpcTimeoutPolicy RPC_POLICY{};
};

struct ClientQueryServiceConf
{
    std::string PROTO_CONF = "ofi+sockets";
    std::string IP = "127.0.0.1";
    uint16_t PORT = 5557;
    uint16_t PROVIDER_ID = 57;
};

struct ClientLogConf
{
    std::string LOGTYPE = "file";
    std::string LOGFILE = "chrono_client.log";
    LogLevel LOGLEVEL = LogLevel::debug;
    std::string LOGNAME = "ChronoClient";
    size_t LOGFILESIZE = 1048576;
    size_t LOGFILENUM = 3;
    LogLevel FLUSHLEVEL = LogLevel::warn;
};

class ClientConfiguration
{
public:
    ClientPortalServiceConf PORTAL_CONF;
    ClientQueryServiceConf QUERY_CONF;
    ClientLogConf LOG_CONF;

    bool load_from_file(const std::string& path);
    std::ostream& log_configuration(std::ostream& out) const;

private:
    void parse_rpc(json_object* rpc_obj, std::string& proto, std::string& ip, uint16_t& port, uint16_t& provider_id);
    void parse_log(json_object* log_obj);
    LogLevel parse_log_level(const std::string& level_str);
};

} // namespace chronolog

#endif // CHRONOLOG_CLIENT_CONFIGURATION_H
