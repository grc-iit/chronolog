#include "VisorConfig.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "absl/strings/str_cat.h"

namespace chronolog::visor
{

namespace
{

std::string upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
    return text;
}

std::vector<std::string> splitCommaList(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream stream(text);
    std::string item;
    while(std::getline(stream, item, ','))
        if(!item.empty())
            out.push_back(item);
    return out;
}

absl::StatusOr<std::vector<KeeperRef>> parseKeepers(const std::string& text)
{
    std::vector<KeeperRef> out;
    for(const std::string& item: splitCommaList(text))
    {
        const auto eq = item.find('=');
        if(eq == std::string::npos)
            return absl::InvalidArgumentError(absl::StrCat("keepers entry ", item, " is not process_id=endpoint"));
        out.push_back(KeeperRef{item.substr(0, eq), item.substr(eq + 1)});
    }
    return out;
}

absl::StatusOr<bool> parseBool(const std::string& key, const std::string& text)
{
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if(lower == "1" || lower == "true" || lower == "yes")
        return true;
    if(lower == "0" || lower == "false" || lower == "no")
        return false;
    return absl::InvalidArgumentError(absl::StrCat("environment value for ", key, " is not a boolean: ", text));
}

absl::StatusOr<uint32_t> parseUint(const std::string& key, const std::string& text)
{
    try
    {
        size_t used = 0;
        unsigned long value = std::stoul(text, &used);
        if(used != text.size() || value > UINT32_MAX)
            throw std::invalid_argument(text);
        return static_cast<uint32_t>(value);
    }
    catch(const std::exception&)
    {
        return absl::InvalidArgumentError(absl::StrCat("environment value for ", key, " is not a number: ", text));
    }
}

absl::Status applyJson(const nlohmann::json& json, VisorConfig& cfg)
{
    static const std::set<std::string> known = {"membership_mode",
                                                "raft",
                                                "listen",
                                                "internal_listen",
                                                "db_path",
                                                "keepers",
                                                "grapher",
                                                "graphers",
                                                "player",
                                                "heartbeat_timeout_ms",
                                                "keeper_failure_timeout_ms",
                                                "release_fence_timeout_ms",
                                                "worker_threads",
                                                "insecure_bind_all",
                                                "acquisition_lease_default_ns",
                                                "acquisition_lease_min_ns",
                                                "acquisition_lease_max_ns",
                                                "lease_safety_margin_ms",
                                                "acquisition_service_tick_ms",
                                                "acquisition_service_gap_ms",
                                                "acquisition_scan_batch",
                                                "acquisition_expiry_batch",
                                                "acquisition_renew_batch",
                                                "acquisition_evidence_batch",
                                                "log_level"};
    if(!json.is_object())
        return absl::InvalidArgumentError("configuration must be a JSON object");
    for(const auto& [key, value]: json.items())
        if(!known.contains(key))
            return absl::InvalidArgumentError(absl::StrCat("unknown configuration key ", key));
    try
    {
        if(json.contains("acquisition_lease_default_ns"))
            cfg.leases.acquisition_lease_default_ns = json.at("acquisition_lease_default_ns").get<int64_t>();
        if(json.contains("acquisition_lease_min_ns"))
            cfg.leases.acquisition_lease_min_ns = json.at("acquisition_lease_min_ns").get<int64_t>();
        if(json.contains("acquisition_lease_max_ns"))
            cfg.leases.acquisition_lease_max_ns = json.at("acquisition_lease_max_ns").get<int64_t>();
        if(json.contains("lease_safety_margin_ms"))
            cfg.leases.lease_safety_margin_ms = json.at("lease_safety_margin_ms").get<uint32_t>();
        if(json.contains("acquisition_service_tick_ms"))
            cfg.leases.acquisition_service_tick_ms = json.at("acquisition_service_tick_ms").get<uint32_t>();
        if(json.contains("acquisition_service_gap_ms"))
            cfg.leases.acquisition_service_gap_ms = json.at("acquisition_service_gap_ms").get<uint32_t>();
        if(json.contains("acquisition_scan_batch"))
            cfg.leases.acquisition_scan_batch = json.at("acquisition_scan_batch").get<uint32_t>();
        if(json.contains("acquisition_expiry_batch"))
            cfg.leases.acquisition_expiry_batch = json.at("acquisition_expiry_batch").get<uint32_t>();
        if(json.contains("acquisition_renew_batch"))
            cfg.leases.acquisition_renew_batch = json.at("acquisition_renew_batch").get<uint32_t>();
        if(json.contains("acquisition_evidence_batch"))
            cfg.leases.acquisition_evidence_batch = json.at("acquisition_evidence_batch").get<uint32_t>();
        if(json.contains("membership_mode"))
            cfg.membership_mode = json.at("membership_mode").get<std::string>();
        if(json.contains("raft"))
        {
            const auto& r = json.at("raft");
            cfg.raft.server_id = r.at("server_id").get<int32_t>();
            cfg.raft.raft_endpoint = r.at("raft_endpoint").get<std::string>();
            cfg.raft.election_lower_ms = r.value("election_lower_ms", 300u);
            cfg.raft.election_upper_ms = r.value("election_upper_ms", 600u);
            cfg.raft.heartbeat_ms = r.value("heartbeat_ms", 75u);
            for(const auto& p: r.at("peers"))
                cfg.raft.peers.push_back({p.at("id").get<int32_t>(),
                                          p.at("raft_endpoint").get<std::string>(),
                                          p.at("catalog_endpoint").get<std::string>(),
                                          p.at("internal_endpoint").get<std::string>()});
        }
        if(json.contains("listen"))
            cfg.listen = json.at("listen").get<std::string>();
        if(json.contains("internal_listen"))
            cfg.internal_listen = json.at("internal_listen").get<std::string>();
        if(json.contains("db_path"))
            cfg.db_path = json.at("db_path").get<std::string>();
        if(json.contains("keepers"))
        {
            cfg.keepers.clear();
            for(const auto& item: json.at("keepers"))
                cfg.keepers.push_back(
                        KeeperRef{item.at("process_id").get<std::string>(), item.at("endpoint").get<std::string>()});
        }
        if(json.contains("grapher"))
            cfg.grapher = json.at("grapher").get<std::string>();
        if(json.contains("graphers"))
        {
            cfg.graphers = json.at("graphers").get<std::vector<std::string>>();
            if(cfg.graphers.empty())
                return absl::InvalidArgumentError("graphers must contain at least one endpoint");
        }
        if(json.contains("player"))
            cfg.player = json.at("player").get<std::string>();
        if(json.contains("heartbeat_timeout_ms"))
            cfg.heartbeat_timeout_ms = json.at("heartbeat_timeout_ms").get<uint32_t>();
        if(json.contains("keeper_failure_timeout_ms"))
            cfg.heartbeat_timeout_ms = json.at("keeper_failure_timeout_ms").get<uint32_t>();
        if(json.contains("release_fence_timeout_ms"))
            cfg.release_fence_timeout_ms = json.at("release_fence_timeout_ms").get<uint32_t>();
        if(json.contains("worker_threads"))
            cfg.worker_threads = json.at("worker_threads").get<uint32_t>();
        if(json.contains("insecure_bind_all"))
            cfg.insecure_bind_all = json.at("insecure_bind_all").get<bool>();
        if(json.contains("log_level"))
            cfg.log_level = json.at("log_level").get<std::string>();
    }
    catch(const nlohmann::json::exception& e)
    {
        return absl::InvalidArgumentError(absl::StrCat("bad configuration value: ", e.what()));
    }
    return absl::OkStatus();
}

std::string hostOf(const std::string& address)
{
    const auto colon = address.rfind(':');
    return colon == std::string::npos ? address : address.substr(0, colon);
}

} // namespace

