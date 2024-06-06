#ifndef CHRONOLOG_CLIENT_CONFIGURATION_H
#define CHRONOLOG_CLIENT_CONFIGURATION_H


namespace chronolog
{

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
