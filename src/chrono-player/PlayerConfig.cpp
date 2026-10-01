#include "chrono-player/PlayerConfig.h"
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
                                                "tail_poll_ms",
                                                "static_routes"};
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
        num("tail_poll_ms", cfg.tail_poll_ms);
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
                            {"keeper_internal_suffix", &cfg.keeper_internal_suffix}})
        if(auto v = env(key))
            *field = *v;
    for(auto [key, field]: {std::pair<const char*, uint32_t*>{"keeper_deadline_ms", &cfg.keeper_deadline_ms},
                            {"batch_size", &cfg.batch_size},
                            {"tail_poll_ms", &cfg.tail_poll_ms}})
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
    if(auto valid = cfg.validate(); !valid.ok())
        return valid;
    return cfg;
}

absl::Status PlayerConfig::validate() const
{
    if(listen.empty() || player_id.empty())
        return absl::InvalidArgumentError("listen and player_id must be set");
    if(batch_size == 0 || tail_poll_ms == 0 || keeper_deadline_ms == 0)
        return absl::InvalidArgumentError("batch_size, tail_poll_ms and keeper_deadline_ms must be positive");
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