absl::StatusOr<VisorConfig>
VisorConfig::load(const std::optional<std::string>& path, const Getenv& getenv, bool allow_bind_all)
{
    VisorConfig cfg;
    if(path)
    {
        std::ifstream file(*path);
        if(!file)
            return absl::NotFoundError(absl::StrCat("cannot read configuration file ", *path));
        nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
        if(json.is_discarded())
            return absl::InvalidArgumentError(absl::StrCat("configuration file ", *path, " is not valid JSON"));
        absl::Status applied = applyJson(json, cfg);
        if(!applied.ok())
            return applied;
    }

    auto env = [&](const char* key) -> std::optional<std::string>
    {
        const char* value = getenv(("CHRONOLOG_VISOR_" + upper(key)).c_str());
        return value ? std::optional<std::string>(value) : std::nullopt;
    };
    if(auto v = env("membership_mode"))
        cfg.membership_mode = *v;
    if(auto v = env("listen"))
        cfg.listen = *v;
    if(auto v = env("internal_listen"))
        cfg.internal_listen = *v;
    if(auto v = env("db_path"))
        cfg.db_path = *v;
    if(auto v = env("keepers"))
    {
        auto parsed = parseKeepers(*v);
        if(!parsed.ok())
            return parsed.status();
        cfg.keepers = std::move(*parsed);
    }
    if(auto v = env("grapher"))
        cfg.grapher = *v;
    if(auto v = env("graphers"))
    {
        cfg.graphers = splitCommaList(*v);
        if(cfg.graphers.empty())
            return absl::InvalidArgumentError("graphers must contain at least one endpoint");
    }
    if(auto v = env("player"))
        cfg.player = *v;
    if(auto v = env("log_level"))
        cfg.log_level = *v;
    for(auto [key, field]: {std::pair<const char*, uint32_t*>{"heartbeat_timeout_ms", &cfg.heartbeat_timeout_ms},
                            {"keeper_failure_timeout_ms", &cfg.heartbeat_timeout_ms},
                            {"release_fence_timeout_ms", &cfg.release_fence_timeout_ms},
                            {"worker_threads", &cfg.worker_threads},
                            {"lease_safety_margin_ms", &cfg.leases.lease_safety_margin_ms},
                            {"acquisition_service_tick_ms", &cfg.leases.acquisition_service_tick_ms},
                            {"acquisition_service_gap_ms", &cfg.leases.acquisition_service_gap_ms},
                            {"acquisition_scan_batch", &cfg.leases.acquisition_scan_batch},
                            {"acquisition_expiry_batch", &cfg.leases.acquisition_expiry_batch},
                            {"acquisition_renew_batch", &cfg.leases.acquisition_renew_batch},
                            {"acquisition_evidence_batch", &cfg.leases.acquisition_evidence_batch}})
    {
        if(auto v = env(key))
        {
            auto parsed = parseUint(key, *v);
            if(!parsed.ok())
                return parsed.status();
            *field = *parsed;
        }
    }
    for(auto [key, field]:
        {std::pair<const char*, int64_t*>{"acquisition_lease_default_ns", &cfg.leases.acquisition_lease_default_ns},
         {"acquisition_lease_min_ns", &cfg.leases.acquisition_lease_min_ns},
         {"acquisition_lease_max_ns", &cfg.leases.acquisition_lease_max_ns}})
    {
        if(auto value = env(key))
        {
            try
            {
                size_t consumed = 0;
                auto parsed = std::stoll(*value, &consumed);
                if(consumed != value->size())
                    return absl::InvalidArgumentError("invalid lease duration configuration");
                *field = parsed;
            }
            catch(const std::exception&)
            {
                return absl::InvalidArgumentError("invalid lease duration configuration");
            }
        }
    }
    if(auto v = env("insecure_bind_all"))
    {
        auto parsed = parseBool("insecure_bind_all", *v);
        if(!parsed.ok())
            return parsed.status();
        cfg.insecure_bind_all = *parsed;
    }

    cfg.insecure_bind_all = cfg.insecure_bind_all || allow_bind_all;
    absl::Status valid = cfg.validate();
    if(!valid.ok())
        return valid;
    return cfg;
}

