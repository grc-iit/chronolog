#ifndef CHRONOLOG_LIFECYCLE_STATUS_H
#define CHRONOLOG_LIFECYCLE_STATUS_H
#include <LifecycleRecord.h>
#include <NativeRuntime.h>
#include <json-c/json.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <optional>
#include <unistd.h>
namespace chronolog::rpc
{
// Construct/publish in normal code only. No signal handler logs or touches FS.
class LifecycleStatus
{
    std::string config_, executable_, boot_, ticks_, deployment_, host_;
    pid_t const pid_ = getpid();
    uid_t const uid_ = geteuid();

public:
    struct Startup
    {
        bool succeeded;
        bool interrupted;
    };
    explicit LifecycleStatus(std::string const& config)
        : config_(std::filesystem::canonical(config).string())
    {
        char host[256]{};
        if(gethostname(host, sizeof(host)) != 0)
            throw std::runtime_error("Cannot read daemon host identity");
        host_ = host;
        executable_ = std::filesystem::read_symlink("/proc/self/exe").string();
        if(auto deployment = std::getenv("CHRONOLOG_DEPLOYMENT_ID"))
            deployment_ = deployment;
        std::ifstream boot("/proc/sys/kernel/random/boot_id");
        std::getline(boot, boot_);
        std::ifstream stat("/proc/self/stat");
        std::string line, field;
        std::getline(stat, line);
        auto end = line.rfind(')');
        if(end == std::string::npos || boot_.empty())
            throw std::runtime_error("Cannot establish daemon start identity");
        std::istringstream fields(line.substr(end + 1));
        for(int i = 0; i <= 19; ++i)
            if(!(fields >> field))
                throw std::runtime_error("Cannot read daemon start ticks");
        ticks_ = field;
    }
    bool matches(json_object* object) const
    {
        auto equal = [&](char const* key, std::string const& expected)
        {
            json_object* value = nullptr;
            return json_object_object_get_ex(object, key, &value) && json_object_is_type(value, json_type_string) &&
                   expected == json_object_get_string(value);
        };
        json_object *pid = nullptr, *uid = nullptr;
        return json_object_object_get_ex(object, "pid", &pid) && json_object_get_int64(pid) == pid_ &&
               json_object_object_get_ex(object, "uid", &uid) && json_object_get_int64(uid) == uid_ &&
               equal("config", config_) && equal("host", host_) && equal("executable", executable_) &&
               equal("boot_id", boot_) && equal("start_ticks", ticks_) && equal("deployment_id", deployment_);
    }
    bool publish(ShutdownState state,
                 int status,
                 uint64_t attempt,
                 RpcDeadline::Clock::time_point entered,
                 NativeRetirementSnapshot const* native = nullptr,
                 std::size_t applicationHandles = 0,
                 std::optional<Startup> startup = std::nullopt) const
    {
        auto json = std::shared_ptr<json_object>(json_object_new_object(),
                                                 [](json_object* object) { json_object_put(object); });
        if(!json)
            return false;
        auto string = [&](char const* key, std::string const& value)
        { json_object_object_add(json.get(), key, json_object_new_string(value.c_str())); };
        auto integer = [&](char const* key, int64_t value)
        { json_object_object_add(json.get(), key, json_object_new_int64(value)); };
        integer("pid", pid_);
        integer("uid", uid_);
        string("executable", executable_);
        string("config", config_);
        string("host", host_);
        string("deployment_id", deployment_);
        string("start_ticks", ticks_);
        string("boot_id", boot_);
        string("state",
               state == ShutdownState::Running     ? "Running"
               : state == ShutdownState::Quiescing ? "Quiescing"
               : state == ShutdownState::Draining  ? "Draining"
                                                   : "Stopped");
        integer("status", status);
        if(startup)
        {
            json_object_object_add(json.get(), "startup_succeeded", json_object_new_boolean(startup->succeeded));
            json_object_object_add(json.get(), "startup_interrupted", json_object_new_boolean(startup->interrupted));
            if(state != ShutdownState::Running && native && native->generation)
                string("drain_id",
                       "drain:" + boot_ + ":" + std::to_string(pid_) + ":" + ticks_ + ":" +
                               std::to_string(native->generation));
        }
        integer("attempt", attempt);
        integer("elapsed_ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(RpcDeadline::Clock::now() - entered).count());
        if(native)
        {
            auto proof = json_object_new_object();
            auto count = [&](char const* name, uint64_t value)
            { json_object_object_add(proof, name, json_object_new_int64(value)); };
            auto flag = [&](char const* name, bool value)
            { json_object_object_add(proof, name, json_object_new_boolean(value)); };
            count("generation", native->generation);
            count("initialized_mids", native->initializedMids);
            count("finalized_mids", native->finalizedMids);
            count("finalize_calls", native->finalizeCalls);
            count("finalize_returns", native->finalizeReturns);
            count("constructor_units_created", native->constructorUnitsCreated);
            count("constructor_units_retired", native->constructorUnitsRetired);
            count("rpc_streams_created", native->rpcStreamsCreated);
            count("rpc_streams_retired", native->rpcStreamsRetired);
            count("pools_created", native->poolsCreated);
            count("pools_retired", native->poolsRetired);
            count("primary_releases", native->primaryReleases);
            count("application_handles", applicationHandles);
            json_object_object_add(proof, "streams_before_release", json_object_new_int(native->streamsBeforeRelease));
            flag("same_primary", native->samePrimary);
            flag("abt_uninitialized", native->abtUninitialized);
            flag("complete", native->complete);
            json_object_object_add(json.get(), "native_proof", proof);
        }
        auto path = config_ + ".lifecycle.json";
        auto temporary = path + ".tmp-" + std::to_string(pid_);
        {
            std::ofstream output(temporary, std::ios::trunc);
            output << json_object_to_json_string_ext(json.get(), JSON_C_TO_STRING_PLAIN) << '\n';
            output.flush();
            if(!output)
                return false;
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        return !error;
    }
};
} // namespace chronolog::rpc
#endif
