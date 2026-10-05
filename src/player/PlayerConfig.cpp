#include "player/PlayerConfig.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>
#include "absl/strings/str_cat.h"

namespace chronolog::player
{
namespace
{

std::string upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
    return text;
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

absl::StatusOr<std::map<std::string, std::string>> parsePairs(const std::string& text)
{
    std::map<std::string, std::string> out;
    std::stringstream stream(text);
    std::string item;
    while(std::getline(stream, item, ','))
    {
        if(item.empty())
            continue;
        const auto eq = item.find('=');
        if(eq == std::string::npos)
            return absl::InvalidArgumentError(
                    absl::StrCat("keeper_internal entry ", item, " is not process_id=address"));
        out[item.substr(0, eq)] = item.substr(eq + 1);
    }
    return out;
}

absl::Status applyJson(const nlohmann::json& json, PlayerConfig& cfg)
{
    static const std::set<std::string> known = {"listen",
                                                "advertise",
                                                "player_id",
                                                "visor",
                                                "visor_internal",
                                                "keeper_internal_suffix",
                                                "keeper_internal",
                                                "keeper_deadline_ms",
                                                "batch_size",
                                                "read_max_events",
                                                "tail_max_bytes",
                                                "tail_poll_ms",
                                                "await_max_wait_ms",
                                                "archive_root",
                                                "manifest_poll_ms",
                                                "archive_read_timeout_ms",
                                                "log_level",
                                                "static_routes",
                                                "deployment_id",
                                                "tiers",
                                                "tier_io_timeout_ms",
                                                "tier_probe_interval_ms",
                                                "tier_probe_timeout_ms",
                                                "slow_tier_io_threads"};
    if(!json.is_object())
        return absl::InvalidArgumentError("configuration must be a JSON object");
    for(const auto& [key, value]: json.items())
        if(!known.contains(key))
            return absl::InvalidArgumentError(absl::StrCat("unknown configuration key ", key));
    try
    {
        auto str = [&](const char* key, std::string& field)
        {
            if(json.contains(key))
                field = json.at(key).get<std::string>();
        };
        auto num = [&](const char* key, uint32_t& field)
        {
            if(json.contains(key))
                field = json.at(key).get<uint32_t>();
        };
        str("listen", cfg.listen);
        str("advertise", cfg.advertise);
        str("player_id", cfg.player_id);
        str("visor", cfg.visor);
        str("visor_internal", cfg.visor_internal);
        str("keeper_internal_suffix", cfg.keeper_internal_suffix);
        num("keeper_deadline_ms", cfg.keeper_deadline_ms);
        num("batch_size", cfg.batch_size);
        num("read_max_events", cfg.read_max_events);
        num("tail_max_bytes", cfg.tail_max_bytes);
        num("tail_poll_ms", cfg.tail_poll_ms);
        num("await_max_wait_ms", cfg.await_max_wait_ms);
        str("archive_root", cfg.archive_root);
        num("manifest_poll_ms", cfg.manifest_poll_ms);
        num("archive_read_timeout_ms", cfg.archive_read_timeout_ms);
        str("log_level", cfg.log_level);
        str("deployment_id", cfg.deployment_id);
        num("tier_io_timeout_ms", cfg.tier_io_timeout_ms);
        num("tier_probe_interval_ms", cfg.tier_probe_interval_ms);
        num("tier_probe_timeout_ms", cfg.tier_probe_timeout_ms);
        num("slow_tier_io_threads", cfg.slow_tier_io_threads);
        if(json.contains("tiers"))
        {
            if(!json.at("tiers").is_array())
                return absl::InvalidArgumentError("tiers must be an array");
            cfg.tiers.clear();
            for(const auto& item: json.at("tiers"))
            {
                static const std::set<std::string> keys = {"name",
                                                           "kind",
                                                           "root",
                                                           "rank",
                                                           "tier_uuid",
                                                           "f_type",
                                                           "st_dev",
                                                           "f_fsid",
                                                           "budget_bytes",
                                                           "min_free_fraction",
                                                           "high_watermark",
                                                           "low_watermark"};
                if(!item.is_object())
                    return absl::InvalidArgumentError("tier must be an object");
                for(const auto& [key, value]: item.items())
                    if(!keys.contains(key))
                        return absl::InvalidArgumentError("unknown tier key " + key);
                cfg.tiers.push_back({item.at("name").get<std::string>(),
                                     item.at("kind").get<std::string>(),
                                     item.at("root").get<std::string>(),
                                     item.at("rank").get<uint32_t>(),
                                     item.at("tier_uuid").get<std::string>()});
                (void)item.at("f_type").get<int64_t>();
                (void)item.at("st_dev").get<uint64_t>();
                (void)item.at("f_fsid").get<std::array<int32_t, 2>>();
            }
        }
        if(json.contains("keeper_internal"))
            cfg.keeper_internal = json.at("keeper_internal").get<std::map<std::string, std::string>>();
        if(json.contains("static_routes"))
        {
            const auto& routes = json.at("static_routes");
            Route route;
            route.epoch = routes.at("epoch").get<uint64_t>();
            for(const auto& item: routes.at("keepers"))
                route.keepers.push_back(
                        KeeperRef{item.at("process_id").get<std::string>(), item.at("endpoint").get<std::string>()});
            cfg.static_routes = std::move(route);
        }
    }
    catch(const nlohmann::json::exception& e)
    {
        return absl::InvalidArgumentError(absl::StrCat("bad configuration value: ", e.what()));
    }
    return absl::OkStatus();
}

} // namespace

absl::StatusOr<PlayerConfig> PlayerConfig::load(const std::optional<std::string>& path, const Getenv& getenv)
{
    PlayerConfig cfg;
    if(path)
    {
        std::ifstream file(*path);
        if(!file)
            return absl::NotFoundError(absl::StrCat("cannot read configuration file ", *path));
        nlohmann::json json = nlohmann::json::parse(file, nullptr, false);
        if(json.is_discarded())
            return absl::InvalidArgumentError(absl::StrCat("configuration file ", *path, " is not valid JSON"));
        if(auto applied = applyJson(json, cfg); !applied.ok())
            return applied;
    }
    auto env = [&](const char* key) -> std::optional<std::string>
    {
        const char* value = getenv(("CHRONOLOG_PLAYER_" + upper(key)).c_str());
        return value ? std::optional<std::string>(value) : std::nullopt;
    };
    for(auto [key, field]: {std::pair<const char*, std::string*>{"listen", &cfg.listen},
                            {"advertise", &cfg.advertise},
                            {"player_id", &cfg.player_id},
                            {"visor", &cfg.visor},
                            {"visor_internal", &cfg.visor_internal},
                            {"keeper_internal_suffix", &cfg.keeper_internal_suffix},
                            {"archive_root", &cfg.archive_root},
                            {"deployment_id", &cfg.deployment_id},
                            {"log_level", &cfg.log_level}})
        if(auto v = env(key))
            *field = *v;
    for(auto [key, field]: {std::pair<const char*, uint32_t*>{"keeper_deadline_ms", &cfg.keeper_deadline_ms},
                            {"batch_size", &cfg.batch_size},
                            {"read_max_events", &cfg.read_max_events},
                            {"tail_max_bytes", &cfg.tail_max_bytes},
                            {"tail_poll_ms", &cfg.tail_poll_ms},
                            {"await_max_wait_ms", &cfg.await_max_wait_ms},
                            {"manifest_poll_ms", &cfg.manifest_poll_ms},
                            {"archive_read_timeout_ms", &cfg.archive_read_timeout_ms},
                            {"tier_io_timeout_ms", &cfg.tier_io_timeout_ms},
                            {"tier_probe_interval_ms", &cfg.tier_probe_interval_ms},
                            {"tier_probe_timeout_ms", &cfg.tier_probe_timeout_ms},
                            {"slow_tier_io_threads", &cfg.slow_tier_io_threads}})
    {
        if(auto v = env(key))
        {
            auto parsed = parseUint(key, *v);
            if(!parsed.ok())
                return parsed.status();
            *field = *parsed;
        }
    }
    if(auto v = env("keeper_internal"))
    {
        auto parsed = parsePairs(*v);
        if(!parsed.ok())
            return parsed.status();
        cfg.keeper_internal = std::move(*parsed);
    }
    if(auto v = env("tiers"))
    {
        auto table = nlohmann::json::parse(*v, nullptr, false);
        if(table.is_discarded())
            return absl::InvalidArgumentError("environment tiers is not valid JSON");
        if(auto status = applyJson(nlohmann::json{{"tiers", table}}, cfg); !status.ok())
            return status;
    }
    if(auto valid = cfg.validate(); !valid.ok())
        return valid;
    return cfg;
}

absl::Status PlayerConfig::validate() const
{
    if(listen.empty() || player_id.empty())
        return absl::InvalidArgumentError("listen and player_id must be set");
    if(await_max_wait_ms == 0 || tail_max_bytes == 0 || read_max_events == 0 || batch_size == 0 || tail_poll_ms == 0 ||
       keeper_deadline_ms == 0 || manifest_poll_ms == 0 || archive_read_timeout_ms == 0)
        return absl::InvalidArgumentError(
                "tail_max_bytes, read_max_events, batch_size, tail_poll_ms, "
                "keeper_deadline_ms, manifest_poll_ms and archive_read_timeout_ms must be positive");
    if(!tier_io_timeout_ms || !tier_probe_interval_ms || !tier_probe_timeout_ms || !slow_tier_io_threads ||
       slow_tier_io_threads > 8)
        return absl::InvalidArgumentError(
                "tier timeouts and interval must be positive; slow_tier_io_threads must be 1..8");
    if(!tiers.empty())
    {
        if(deployment_id.empty())
            return absl::InvalidArgumentError("tiers require deployment_id");
        if(tiers.front().name != "local" || tiers.front().rank != 0 ||
           tiers.front().root.lexically_normal() != std::filesystem::path(archive_root).lexically_normal())
            return absl::InvalidArgumentError("rank 0 must be local at archive_root");
        std::set<std::string> names;
        uint32_t previous = 0;
        for(size_t i = 0; i < tiers.size(); ++i)
        {
            const auto& tier = tiers[i];
            if(tier.kind != "posix")
                return absl::InvalidArgumentError("Player supports posix tiers only; s3 is unsupported");
            if(tier.name.empty() || tier.root.empty() || tier.tier_uuid.empty() || !names.insert(tier.name).second ||
               (i && tier.rank <= previous))
                return absl::InvalidArgumentError("tier names must be unique and ranks strictly increasing from 0");
            previous = tier.rank;
        }
    }
    if(log_level != "info" && log_level != "warning" && log_level != "error")
        return absl::InvalidArgumentError("log_level must be info, warning or error");
    if(!static_routes && visor_internal.empty())
        return absl::InvalidArgumentError("visor_internal is required without static_routes");
    if(static_routes && static_routes->keepers.empty())
        return absl::InvalidArgumentError("static_routes must list at least one keeper");
    return absl::OkStatus();
}

std::string PlayerConfig::keeperInternal(const KeeperRef& keeper) const
{
    if(auto it = keeper_internal.find(keeper.process_id); it != keeper_internal.end())
        return it->second;
    const auto colon = keeper.endpoint.rfind(':');
    const std::string host = colon == std::string::npos ? keeper.endpoint : keeper.endpoint.substr(0, colon);
    return host + keeper_internal_suffix;
}

} // namespace chronolog::player
