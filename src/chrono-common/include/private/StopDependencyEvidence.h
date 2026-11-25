#ifndef CHRONOLOG_STOP_DEPENDENCY_EVIDENCE_H
#define CHRONOLOG_STOP_DEPENDENCY_EVIDENCE_H
#include <json-c/json.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
namespace chronolog::rpc
{
// Only Load can establish this proof. It never settles an admitted native call.
class StopDependencyEvidence
{
    friend class ManagedStopBarrier;
    bool valid_ = false;
    std::set<std::string> targets_;
    std::string reason_;
    using Json = std::shared_ptr<json_object>;
    static std::filesystem::path deploymentRoot(std::filesystem::path const& config)
    {
        if(auto root = std::getenv("CHRONOLOG_DEPLOYMENT_ROOT"); root && *root)
            return std::filesystem::path(root);
        return config.parent_path().parent_path();
    }
    static json_object* field(json_object* object, char const* name)
    {
        json_object* value = nullptr;
        if(!object || !json_object_object_get_ex(object, name, &value))
            throw std::runtime_error(std::string("Missing stop evidence field: ") + name);
        return value;
    }
    static std::string string(json_object* object, char const* name)
    {
        auto value = field(object, name);
        if(!json_object_is_type(value, json_type_string))
            throw std::runtime_error("Stop evidence field is not a string");
        return json_object_get_string(value);
    }
    static std::string bytes(std::filesystem::path const& path)
    {
        std::ifstream input(path, std::ios::binary);
        if(!input)
            throw std::runtime_error("Cannot read stop evidence file: " + path.string());
        return std::string(std::istreambuf_iterator<char>(input), {});
    }
    static Json parse(std::string const& source)
    {
        auto result = Json(json_tokener_parse(source.c_str()),
                           [](json_object* object)
                           {
                               if(object)
                                   json_object_put(object);
                           });
        if(!result || !json_object_is_type(result.get(), json_type_object))
            throw std::runtime_error("Invalid stop evidence JSON");
        return result;
    }
    static void require(bool value, char const* reason)
    {
        if(!value)
            throw std::runtime_error(reason);
    }
    static void identity(json_object* a, json_object* b)
    {
        for(auto key: {"host", "pid", "uid", "executable", "config", "start_ticks", "boot_id"})
            require(json_object_equal(field(a, key), field(b, key)), "Stop evidence process identity mismatch");
    }

