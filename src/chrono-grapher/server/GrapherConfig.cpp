#include "chrono-grapher/server/GrapherConfig.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

namespace chronolog::grapher
{
absl::StatusOr<GrapherConfig> GrapherConfig::load(const std::optional<std::string>& path, bool allow_bind_all)
{
    using Json = nlohmann::json;
    Json input = Json::object();
    if(path)
    {
        std::ifstream file(*path);
        if(!file)
            return absl::NotFoundError("cannot open grapher configuration");
        input = Json::parse(file, nullptr, false);
        if(!input.is_object())
            return absl::InvalidArgumentError("grapher configuration must be a JSON object");
    }
    GrapherConfig config;
    std::map<std::string, std::string*> strings = {{"process_id", &config.process_id},
                                                   {"manifest_writer", &config.manifest_writer},
                                                   {"internal_listen", &config.internal_listen},
                                                   {"self_endpoint", &config.self_endpoint},
                                                   {"visor_internal", &config.visor_internal},
                                                   {"archive_codec", &config.archive_codec},
                                                   {"log_level", &config.log_level},
                                                   {"archive_root", &config.archive_root}};
    std::map<std::string, uint64_t> numbers = {{"heartbeat_interval_ms", config.heartbeat_interval_ms},
                                               {"rpc_timeout_ms", config.rpc_timeout_ms},
                                               {"drain_timeout_ms", config.drain_timeout_ms},
                                               {"max_chunk_bytes", config.limits.chunk_bytes},
                                               {"max_frame_bytes", config.limits.frame_bytes},
                                               {"concurrent_transfers", config.limits.concurrent_transfers}};
    for(const auto& [key, value]: input.items())
        if(!strings.contains(key) && !numbers.contains(key) && key != "insecure_bind_all")
            return absl::InvalidArgumentError("unknown grapher configuration key " + key);
    auto env = [](std::string key) -> const char*
    {
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::toupper(c); });
        return std::getenv(("CHRONOLOG_GRAPHER_" + key).c_str());
    };
    try
    {
        for(const auto& [key, field]: strings)
        {
            if(input.contains(key))
                *field = input.at(key).get<std::string>();
            if(const auto* value = env(key))
                *field = value;
        }
        for(auto& [key, number]: numbers)
        {
            if(input.contains(key))
            {
                if(!input.at(key).is_number_integer() || (input.at(key).is_number_integer() && input.at(key) < 0))
                    return absl::InvalidArgumentError("invalid number for " + key);
                number = input.at(key).get<uint64_t>();
            }
            if(const auto* value = env(key))
            {
                const std::string text(value);
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
                if(parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                    return absl::InvalidArgumentError("invalid environment number for " + key);
            }
            if(number == 0 || number > UINT32_MAX)
                return absl::InvalidArgumentError("number out of range for " + key);
        }
        if(input.contains("insecure_bind_all"))
            config.insecure_bind_all = input.at("insecure_bind_all").get<bool>();
        if(const auto* value = env("insecure_bind_all"))
        {
            const std::string text(value);
            if(text != "true" && text != "false" && text != "1" && text != "0")
                return absl::InvalidArgumentError("invalid insecure_bind_all boolean");
            config.insecure_bind_all = text == "true" || text == "1";
        }
    }
    catch(const Json::exception& error)
    {
        return absl::InvalidArgumentError(error.what());
    }
    config.heartbeat_interval_ms = numbers.at("heartbeat_interval_ms");
    config.rpc_timeout_ms = numbers.at("rpc_timeout_ms");
    config.drain_timeout_ms = numbers.at("drain_timeout_ms");
    config.limits.chunk_bytes = numbers.at("max_chunk_bytes");
    config.limits.frame_bytes = numbers.at("max_frame_bytes");
    config.limits.concurrent_transfers = numbers.at("concurrent_transfers");
    config.insecure_bind_all = config.insecure_bind_all || allow_bind_all;
    if(config.manifest_writer.empty())
        config.manifest_writer = config.process_id;
    const auto valid = config.validate();
    if(!valid.ok())
        return valid;
    return config;
}

absl::Status GrapherConfig::validate() const
{
    if(archive_codec != "proto" && archive_codec != "hdf5")
        return absl::InvalidArgumentError("archive_codec must be proto or hdf5");
    if(log_level != "info" && log_level != "warning" && log_level != "error")
        return absl::InvalidArgumentError("log_level must be info, warning or error");
    if(process_id.empty() || manifest_writer.empty() || internal_listen.empty() || self_endpoint.empty() ||
       visor_internal.empty() || archive_root.empty())
        return absl::InvalidArgumentError("grapher identities and endpoints required");
    if(limits.chunk_bytes == 0 || limits.chunk_bytes > 256 * 1024 * 1024 || limits.frame_bytes == 0 ||
       limits.frame_bytes > 4 * 1024 * 1024 || limits.frame_bytes > limits.chunk_bytes ||
       limits.concurrent_transfers == 0 || limits.concurrent_transfers > 32 || heartbeat_interval_ms == 0 ||
       heartbeat_interval_ms > 60000 || rpc_timeout_ms == 0 || rpc_timeout_ms > 60000 || drain_timeout_ms == 0 ||
       drain_timeout_ms > 60000)
        return absl::InvalidArgumentError("grapher limits out of range");
    const auto colon = internal_listen.rfind(':');
    if(colon == std::string::npos || colon + 1 == internal_listen.size())
        return absl::InvalidArgumentError("invalid internal_listen");
    const auto host = internal_listen.substr(0, colon);
    if((host.empty() || host == "0.0.0.0" || host == "[::]" || host == "::" || host == "*") && !insecure_bind_all)
        return absl::FailedPreconditionError("wildcard internal_listen requires --insecure-bind-all");
    return absl::OkStatus();
}
} // namespace chronolog::grapher
