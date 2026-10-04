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
                                                   {"deployment_id", &config.deployment_id},
                                                   {"tier_status_file", &config.migration.status_file},
                                                   {"archive_root", &config.archive_root}};
    std::map<std::string, uint64_t> numbers = {{"heartbeat_interval_ms", config.heartbeat_interval_ms},
                                               {"rpc_timeout_ms", config.rpc_timeout_ms},
                                               {"drain_timeout_ms", config.drain_timeout_ms},
                                               {"max_chunk_bytes", config.limits.chunk_bytes},
                                               {"max_frame_bytes", config.limits.frame_bytes},
                                               {"concurrent_transfers", config.limits.concurrent_transfers}};
    const auto& policy = config.compaction.policy;
    // Compaction knobs; compact_min_age_secs alone may be zero. Ranges are checked by validate().
    std::map<std::string, uint64_t> compaction = {
            {"migrate_after_s", config.migration.after_s},
            {"migration_io_bytes_per_sec", config.migration.io_bytes_per_sec},
            {"tier_probe_interval_ms", config.migration.probe_interval_ms},
            {"tier_probe_timeout_ms", config.migration.probe_timeout_ms},
            {"tier_io_timeout_ms", config.migration.io_timeout_ms},
            {"slow_tier_io_threads", config.migration.io_threads},
            {"compact_scan_interval_secs", static_cast<uint64_t>(config.compaction.scan_interval.count())},
            {"compact_min_files", policy.min_files},
            {"compact_max_files", policy.max_files},
            {"compact_small_file_bytes", policy.small_file_bytes},
            {"compact_min_age_secs", static_cast<uint64_t>(policy.min_age.count())},
            {"compact_max_span_secs", static_cast<uint64_t>(policy.max_span_ns / 1000000000)},
            {"compact_max_output_bytes", policy.max_output_bytes},
            {"compact_max_events", policy.max_events},
            {"compact_io_bytes_per_sec", policy.io_bytes_per_sec},
            {"compact_io_burst_bytes", policy.io_burst_bytes}};
    for(const auto& [key, value]: input.items())
        if(!strings.contains(key) && !numbers.contains(key) && !compaction.contains(key) &&
           key != "insecure_bind_all" && key != "compact_enabled" && key != "hard_stop_reserve_bytes" &&
           key != "tiers" && key != "migrate_enabled")
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
        for(auto& [key, number]: compaction)
        {
            if(input.contains(key))
            {
                if(!input.at(key).is_number_integer() || input.at(key) < 0)
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
            if(number > (uint64_t{1} << 40) || (number == 0 && key != "compact_min_age_secs"))
                return absl::InvalidArgumentError("number out of range for " + key);
        }
        if(input.contains("hard_stop_reserve_bytes"))
        {
            if(!input.at("hard_stop_reserve_bytes").is_number_integer() || input.at("hard_stop_reserve_bytes") < 0)
                return absl::InvalidArgumentError("invalid number for hard_stop_reserve_bytes");
            config.hard_stop_reserve_bytes = input.at("hard_stop_reserve_bytes").get<uint64_t>();
        }
        if(const auto* value = env("hard_stop_reserve_bytes"))
        {
            const std::string text(value);
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), config.hard_stop_reserve_bytes);
            if(parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return absl::InvalidArgumentError("invalid environment number for hard_stop_reserve_bytes");
        }
        if(input.contains("tiers"))
        {
            if(!input.at("tiers").is_array() || input.at("tiers").empty())
                return absl::InvalidArgumentError("tiers must be a nonempty JSON array");
            const std::set<std::string> keys{"name",
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
            for(const auto& entry: input.at("tiers"))
            {
                if(!entry.is_object())
                    return absl::InvalidArgumentError("tier must be an object");
                for(const auto& [key, value]: entry.items())
                    if(!keys.contains(key))
                        return absl::InvalidArgumentError("unknown tier configuration key " + key);
                GrapherTier tier;
                tier.config.name = entry.at("name").get<std::string>();
                tier.config.kind = entry.at("kind").get<std::string>();
                tier.config.root = entry.at("root").get<std::string>();
                if(!entry.at("rank").is_number_integer() || entry.at("rank") < 0 || entry.at("rank") > UINT32_MAX)
                    return absl::InvalidArgumentError("invalid tier rank");
                tier.config.rank = entry.at("rank").get<uint32_t>();
                tier.config.tier_uuid = entry.at("tier_uuid").get<std::string>();
                if(!entry.at("f_type").is_number_integer() || !entry.at("st_dev").is_number_integer() ||
                   !entry.at("f_fsid").is_array())
                    return absl::InvalidArgumentError("invalid tier filesystem identity");
                if(entry.contains("budget_bytes"))
                {
                    if(!entry.at("budget_bytes").is_number_integer() || entry.at("budget_bytes") < 0)
                        return absl::InvalidArgumentError("invalid tier budget_bytes");
                    tier.budget_bytes = entry.at("budget_bytes").get<uint64_t>();
                }
                tier.min_free_fraction = entry.value("min_free_fraction", tier.min_free_fraction);
                tier.high_watermark = entry.value("high_watermark", tier.high_watermark);
                tier.low_watermark = entry.value("low_watermark", tier.low_watermark);
                config.migration.tiers.push_back(std::move(tier));
            }
        }
        if(input.contains("migrate_enabled"))
            config.migration.enabled = input.at("migrate_enabled").get<bool>();
        if(const auto* value = env("migrate_enabled"))
        {
            const std::string text(value);
            if(text != "true" && text != "false" && text != "1" && text != "0")
                return absl::InvalidArgumentError("invalid migrate_enabled boolean");
            config.migration.enabled = text == "true" || text == "1";
        }
        if(input.contains("compact_enabled"))
            config.compaction.enabled = input.at("compact_enabled").get<bool>();
        if(const auto* value = env("compact_enabled"))
        {
            const std::string text(value);
            if(text != "true" && text != "false" && text != "1" && text != "0")
                return absl::InvalidArgumentError("invalid compact_enabled boolean");
            config.compaction.enabled = text == "true" || text == "1";
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
    config.migration.after_s = compaction.at("migrate_after_s");
    config.migration.io_bytes_per_sec = compaction.at("migration_io_bytes_per_sec");
    for(const auto key:
        {"tier_probe_interval_ms", "tier_probe_timeout_ms", "tier_io_timeout_ms", "slow_tier_io_threads"})
        if(compaction.at(key) > UINT32_MAX)
            return absl::InvalidArgumentError("tier knob out of range");
    config.migration.probe_interval_ms = compaction.at("tier_probe_interval_ms");
    config.migration.probe_timeout_ms = compaction.at("tier_probe_timeout_ms");
    config.migration.io_timeout_ms = compaction.at("tier_io_timeout_ms");
    config.migration.io_threads = compaction.at("slow_tier_io_threads");
    config.migration.writer = config.manifest_writer.empty() ? config.process_id : config.manifest_writer;
    config.heartbeat_interval_ms = numbers.at("heartbeat_interval_ms");
    config.rpc_timeout_ms = numbers.at("rpc_timeout_ms");
    config.drain_timeout_ms = numbers.at("drain_timeout_ms");
    config.limits.chunk_bytes = numbers.at("max_chunk_bytes");
    config.limits.frame_bytes = numbers.at("max_frame_bytes");
    config.limits.concurrent_transfers = numbers.at("concurrent_transfers");
    config.compaction.scan_interval = std::chrono::seconds(compaction.at("compact_scan_interval_secs"));
    config.compaction.policy.min_files = compaction.at("compact_min_files");
    config.compaction.policy.max_files = compaction.at("compact_max_files");
    config.compaction.policy.small_file_bytes = compaction.at("compact_small_file_bytes");
    config.compaction.policy.min_age = std::chrono::seconds(compaction.at("compact_min_age_secs"));
    config.compaction.policy.max_span_ns = static_cast<int64_t>(compaction.at("compact_max_span_secs")) * 1000000000;
    config.compaction.policy.max_output_bytes = compaction.at("compact_max_output_bytes");
    config.compaction.policy.max_events = compaction.at("compact_max_events");
    config.compaction.policy.io_bytes_per_sec = compaction.at("compact_io_bytes_per_sec");
    config.compaction.policy.io_burst_bytes = compaction.at("compact_io_burst_bytes");
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
    const auto& policy = compaction.policy;
    if(compaction.scan_interval.count() <= 0 || compaction.scan_interval.count() > 86400 || policy.min_files < 2 ||
       policy.max_files < policy.min_files || policy.max_files > 4096 || policy.max_events == 0 ||
       policy.max_events > 65536 || policy.max_output_bytes == 0 || policy.max_output_bytes > 256 * 1024 * 1024 ||
       policy.small_file_bytes == 0 || policy.max_span_ns <= 0 || policy.max_span_ns > int64_t{86400} * 1000000000 ||
       policy.io_bytes_per_sec == 0 || policy.io_burst_bytes == 0)
        return absl::InvalidArgumentError("grapher compaction limits out of range");
    if(!migration.tiers.empty())
    {
        if(deployment_id.empty())
            return absl::InvalidArgumentError("tiers require deployment_id");
        if(migration.after_s <= static_cast<uint64_t>(policy.min_age.count() + compaction.scan_interval.count()))
            return absl::InvalidArgumentError(
                    "migrate_after_s must exceed compact_min_age_secs + compact_scan_interval_secs");
        std::set<std::string> names;
        uint32_t previous = 0;
        for(size_t i = 0; i < migration.tiers.size(); ++i)
        {
            const auto& tier = migration.tiers[i];
            const auto& entry = tier.config;
            if(entry.kind != "posix")
                return absl::InvalidArgumentError("tier kind must be posix; s3 is unsupported");
            if(entry.name.empty() || entry.root.empty() || entry.tier_uuid.empty() || !names.insert(entry.name).second)
                return absl::InvalidArgumentError("tier names must be unique and identities nonempty");
            if(i == 0 ? (entry.name != "local" || entry.rank != 0 ||
                         entry.root.lexically_normal() != std::filesystem::path(archive_root).lexically_normal())
                      : (entry.rank <= previous || entry.name == "local"))
                return absl::InvalidArgumentError(
                        "tiers must start with local at archive_root and strictly increasing ranks");
            previous = entry.rank;
            if(!(tier.min_free_fraction >= 0 && tier.min_free_fraction < 1 && tier.low_watermark > 0 &&
                 tier.low_watermark < tier.high_watermark && tier.high_watermark <= 1))
                return absl::InvalidArgumentError("invalid tier capacity watermarks");
            if(i > 0 && !migration.status_file.empty())
            {
                auto relative = std::filesystem::absolute(migration.status_file)
                                        .lexically_normal()
                                        .lexically_relative(std::filesystem::absolute(entry.root).lexically_normal());
                if(!relative.empty() && *relative.begin() != "..")
                    return absl::InvalidArgumentError("tier_status_file must not be under a slow tier root");
            }
        }
    }
    if(migration.after_s > static_cast<uint64_t>(INT64_MAX / 1000000000))
        return absl::InvalidArgumentError("migrate_after_s out of range");
    if(!migration.probe_interval_ms || migration.probe_interval_ms > 60000 || !migration.probe_timeout_ms ||
       migration.probe_timeout_ms > 60000 || !migration.io_timeout_ms || migration.io_timeout_ms > 60000 ||
       !migration.io_threads || migration.io_threads > 8 || !migration.io_bytes_per_sec)
        return absl::InvalidArgumentError("invalid tier worker limits");
    const auto colon = internal_listen.rfind(':');
    if(colon == std::string::npos || colon + 1 == internal_listen.size())
        return absl::InvalidArgumentError("invalid internal_listen");
    const auto host = internal_listen.substr(0, colon);
    if((host.empty() || host == "0.0.0.0" || host == "[::]" || host == "::" || host == "*") && !insecure_bind_all)
        return absl::FailedPreconditionError("wildcard internal_listen requires --insecure-bind-all");
    return absl::OkStatus();
}
TierChain GrapherConfig::tierChain() const
{
    if(migration.tiers.empty())
        return {};
    TierChain chain;
    chain.deployment_id = deployment_id;
    chain.io_threads = migration.io_threads;
    chain.io_timeout = std::chrono::milliseconds(migration.io_timeout_ms);
    for(size_t i = 1; i < migration.tiers.size(); ++i) chain.tiers.push_back(migration.tiers[i].config);
    return chain;
}
} // namespace chronolog::grapher