    static std::string key(json_object* record)
    {
        return string(record, "host") + "\n" + string(record, "config") + "\n" +
               std::to_string(json_object_get_int64(field(record, "pid"))) + "\n" + string(record, "start_ticks") +
               "\n" + string(record, "boot_id");
    }

public:
    bool valid() const noexcept { return valid_; }
    bool contains(std::string const& target) const { return valid_ && targets_.count(target); }
    std::string const& reason() const noexcept { return reason_; }
    static StopDependencyEvidence Load(std::string const& config, uint64_t attempt, uint32_t group)
    {
        StopDependencyEvidence proof;
        try
        {
            auto path = std::filesystem::canonical(config);
            auto work = deploymentRoot(path);
            auto input = parse(bytes(path.string() + ".stop-evidence.json"));
            auto own = parse(bytes(path.string() + ".lifecycle.json"));
            auto manifestBytes = bytes(work / "monitor/lifecycle/deployment.json");
            auto manifest = parse(manifestBytes);
            auto deployment = std::getenv("CHRONOLOG_DEPLOYMENT_ID");
            require(deployment && *deployment, "No managed deployment identity");
            require(string(input.get(), "deployment_id") == deployment && string(manifest.get(), "id") == deployment,
                    "Stop evidence deployment mismatch");
            require(string(manifest.get(), "state") == "stopping" &&
                            string(manifest.get(), "stop_attempt") == string(input.get(), "stop_attempt"),
                    "Stop evidence operation mismatch");
            require(string(input.get(), "managed_fence_bytes") == manifestBytes, "Managed deployment fence changed");
            // The controller holds this shared deployment lock while publishing
            // evidence and signaling this exact recipient. A later managed start
            // must also own it and must refuse any occupied/unverified process.
            int lock = open((work / "monitor/lifecycle/control.lock").c_str(), O_RDWR | O_CLOEXEC);
            require(lock >= 0, "Managed stop fence is missing");
            int claimed = flock(lock, LOCK_EX | LOCK_NB);
            int error = errno;
            if(claimed == 0)
                flock(lock, LOCK_UN);
            close(lock);
            require(claimed != 0 && (error == EWOULDBLOCK || error == EAGAIN), "No active managed stop fence");
            identity(field(input.get(), "recipient"), own.get());
            require(json_object_get_int64(field(own.get(), "pid")) == getpid() &&
                            json_object_get_int64(field(own.get(), "uid")) == geteuid() &&
                            string(own.get(), "config") == path.string() && string(own.get(), "state") == "Quiescing" &&
                            json_object_get_int64(field(own.get(), "attempt")) == static_cast<int64_t>(attempt) &&
                            json_object_get_int64(field(input.get(), "recipient_attempt")) ==
                                    static_cast<int64_t>(attempt),
                    "Stop evidence recipient/attempt mismatch");
            require(string(input.get(), "recipient_config_bytes") == bytes(path), "Grapher config changed");
            require(json_object_get_int64(field(input.get(), "group")) == group, "Stop evidence group mismatch");
            std::set<std::string> expected, observed;
            auto roster = field(manifest.get(), "keepers");
            require(json_object_is_type(roster, json_type_array), "Managed Keeper roster is missing");
            for(size_t i = 0; i < json_object_array_length(roster); ++i)
            {
                auto record = json_object_array_get_idx(roster, i);
                auto configured = parse(bytes(string(record, "config")));
                if(json_object_get_int64(field(field(configured.get(), "chrono_keeper"), "RecordingGroup")) == group)
                    expected.insert(key(record));
            }
            auto keepers = field(input.get(), "keepers");
            require(json_object_is_type(keepers, json_type_array), "Invalid stopped Keeper evidence list");
            for(size_t i = 0; i < json_object_array_length(keepers); ++i)
            {
                auto item = json_object_array_get_idx(keepers, i);
                auto record = field(item, "record");
                require(observed.insert(key(record)).second, "Repeated Keeper terminal evidence");
                require(json_object_get_boolean(field(item, "authoritative_absent")),
                        "Keeper process absence is unconfirmed");
                auto keeperConfig = std::filesystem::canonical(string(record, "config"));
                auto terminalBytes = bytes(keeperConfig.string() + ".lifecycle.json");
                require(terminalBytes == string(item, "terminal_bytes"), "Keeper terminal record changed");
                auto terminal = parse(terminalBytes);
                identity(record, terminal.get());
                require(string(terminal.get(), "state") == "Stopped" &&
                                json_object_get_int(field(terminal.get(), "status")) == 0 &&
                                string(terminal.get(), "deployment_id") == deployment,
                        "Keeper does not have matching normal Stopped evidence");
                // Byte identity is stronger than a digest comparison. Retain and
                // cross-check the authoritative worker's SHA256 identifier too.
                auto recordBytes = bytes(string(item, "record_path"));
                require(recordBytes == string(item, "record_bytes"), "Keeper identity record changed");
                auto retainedRecord = parse(recordBytes);
                identity(record, retainedRecord.get());
                auto digest = string(item, "terminal_sha256");
                require(digest.size() == 64 && digest == string(retainedRecord.get(), "terminal_sha256"),
                        "Keeper terminal digest mismatch");
                auto keeperBytes = bytes(keeperConfig);
                require(keeperBytes == string(item, "config_bytes"), "Keeper configuration changed");
                auto configuration = parse(keeperBytes);
                auto keeper = field(configuration.get(), "chrono_keeper");
                require(json_object_get_int64(field(keeper, "RecordingGroup")) == group,
                        "Keeper recording group mismatch");
                auto endpoint = field(field(keeper, "KeeperDataStoreAdminService"), "rpc");
                auto key = string(endpoint, "protocol_conf") + "://" + string(endpoint, "service_ip") + ":" +
                           std::to_string(json_object_get_int(field(endpoint, "service_base_port"))) + "@" +
                           std::to_string(json_object_get_int(field(endpoint, "service_provider_id")));
                require(key == string(item, "target"), "Keeper admin endpoint mismatch");
                proof.targets_.insert(std::move(key));
            }
            require(observed == expected, "Stopped evidence does not cover the managed Keeper roster");
            proof.valid_ = true;
        }
        catch(std::exception const& error)
        {
            proof.reason_ = error.what();
        }
        return proof;
    }
};
} // namespace chronolog::rpc
#endif