absl::Status VisorConfig::validate() const
{
    auto lease_status = leases.validate(raft.election_upper_ms, heartbeat_timeout_ms, release_fence_timeout_ms);
    if(!lease_status.ok())
        return lease_status;
    if(membership_mode != "static" && membership_mode != "dynamic")
        return absl::InvalidArgumentError("membership_mode must be static or dynamic");
    if(membership_mode == "dynamic")
    {
        std::set<int32_t> ids;
        std::set<std::string> endpoints;
        bool self = false;
        for(const auto& p: raft.peers)
        {
            if(p.id <= 0 || p.raft_endpoint.empty() || p.catalog_endpoint.empty() || p.internal_endpoint.empty() ||
               !ids.insert(p.id).second || !endpoints.insert(p.raft_endpoint).second)
                return absl::InvalidArgumentError("invalid or duplicate raft peer");
            if(p.id == raft.server_id)
                self = p.raft_endpoint == raft.raft_endpoint;
        }
        if(!self || raft.peers.size() % 2 == 0 || raft.election_upper_ms > INT32_MAX || raft.heartbeat_ms > INT32_MAX ||
           raft.heartbeat_ms == 0 || raft.election_lower_ms <= 2 * raft.heartbeat_ms ||
           raft.election_upper_ms < raft.election_lower_ms)
            return absl::InvalidArgumentError("invalid raft membership or timings");
    }
    if(listen.empty() || internal_listen.empty() || db_path.empty() || (grapher.empty() && graphers.empty()) ||
       player.empty())
        return absl::InvalidArgumentError("listen, internal_listen, db_path, grapher and player must be set");
    if(keepers.empty() || std::any_of(keepers.begin(),
                                      keepers.end(),
                                      [](const KeeperRef& k) { return k.process_id.empty() || k.endpoint.empty(); }))
        return absl::InvalidArgumentError("keepers must list at least one {process_id, endpoint}");
    if(std::any_of(graphers.begin(), graphers.end(), [](const auto& endpoint) { return endpoint.empty(); }))
        return absl::InvalidArgumentError("graphers endpoints must be nonempty");
    if(worker_threads == 0)
        return absl::InvalidArgumentError("worker_threads must be positive");
    if(log_level != "info" && log_level != "warning" && log_level != "error")
        return absl::InvalidArgumentError("log_level must be info, warning or error");
    const std::string host = hostOf(internal_listen);
    const bool wildcard = host.empty() || host == "0.0.0.0" || host == "[::]" || host == "::" || host == "*";
    if(wildcard && !insecure_bind_all)
    {
        return absl::FailedPreconditionError(absl::StrCat("internal_listen ",
                                                          internal_listen,
                                                          " binds every interface; bind the cluster interface or set "
                                                          "insecure_bind_all"));
    }
    return absl::OkStatus();
}

} // namespace chronolog::visor
