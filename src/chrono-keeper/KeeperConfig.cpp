#include "KeeperConfig.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "absl/strings/str_cat.h"

namespace chronolog::keeper
{

namespace
{

std::string upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::toupper(c); });
    return text;
}

absl::StatusOr<uint64_t> parseUint(const std::string& key, const std::string& text)
{
    try
    {
        size_t used = 0;
        if(!text.empty() && text[0] == '-')
            throw std::invalid_argument(text);
        unsigned long long value = std::stoull(text, &used);
        if(used != text.size())
            throw std::invalid_argument(text);
        return static_cast<uint64_t>(value);
    }
    catch(const std::exception&)
    {
        return absl::InvalidArgumentError(absl::StrCat("environment value for ", key, " is not a number: ", text));
    }
}

absl::StatusOr<bool> parseBool(const std::string& key, std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    if(text == "1" || text == "true" || text == "yes")
        return true;
    if(text == "0" || text == "false" || text == "no")
        return false;
    return absl::InvalidArgumentError(absl::StrCat("environment value for ", key, " is not a boolean: ", text));
}

absl::Status applyJson(const nlohmann::json& json, KeeperConfig& cfg)
{
    static const std::set<std::string> known = {"listen",
                                                "internal_listen",
                                                "process_id",
                                                "log_level",
                                                "self_endpoint",
                                                "visor_internal",
                                                "payload_max_bytes",
                                                "causal_floor_skew_limit_ns",
                                                "dedupe_window",
                                                "wal_dir",
                                                "group_commit_max_bytes",
                                                "reserve_ahead_ms",
                                                "wal_max_bytes",
                                                "wal_segment_bytes",
                                                "shutdown_confirm_timeout_secs",
                                                "story_chunk_duration_secs",
                                                "seal_interval_ms",
                                                "chunk_max_bytes",
                                                "chunk_max_events",
                                                "frame_bytes",
                                                "watermark_resend_timeout_secs",
                                                "archive_visibility_delay_secs",
                                                "retention_cap_mb",

                                                "worker_threads",
                                                "heartbeat_interval_ms",
                                                "append_ceiling_wait_ms",
                                                "insecure_bind_all",
                                                "static_routes",
                                                "static_writers"};
    if(!json.is_object())
        return absl::InvalidArgumentError("configuration must be a JSON object");
    for(const auto& [key, value]: json.items())
        if(!known.contains(key))
            return absl::InvalidArgumentError(absl::StrCat("unknown configuration key ", key));
    // nlohmann converts a negative or oversized number into an unsigned field by wrapping, which would
    // quietly turn a mechanism off instead of failing the daemon at startup.
    static const std::map<std::string, uint64_t> unsigned_keys = {{"payload_max_bytes", UINT64_MAX},
                                                                  {"dedupe_window", UINT64_MAX},
                                                                  {"group_commit_window_ms", UINT32_MAX},
                                                                  {"group_commit_max_bytes", UINT64_MAX},
                                                                  {"reserve_ahead_ms", UINT32_MAX},
                                                                  {"wal_max_bytes", UINT64_MAX},
                                                                  {"wal_segment_bytes", UINT64_MAX},
                                                                  {"shutdown_confirm_timeout_secs", UINT32_MAX},
                                                                  {"story_chunk_duration_secs", UINT32_MAX},
                                                                  {"seal_interval_ms", UINT32_MAX},
                                                                  {"chunk_max_bytes", UINT64_MAX},
                                                                  {"chunk_max_events", UINT32_MAX},
                                                                  {"frame_bytes", UINT64_MAX},
                                                                  {"watermark_resend_timeout_secs", UINT32_MAX},
                                                                  {"archive_visibility_delay_secs", UINT32_MAX},
                                                                  {"retention_cap_mb", UINT64_MAX},
                                                                  {"worker_threads", UINT32_MAX},
                                                                  {"heartbeat_interval_ms", UINT32_MAX},
                                                                  {"append_ceiling_wait_ms", UINT32_MAX}};
    for(const auto& [key, maximum]: unsigned_keys)
        if(json.contains(key) && (!json.at(key).is_number_unsigned() || json.at(key).get<uint64_t>() > maximum))
            return absl::InvalidArgumentError(absl::StrCat(key, " must be a non-negative integer in range"));
    try
    {
        auto str = [&](const char* key, std::string& field)
        {
            if(json.contains(key))
                field = json.at(key).get<std::string>();
        };
        str("wal_dir", cfg.wal_dir);
        str("listen", cfg.listen);
        str("internal_listen", cfg.internal_listen);
        str("process_id", cfg.process_id);
        str("log_level", cfg.log_level);
        str("self_endpoint", cfg.self_endpoint);
        str("visor_internal", cfg.visor_internal);
        if(json.contains("payload_max_bytes"))
            cfg.payload_max_bytes = json.at("payload_max_bytes").get<size_t>();
        if(json.contains("causal_floor_skew_limit_ns"))
            cfg.causal_floor_skew_limit_ns = json.at("causal_floor_skew_limit_ns").get<int64_t>();
        if(json.contains("dedupe_window"))
            cfg.dedupe_window = json.at("dedupe_window").get<size_t>();
        if(json.contains("group_commit_max_bytes"))
            cfg.group_commit_max_bytes = json.at("group_commit_max_bytes").get<size_t>();
        if(json.contains("reserve_ahead_ms"))
            cfg.reserve_ahead_ms = json.at("reserve_ahead_ms").get<uint32_t>();
        if(json.contains("wal_segment_bytes"))
            cfg.wal_segment_bytes = json.at("wal_segment_bytes").get<uint64_t>();
        if(json.contains("shutdown_confirm_timeout_secs"))
            cfg.shutdown_confirm_timeout_secs = json.at("shutdown_confirm_timeout_secs").get<uint32_t>();
        if(json.contains("wal_max_bytes"))
            cfg.wal_max_bytes = json.at("wal_max_bytes").get<uint64_t>();
        if(json.contains("story_chunk_duration_secs"))
            cfg.story_chunk_duration_secs =
                    json.at("story_chunk_duration_secs").get<decltype(cfg.story_chunk_duration_secs)>();
        if(json.contains("seal_interval_ms"))
            cfg.seal_interval_ms = json.at("seal_interval_ms").get<decltype(cfg.seal_interval_ms)>();
        if(json.contains("chunk_max_bytes"))
            cfg.chunk_max_bytes = json.at("chunk_max_bytes").get<decltype(cfg.chunk_max_bytes)>();
        if(json.contains("chunk_max_events"))
            cfg.chunk_max_events = json.at("chunk_max_events").get<uint32_t>();
        if(json.contains("frame_bytes"))
            cfg.frame_bytes = json.at("frame_bytes").get<decltype(cfg.frame_bytes)>();
        if(json.contains("watermark_resend_timeout_secs"))
            cfg.watermark_resend_timeout_secs =
                    json.at("watermark_resend_timeout_secs").get<decltype(cfg.watermark_resend_timeout_secs)>();
        if(json.contains("archive_visibility_delay_secs"))
            cfg.archive_visibility_delay_secs =
                    json.at("archive_visibility_delay_secs").get<decltype(cfg.archive_visibility_delay_secs)>();
        if(json.contains("retention_cap_mb"))
            cfg.retention_cap_mb = json.at("retention_cap_mb").get<decltype(cfg.retention_cap_mb)>();
        if(json.contains("worker_threads"))
            cfg.worker_threads = json.at("worker_threads").get<uint32_t>();
        if(json.contains("append_ceiling_wait_ms"))
            cfg.append_ceiling_wait_ms = json.at("append_ceiling_wait_ms").get<uint32_t>();
        if(json.contains("heartbeat_interval_ms"))
            cfg.heartbeat_interval_ms = json.at("heartbeat_interval_ms").get<uint32_t>();
        if(json.contains("insecure_bind_all"))
            cfg.insecure_bind_all = json.at("insecure_bind_all").get<bool>();
        if(json.contains("static_routes"))
        {
            for(const auto& item: json.at("static_routes"))
            {
                StaticRoute entry;
                entry.story_id = item.at("story_id").get<uint64_t>();
                entry.route.epoch = item.at("epoch").get<uint64_t>();
                for(const auto& k: item.at("keepers"))
                    entry.route.keepers.push_back(
                            KeeperRef{k.at("process_id").get<std::string>(), k.at("endpoint").get<std::string>()});
                entry.route.grapher = item.value("grapher", "");
                entry.route.player = item.value("player", "");
                cfg.static_routes.push_back(std::move(entry));
            }
        }
        if(json.contains("static_writers"))
        {
            for(const auto& item: json.at("static_writers"))
                cfg.static_writers.push_back(StaticWriter{item.at("story_id").get<uint64_t>(),
                                                          item.at("writer_id").get<uint64_t>(),
                                                          item.at("incarnation").get<uint64_t>()});
        }
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

absl::StatusOr<KeeperConfig>
KeeperConfig::load(const std::optional<std::string>& path, const Getenv& getenv, bool allow_bind_all)
{
    KeeperConfig cfg;
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
        const char* value = getenv(("CHRONOLOG_KEEPER_" + upper(key)).c_str());
        return value ? std::optional<std::string>(value) : std::nullopt;
    };
    for(auto [key, field]: {std::pair<const char*, std::string*>{"listen", &cfg.listen},
                            {"internal_listen", &cfg.internal_listen},
                            {"process_id", &cfg.process_id},
                            {"log_level", &cfg.log_level},
                            {"self_endpoint", &cfg.self_endpoint},
                            {"visor_internal", &cfg.visor_internal},
                            {"wal_dir", &cfg.wal_dir}})
        if(auto v = env(key))
            *field = *v;
    if(auto v = env("payload_max_bytes"))
    {
        auto parsed = parseUint("payload_max_bytes", *v);
        if(!parsed.ok())
            return parsed.status();
        cfg.payload_max_bytes = *parsed;
    }
    for(auto [key, field]: {std::pair<const char*, uint64_t*>{"wal_max_bytes", &cfg.wal_max_bytes},
                            {"wal_segment_bytes", &cfg.wal_segment_bytes},
                            {"group_commit_max_bytes", &cfg.group_commit_max_bytes},
                            {"chunk_max_bytes", &cfg.chunk_max_bytes},
                            {"frame_bytes", &cfg.frame_bytes},
                            {"retention_cap_mb", &cfg.retention_cap_mb}})
    {
        if(auto v = env(key))
        {
            auto parsed = parseUint(key, *v);
            if(!parsed.ok())
                return parsed.status();
            *field = *parsed;
        }
    }
    if(auto v = env("dedupe_window"))
    {
        auto parsed = parseUint("dedupe_window", *v);
        if(!parsed.ok())
            return parsed.status();
        cfg.dedupe_window = *parsed;
    }
    if(auto v = env("causal_floor_skew_limit_ns"))
    {
        auto parsed = parseUint("causal_floor_skew_limit_ns", *v);
        if(!parsed.ok())
            return parsed.status();
        cfg.causal_floor_skew_limit_ns = static_cast<int64_t>(*parsed);
    }
    for(auto [key, field]: {std::pair<const char*, uint32_t*>{"worker_threads", &cfg.worker_threads},
                            {"shutdown_confirm_timeout_secs", &cfg.shutdown_confirm_timeout_secs},
                            {"heartbeat_interval_ms", &cfg.heartbeat_interval_ms},
                            {"append_ceiling_wait_ms", &cfg.append_ceiling_wait_ms},
                            {"reserve_ahead_ms", &cfg.reserve_ahead_ms},
                            {"story_chunk_duration_secs", &cfg.story_chunk_duration_secs},
                            {"seal_interval_ms", &cfg.seal_interval_ms},
                            {"chunk_max_events", &cfg.chunk_max_events},
                            {"watermark_resend_timeout_secs", &cfg.watermark_resend_timeout_secs},
                            {"archive_visibility_delay_secs", &cfg.archive_visibility_delay_secs}})
    {
        if(auto v = env(key))
        {
            auto parsed = parseUint(key, *v);
            if(!parsed.ok())
                return parsed.status();
            if(*parsed > UINT32_MAX)
                return absl::InvalidArgumentError(absl::StrCat("environment value for ", key, " is too large"));
            *field = static_cast<uint32_t>(*parsed);
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
    if(auto valid = cfg.validate(); !valid.ok())
        return valid;
    return cfg;
}

absl::Status KeeperConfig::validate() const
{
    if(listen.empty() || internal_listen.empty() || process_id.empty() || self_endpoint.empty() ||
       visor_internal.empty())
        return absl::InvalidArgumentError(
                "listen, internal_listen, process_id, self_endpoint and visor_internal must be set");
    if(payload_max_bytes == 0 || dedupe_window == 0 || heartbeat_interval_ms == 0 || causal_floor_skew_limit_ns < 0)
        return absl::InvalidArgumentError(
                "payload_max_bytes, dedupe_window and heartbeat_interval_ms must be positive and the skew limit "
                "non-negative");
    if(wal_dir.empty() || group_commit_max_bytes == 0 || reserve_ahead_ms == 0 || wal_max_bytes == 0 ||
       wal_segment_bytes == 0)
        return absl::InvalidArgumentError(
                "wal_dir, group_commit_max_bytes, reserve_ahead_ms, wal_max_bytes and wal_segment_bytes must be set");
    // Zero would send every unconfirmed chunk again on each pass and replace its receipt each time.
    if(watermark_resend_timeout_secs == 0)
        return absl::InvalidArgumentError("watermark_resend_timeout_secs must be positive");
    if(story_chunk_duration_secs == 0 || seal_interval_ms == 0 || chunk_max_bytes == 0 ||
       chunk_max_bytes > (64u << 20) || chunk_max_events == 0 || chunk_max_events > 65536 || frame_bytes == 0 ||
       frame_bytes > (4u << 20))
        return absl::InvalidArgumentError("invalid archive chunk, frame or timer configuration");
    if(log_level != "info" && log_level != "warning" && log_level != "error")
        return absl::InvalidArgumentError("log_level must be info, warning or error");
    for(const auto& writer: static_writers)
        if(writer.story_id == 0 || writer.writer_id == 0 || writer.incarnation == 0)
            return absl::InvalidArgumentError("static_writers entries need story_id, writer_id and incarnation");
    const std::string host = hostOf(internal_listen);
    const bool wildcard = host.empty() || host == "0.0.0.0" || host == "[::]" || host == "::" || host == "*";
    if(wildcard && !insecure_bind_all)
        return absl::FailedPreconditionError(absl::StrCat("internal_listen ",
                                                          internal_listen,
                                                          " binds every interface; bind the cluster interface or set "
                                                          "insecure_bind_all"));
    return absl::OkStatus();
}

uint32_t KeeperConfig::effectiveWorkerThreads() const
{
    if(worker_threads != 0)
        return worker_threads;
    return std::max(1u, std::thread::hardware_concurrency());
}

} // namespace chronolog::keeper
