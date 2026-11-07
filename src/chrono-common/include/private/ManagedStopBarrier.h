#ifndef CHRONOLOG_MANAGED_STOP_BARRIER_H
#define CHRONOLOG_MANAGED_STOP_BARRIER_H
#include <private/StopDependencyEvidence.h>
#include <private/StopPublicationProof.h>
#include <private/LifecycleStatus.h>
#include <vector>
namespace chronolog::rpc
{
// Exact normal-code/process provenance in the same-user managed deployment.
// No elapsed grace, remote send, or peer proof settles local native work.
class ManagedStopBarrier
{
    using IO = StopDependencyEvidence;
    using Json = IO::Json;
    std::string config_, deployment_, stop_;
    uint32_t group_ = 0;
    uint64_t generation_ = 0;
    Json manifest_, input_;
    std::vector<PublisherParkProof> requests_;
    bool valid_ = false;
    std::string reason_;
    static void put(json_object* object, char const* name, std::string const& value)
    {
        json_object_object_add(object, name, json_object_new_string(value.c_str()));
    }
    static bool atomic(std::string const& path, json_object* object)
    {
        auto temporary = path + ".tmp-" + std::to_string(getpid());
        {
            std::ofstream out(temporary);
            out << json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
            out.flush();
            if(!out)
                return false;
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        return !error;
    }
    static int64_t number(json_object* object, char const* key)
    {
        auto value = IO::field(object, key);
        IO::require(json_object_is_type(value, json_type_int), "Barrier integer missing");
        return json_object_get_int64(value);
    }
    static json_object* array(json_object* object, char const* key)
    {
        auto value = IO::field(object, key);
        IO::require(json_object_is_type(value, json_type_array), "Barrier array missing");
        return value;
    }
    json_object* managed(json_object* record, char const* role) const
    {
        auto roster = array(manifest_.get(), "records");
        for(size_t i = 0; i < json_object_array_length(roster); ++i)
        {
            auto candidate = json_object_array_get_idx(roster, i);
            if(IO::key(candidate) != IO::key(record))
                continue;
            IO::identity(record, candidate);
            IO::require(IO::string(candidate, "component") == role, "Barrier role mismatch");
            IO::require(IO::string(candidate, "deployment_id") == deployment_, "Barrier roster deployment mismatch");
            IO::require(IO::string(candidate, "config_bytes") == IO::bytes(IO::string(candidate, "config")),
                        "Managed configuration changed");
            return candidate;
        }
        throw std::runtime_error("Barrier identity absent from managed roster");
    }
    uint32_t recordGroup(json_object* record) const
    {
        auto configured = IO::parse(IO::bytes(IO::string(record, "config")));
        auto role = "chrono_" + IO::string(record, "component");
        return number(IO::field(configured.get(), role.c_str()), "RecordingGroup");
    }
    std::string target(json_object* record) const
    {
        auto configured = IO::parse(IO::bytes(IO::string(record, "config")));
        auto endpoint =
                IO::field(IO::field(IO::field(configured.get(), "chrono_keeper"), "KeeperDataStoreAdminService"),
                          "rpc");
        return IO::string(endpoint, "protocol_conf") + "://" + IO::string(endpoint, "service_ip") + ":" +
               std::to_string(number(endpoint, "service_base_port")) + "@" +
               std::to_string(number(endpoint, "service_provider_id"));
    }
    std::string visorTarget(json_object* record) const
    {
        auto configured = IO::parse(IO::bytes(IO::string(record, "config")));
        auto endpoint =
                IO::field(IO::field(IO::field(configured.get(), "chrono_visor"), "VisorKeeperRegistryService"), "rpc");
        return IO::string(endpoint, "protocol_conf") + "://" + IO::string(endpoint, "service_ip") + ":" +
               std::to_string(number(endpoint, "service_base_port")) + "@" +
               std::to_string(number(endpoint, "service_provider_id"));
    }
    std::string playerTarget(json_object* record) const
    {
        auto configured = IO::parse(IO::bytes(IO::string(record, "config")));
        auto endpoint =
                IO::field(IO::field(IO::field(configured.get(), "chrono_player"), "PlaybackQueryService"), "rpc");
        return IO::string(endpoint, "protocol_conf") + "://" + IO::string(endpoint, "service_ip") + ":" +
               std::to_string(number(endpoint, "service_base_port")) + "@" +
               std::to_string(number(endpoint, "service_provider_id"));
    }
    Json queryProof(json_object* record, std::string const& encoded) const
    {
        managed(record, "player");
        auto value = IO::parse(encoded);
        IO::identity(record, value.get());
        auto config = IO::string(record, "config");
        IO::require(IO::string(value.get(), "deployment_id") == deployment_ &&
                            IO::string(value.get(), "stop_attempt") == stop_ &&
                            IO::string(value.get(), "state") == "QueriesQuiesced" &&
                            number(value.get(), "status") == 0 && number(value.get(), "native_generation") > 0 &&
                            !IO::string(value.get(), "drain_id").empty(),
                    "Player query proof identity mismatch");
        IO::require(encoded == IO::bytes(config + ".queries-quiesced.json") &&
                            IO::string(value.get(), "config_bytes") == IO::bytes(config) &&
                            IO::string(value.get(), "target") == playerTarget(record),
                    "Player query proof/config changed");
        for(auto gate: {"query_admission_closed", "query_native_work_settled", "keeper_dispatch_closed"})
            IO::require(json_object_is_type(IO::field(value.get(), gate), json_type_boolean) &&
                                json_object_get_boolean(IO::field(value.get(), gate)),
                        "Player query phase incomplete");
        auto status = IO::parse(IO::bytes(config + ".lifecycle.json"));
        IO::identity(record, status.get());
        auto native = IO::field(status.get(), "native_proof");
        IO::require(number(native, "generation") == number(value.get(), "native_generation") &&
                            number(native, "initialized_mids") == 3 && number(native, "finalized_mids") == 0,
                    "Player ingestion native dependencies changed");
        return value;
    }
    void playerProofs(json_object* packet) const
    {
        std::set<std::string> expected, observed;
        auto roster = array(manifest_.get(), "records");
        for(size_t i = 0; i < json_object_array_length(roster); ++i)
        {
            auto record = json_object_array_get_idx(roster, i);
            if(IO::string(record, "component") == "player")
                expected.insert(IO::key(record));
        }
        json_object* entries = nullptr;
        if(expected.empty() && !json_object_object_get_ex(packet, "players", &entries))
            return;
        entries = array(packet, "players");
        for(size_t i = 0; i < json_object_array_length(entries); ++i)
        {
            auto item = json_object_array_get_idx(entries, i);
            auto record = IO::field(item, "record");
            IO::require(observed.insert(IO::key(record)).second, "Repeated Player query proof");
            IO::require(IO::string(item, "proof_path") == IO::string(record, "config") + ".queries-quiesced.json",
                        "Player query proof path mismatch");
            queryProof(record, IO::string(item, "proof_bytes"));
        }
        IO::require(expected == observed, "Missing managed Player query proof");
    }
    Json publicProof(json_object* record, std::string const& encoded) const
    {
        managed(record, "visor");
        auto value = IO::parse(encoded);
        IO::identity(record, value.get());
        auto config = IO::string(record, "config");
        IO::require(IO::string(value.get(), "deployment_id") == deployment_ &&
                            IO::string(value.get(), "state") == "PublicQuiesced" &&
                            number(value.get(), "status") == 0 && number(value.get(), "native_generation") > 0 &&
                            !IO::string(value.get(), "drain_id").empty(),
                    "Visor public proof identity mismatch");
        IO::require(encoded == IO::bytes(config + ".public-quiesced.json") &&
                            IO::string(value.get(), "config_bytes") == IO::bytes(config) &&
                            IO::string(value.get(), "target") == visorTarget(record),
                    "Visor public proof/config changed");
        for(auto gate: {"public_admission_closed", "native_work_settled", "keeper_dispatch_closed"})
            IO::require(json_object_is_type(IO::field(value.get(), gate), json_type_boolean) &&
                                json_object_get_boolean(IO::field(value.get(), gate)),
                        "Visor public gate is incomplete");
        auto status = IO::parse(IO::bytes(config + ".lifecycle.json"));
        IO::identity(record, status.get());
        IO::require(number(IO::field(status.get(), "native_proof"), "generation") ==
                            number(value.get(), "native_generation"),
                    "Visor public native generation changed");
        return value;
    }
    void visorProofs(json_object* packet) const
    {
        std::set<std::string> expected, observed;
        auto roster = array(manifest_.get(), "records");
        for(size_t i = 0; i < json_object_array_length(roster); ++i)
        {
            auto record = json_object_array_get_idx(roster, i);
            if(IO::string(record, "component") == "visor")
                expected.insert(IO::key(record));
        }
        json_object* entries = nullptr;
        if(expected.empty() && !json_object_object_get_ex(packet, "visors", &entries))
            return;
        entries = array(packet, "visors");
        for(size_t i = 0; i < json_object_array_length(entries); ++i)
        {
            auto item = json_object_array_get_idx(entries, i);
            auto record = IO::field(item, "record");
            IO::require(observed.insert(IO::key(record)).second, "Repeated Visor public proof");
            IO::require(IO::string(item, "proof_path") == IO::string(record, "config") + ".public-quiesced.json",
                        "Visor public proof path mismatch");
            publicProof(record, IO::string(item, "proof_bytes"));
        }
        IO::require(expected == observed, "Missing managed Visor public proof");
    }
    void terminal(json_object* item) const
    {
        auto record = IO::field(item, "record");
        auto role = IO::string(record, "component");
        IO::require(role == "keeper" || role == "grapher" || role == "player", "Unexpected final dependency role");
        managed(record, role.c_str());
        auto absent = IO::field(item, "authoritative_absent");
        IO::require(json_object_is_type(absent, json_type_boolean) && json_object_get_boolean(absent),
                    "Final dependency process absence is unknown");
        auto config = IO::string(record, "config");
        auto encoded = IO::string(item, "terminal_bytes");
        IO::require(encoded == IO::bytes(config + ".lifecycle.json") &&
                            IO::string(item, "config_bytes") == IO::bytes(config),
                    "Final dependency record/config changed");
        auto value = IO::parse(encoded);
        IO::identity(record, value.get());
        IO::require(IO::string(value.get(), "deployment_id") == deployment_ &&
                            IO::string(value.get(), "state") == "Stopped" && number(value.get(), "status") == 0,
                    "Final dependency is not normally Stopped");
        auto native = IO::field(value.get(), "native_proof");
        auto mids = role == "grapher" ? 2 : 3;
        for(auto count: {"initialized_mids",
                         "finalized_mids",
                         "finalize_calls",
                         "finalize_returns",
                         "constructor_units_created",
                         "constructor_units_retired",
                         "pools_created",
                         "pools_retired"})
            IO::require(number(native, count) == mids, "Final dependency native MID proof incomplete");
        IO::require(number(native, "application_handles") == 0 && number(native, "primary_releases") == 1 &&
                            number(native, "streams_before_release") == 1 && number(native, "generation") > 0 &&
                            number(native, "rpc_streams_created") == number(native, "rpc_streams_retired"),
                    "Final dependency native handle proof incomplete");
        for(auto flag: {"complete", "abt_uninitialized", "same_primary"})
            IO::require(json_object_is_type(IO::field(native, flag), json_type_boolean) &&
                                json_object_get_boolean(IO::field(native, flag)),
                        "Final dependency native release incomplete");
        auto retainedBytes = IO::bytes(IO::string(item, "record_path"));
        IO::require(retainedBytes == IO::string(item, "record_bytes"), "Final identity record changed");
        auto retained = IO::parse(retainedBytes);
        IO::identity(record, retained.get());
        auto digest = IO::string(item, "terminal_sha256");
        IO::require(digest.size() == 64 && digest == IO::string(retained.get(), "terminal_sha256"),
                    "Final dependency digest mismatch");
    }
    void base(std::string const& config, uint32_t group, uint64_t generation, char const* role)
    {
        config_ = std::filesystem::canonical(config).string();
        group_ = group;
        generation_ = generation;
        auto deployment = std::getenv("CHRONOLOG_DEPLOYMENT_ID");
        IO::require(deployment && *deployment && generation, "No managed native generation");
        deployment_ = deployment;
        auto work = IO::deploymentRoot(config_);
        manifest_ = IO::parse(IO::bytes(work / "monitor/lifecycle/deployment.json"));
        IO::require(IO::string(manifest_.get(), "id") == deployment_ &&
                            IO::string(manifest_.get(), "state") == "stopping",
                    "Managed start/stop fence mismatch");
        stop_ = IO::string(manifest_.get(), "stop_attempt");
        IO::require(!stop_.empty(), "Missing stable stop identity");
        auto own = IO::parse(IO::bytes(config_ + ".lifecycle.json"));
        IO::require(LifecycleStatus(config_).matches(own.get()), "Current daemon identity mismatch");
        IO::require(number(IO::field(own.get(), "native_proof"), "generation") == static_cast<int64_t>(generation_),
                    "Current native generation mismatch");
        managed(own.get(), role);
    }
    Json ready(json_object* record, std::string const& encoded) const
    {
        managed(record, "keeper");
        auto ready = IO::parse(encoded);
        IO::identity(record, ready.get());
        IO::require(recordGroup(record) == group_ && number(ready.get(), "group") == group_, "Ready group mismatch");
        IO::require(IO::string(ready.get(), "deployment_id") == deployment_ &&
                            IO::string(ready.get(), "state") == "Ready" && number(ready.get(), "status") == 0 &&
                            number(ready.get(), "native_generation") > 0 &&
                            !IO::string(ready.get(), "drain_id").empty(),
                    "Ready identity is invalid");
        auto config = IO::string(record, "config");
        IO::require(IO::string(ready.get(), "config_bytes") == IO::bytes(config) &&
                            encoded == IO::bytes(config + ".ready.json"),
                    "Immutable Ready record/config changed");
        for(auto gate: {"write_admission_closed",
                        "admitted_ingest_complete",
                        "archive_confirmed",
                        "joins_complete",
                        "outgoing_settled"})
            IO::require(json_object_is_type(IO::field(ready.get(), gate), json_type_boolean) &&
                                json_object_get_boolean(IO::field(ready.get(), gate)),
                        "Ready data gate is incomplete");
        IO::require(IO::string(ready.get(), "target") == target(record), "Ready target endpoint mismatch");
        auto status = IO::parse(IO::bytes(config + ".lifecycle.json"));
        IO::identity(record, status.get());
        IO::require(number(IO::field(status.get(), "native_proof"), "generation") ==
                            number(ready.get(), "native_generation"),
                    "Ready native incarnation changed");
        return ready;
    }

public:
    struct ReadyConditions
    {
        bool admissionClosed = false, ingestSettled = false, archiveConfirmed = false, joinsComplete = false,
             outgoingSettled = false;
    };
    bool valid() const noexcept { return valid_; }
    std::string const& reason() const noexcept { return reason_; }
    std::vector<PublisherParkProof> const& requests() const noexcept { return requests_; }
    static bool PublishReady(std::string const& config,
                             uint32_t group,
                             uint64_t generation,
                             std::string const& endpoint,
                             ReadyConditions gates)
    {
        if(!gates.admissionClosed || !gates.ingestSettled || !gates.archiveConfirmed || !gates.joinsComplete ||
           !gates.outgoingSettled)
            return false;
        try
        {
            ManagedStopBarrier proof;
            proof.base(config, group, generation, "keeper");
            auto own = IO::parse(IO::bytes(proof.config_ + ".lifecycle.json"));
            auto record = json_object_new_object();
            Json output(record, [](json_object* value) { json_object_put(value); });
            for(auto key: {"host", "pid", "uid", "executable", "config", "start_ticks", "boot_id", "deployment_id"})
                json_object_object_add(record, key, json_object_get(IO::field(own.get(), key)));
            put(record, "component", "keeper");
            put(record, "state", "Ready");
            put(record, "config_bytes", IO::bytes(proof.config_));
            put(record, "target", endpoint);
            put(record,
                "drain_id",
                IO::string(own.get(), "boot_id") + ":" + std::to_string(getpid()) + ":" +
                        IO::string(own.get(), "start_ticks") + ":" + std::to_string(generation));
            json_object_object_add(record, "status", json_object_new_int(0));
            json_object_object_add(record, "group", json_object_new_int64(group));
            json_object_object_add(record, "native_generation", json_object_new_int64(generation));
            for(auto gate: {"write_admission_closed",
                            "admitted_ingest_complete",
                            "archive_confirmed",
                            "joins_complete",
                            "outgoing_settled"})
                json_object_object_add(record, gate, json_object_new_boolean(true));
            auto encoded = std::string(json_object_to_json_string_ext(record, JSON_C_TO_STRING_PLAIN));
            auto path = proof.config_ + ".ready.json";
            if(std::filesystem::exists(path))
                return IO::bytes(path) == encoded;
            return atomic(path, record);
        }
        catch(std::exception const&)
        {
            return false;
        }
    }
    static ManagedStopBarrier LoadPublisher(std::string const& config, uint32_t group, uint64_t generation)
    {
        ManagedStopBarrier proof;
        try
        {
            proof.base(config, group, generation, "grapher");
            proof.input_ = IO::parse(IO::bytes(proof.config_ + ".publisher-park.json"));
            auto input = proof.input_.get();
            IO::require(IO::string(input, "deployment_id") == proof.deployment_ &&
                                IO::string(input, "stop_attempt") == proof.stop_,
                        "Publisher stop identity mismatch");
            IO::require(LifecycleStatus(config).matches(IO::field(input, "recipient")) &&
                                IO::string(input, "recipient_config_bytes") == IO::bytes(proof.config_) &&
                                number(input, "recipient_native_generation") == static_cast<int64_t>(generation),
                        "Publisher incarnation mismatch");
            std::set<std::string> targets;
            auto keepers = array(input, "keepers");
            for(size_t i = 0; i < json_object_array_length(keepers); ++i)
            {
                auto item = json_object_array_get_idx(keepers, i);
                auto record = IO::field(item, "record");
                auto encoded = IO::string(item, "ready_bytes");
                auto ready = proof.ready(record, encoded);
                auto endpoint = IO::string(ready.get(), "target");
                IO::require(endpoint == IO::string(item, "target") && targets.insert(endpoint).second,
                            "Repeated/mismatched park target");
                proof.requests_.push_back(PublisherParkProof(endpoint, std::move(encoded)));
            }
            proof.valid_ = true;
        }
        catch(std::exception const& error)
        {
            proof.reason_ = error.what();
        }
        return proof;
    }
    bool acknowledge(std::vector<PublisherParkProof> const& settled) const
    {
        if(!valid_)
            return false;
        auto output = IO::parse(json_object_to_json_string_ext(input_.get(), JSON_C_TO_STRING_PLAIN));
        auto keepers = json_object_new_array();
        auto original = array(input_.get(), "keepers");
        for(auto const& proof: settled)
            for(size_t i = 0; i < json_object_array_length(original); ++i)
            {
                auto item = json_object_array_get_idx(original, i);
                if(IO::string(item, "target") == proof.target() && IO::string(item, "ready_bytes") == proof.identity())
                    json_object_array_add(keepers, json_object_get(item));
            }
        json_object_object_add(output.get(), "keepers", keepers);
        put(output.get(), "state", "Parked");
        json_object_object_add(output.get(), "status", json_object_new_int(0));
        json_object_object_add(output.get(), "native_generation", json_object_new_int64(generation_));
        return atomic(config_ + ".park-ack.json", output.get());
    }
    struct PublicConditions
    {
        bool admissionClosed = false, nativeSettled = false, keeperDispatchClosed = false;
    };
    static bool QueriesRequested(std::string const& config, uint32_t group, uint64_t generation)
    {
        try
        {
            ManagedStopBarrier proof;
            proof.base(config, group, generation, "player");
            auto packet = IO::parse(IO::bytes(proof.config_ + ".queries-quiesce.json"));
            auto input = packet.get();
            IO::require(IO::string(input, "deployment_id") == proof.deployment_ &&
                                IO::string(input, "stop_attempt") == proof.stop_ &&
                                LifecycleStatus(config).matches(IO::field(input, "recipient")) &&
                                IO::string(input, "recipient_config_bytes") == IO::bytes(proof.config_) &&
                                number(input, "recipient_native_generation") == static_cast<int64_t>(generation),
                        "Player query request incarnation mismatch");
            return true;
        }
        catch(std::exception const&)
        {
            return false;
        }
    }
    static bool PublishQueriesQuiesced(std::string const& config,
                                       uint32_t group,
                                       uint64_t generation,
                                       std::string const& endpoint,
                                       PublicConditions gates)
    {
        if(!gates.admissionClosed || !gates.nativeSettled || !gates.keeperDispatchClosed ||
           !QueriesRequested(config, group, generation))
            return false;
        try
        {
            ManagedStopBarrier proof;
            proof.base(config, group, generation, "player");
            auto own = IO::parse(IO::bytes(proof.config_ + ".lifecycle.json"));
            auto native = IO::field(own.get(), "native_proof");
            IO::require(number(native, "initialized_mids") == 3 && number(native, "finalized_mids") == 0,
                        "All Player ingestion MIDs must remain alive");
            IO::require(endpoint == proof.playerTarget(own.get()), "Player query endpoint mismatch");
            Json output(json_object_new_object(), [](json_object* value) { json_object_put(value); });
            auto record = output.get();
            for(auto key: {"host", "pid", "uid", "executable", "config", "start_ticks", "boot_id", "deployment_id"})
                json_object_object_add(record, key, json_object_get(IO::field(own.get(), key)));
            put(record, "component", "player");
            put(record, "state", "QueriesQuiesced");
            put(record, "stop_attempt", proof.stop_);
            put(record, "config_bytes", IO::bytes(proof.config_));
            put(record, "target", endpoint);
            put(record,
                "drain_id",
                "queries:" + IO::string(own.get(), "boot_id") + ":" + std::to_string(getpid()) + ":" +
                        IO::string(own.get(), "start_ticks") + ":" + std::to_string(generation) + ":" + proof.stop_);
            json_object_object_add(record, "status", json_object_new_int(0));
            json_object_object_add(record, "native_generation", json_object_new_int64(generation));
            for(auto gate: {"query_admission_closed", "query_native_work_settled", "keeper_dispatch_closed"})
                json_object_object_add(record, gate, json_object_new_boolean(true));
            auto path = proof.config_ + ".queries-quiesced.json";
            auto encoded = std::string(json_object_to_json_string_ext(record, JSON_C_TO_STRING_PLAIN));
            if(std::filesystem::exists(path))
                return IO::bytes(path) == encoded;
            return atomic(path, record);
        }
        catch(std::exception const&)
        {
            return false;
        }
    }
    static bool PermitPlayerIngestionStop(std::string const& config, uint32_t group, uint64_t generation)
    {
        try
        {
            ManagedStopBarrier proof;
            proof.base(config, group, generation, "player");
            auto packet = IO::parse(IO::bytes(proof.config_ + ".ingestion-stop-permit.json"));
            auto input = packet.get();
            IO::require(IO::string(input, "deployment_id") == proof.deployment_ &&
                                IO::string(input, "stop_attempt") == proof.stop_ &&
                                LifecycleStatus(config).matches(IO::field(input, "recipient")) &&
                                IO::string(input, "recipient_config_bytes") == IO::bytes(proof.config_) &&
                                number(input, "recipient_native_generation") == static_cast<int64_t>(generation),
                        "Player ingestion permit incarnation mismatch");
            std::set<std::string> expected, observed, keeperRoster;
            auto records = array(proof.manifest_.get(), "records");
            for(size_t i = 0; i < json_object_array_length(records); ++i)
            {
                auto record = json_object_array_get_idx(records, i);
                if(IO::string(record, "component") == "keeper")
                    expected.insert(IO::key(record));
            }
            auto keepers = array(proof.manifest_.get(), "keepers");
            for(size_t i = 0; i < json_object_array_length(keepers); ++i)
                IO::require(keeperRoster.insert(IO::key(json_object_array_get_idx(keepers, i))).second,
                            "Repeated managed Keeper roster");
            IO::require(expected == keeperRoster, "Managed Keeper roster is incomplete");
            if(expected.empty())
            {
                // This is only known failed-start cleanup. The controller must
                // own the exclusive start fence after complete host inventory;
                // original remote launch intent was retained BEFORE SSH.
                for(auto flag: {"inventory_complete", "stop_inventory_complete", "keeper_launches_closed"})
                    IO::require(json_object_is_type(IO::field(proof.manifest_.get(), flag), json_type_boolean) &&
                                        json_object_get_boolean(IO::field(proof.manifest_.get(), flag)),
                                "Start/host inventory is unproved");
                IO::require(json_object_get_boolean(IO::field(input, "empty_keeper_roster")) &&
                                    json_object_get_boolean(IO::field(input, "keeper_launches_closed")),
                            "Empty Keeper permit is unproved");
                json_object* pending = nullptr;
                IO::require(!json_object_object_get_ex(proof.manifest_.get(), "pending_launch", &pending),
                            "Original launch remains pending");
                auto hosts = array(proof.manifest_.get(), "hosts");
                IO::require(json_object_array_length(hosts) > 0 &&
                                    json_object_equal(hosts, array(proof.manifest_.get(), "stop_inventory_routes")),
                            "Authoritative host inventory changed");
                auto work = IO::deploymentRoot(proof.config_);
                int lock = open((work / "monitor/lifecycle/control.lock").c_str(), O_RDWR | O_CLOEXEC);
                IO::require(lock >= 0, "Closed-start fence is missing");
                int claimed = flock(lock, LOCK_EX | LOCK_NB);
                int error = errno;
                if(claimed == 0)
                    flock(lock, LOCK_UN);
                close(lock);
                IO::require(claimed != 0 && (error == EWOULDBLOCK || error == EAGAIN),
                            "Closed-start fence is not held");
            }
            else
                proof.queryProof(IO::field(input, "recipient"), IO::string(input, "query_quiesced_bytes"));
            auto services = array(input, "services");
            for(size_t i = 0; i < json_object_array_length(services); ++i)
            {
                auto item = json_object_array_get_idx(services, i);
                auto record = IO::field(item, "record");
                IO::require(IO::string(record, "component") == "keeper" && observed.insert(IO::key(record)).second,
                            "Unexpected or repeated ingestion dependency");
                proof.terminal(item);
            }
            IO::require(expected == observed, "Ingestion permit omits a managed Keeper");
            return true;
        }
        catch(std::exception const&)
        {
            return false;
        }
    }
    static bool PublishPublicQuiesced(std::string const& config,
                                      uint64_t generation,
                                      std::string const& endpoint,
                                      PublicConditions gates)
    {
        if(!gates.admissionClosed || !gates.nativeSettled || !gates.keeperDispatchClosed)
            return false;
        try
        {
            ManagedStopBarrier proof;
            proof.base(config, 0, generation, "visor");
            auto own = IO::parse(IO::bytes(proof.config_ + ".lifecycle.json"));
            Json output(json_object_new_object(), [](json_object* value) { json_object_put(value); });
            auto record = output.get();
            for(auto key: {"host", "pid", "uid", "executable", "config", "start_ticks", "boot_id", "deployment_id"})
                json_object_object_add(record, key, json_object_get(IO::field(own.get(), key)));
            put(record, "component", "visor");
            put(record, "state", "PublicQuiesced");
            put(record, "config_bytes", IO::bytes(proof.config_));
            put(record, "target", endpoint);
            put(record,
                "drain_id",
                "public:" + IO::string(own.get(), "boot_id") + ":" + std::to_string(getpid()) + ":" +
                        IO::string(own.get(), "start_ticks") + ":" + std::to_string(generation));
            json_object_object_add(record, "status", json_object_new_int(0));
            json_object_object_add(record, "native_generation", json_object_new_int64(generation));
            for(auto gate: {"public_admission_closed", "native_work_settled", "keeper_dispatch_closed"})
                json_object_object_add(record, gate, json_object_new_boolean(true));
            auto encoded = std::string(json_object_to_json_string_ext(record, JSON_C_TO_STRING_PLAIN));
            auto path = proof.config_ + ".public-quiesced.json";
            if(std::filesystem::exists(path))
                return IO::bytes(path) == encoded;
            IO::require(number(IO::field(own.get(), "native_proof"), "initialized_mids") == 2,
                        "Both actual Visor MIDs must exist before public proof");
            IO::require(endpoint == proof.visorTarget(own.get()), "Visor registry endpoint mismatch");
            return atomic(path, record);
        }
        catch(std::exception const&)
        {
            return false;
        }
    }
    static bool PermitVisorRetirement(std::string const& config, uint64_t generation)
    {
        try
        {
            ManagedStopBarrier proof;
            proof.base(config, 0, generation, "visor");
            auto packet = IO::parse(IO::bytes(proof.config_ + ".retirement-permit.json"));
            auto input = packet.get();
            IO::require(IO::string(input, "deployment_id") == proof.deployment_ &&
                                IO::string(input, "stop_attempt") == proof.stop_ &&
                                LifecycleStatus(config).matches(IO::field(input, "recipient")) &&
                                IO::string(input, "recipient_config_bytes") == IO::bytes(proof.config_) &&
                                number(input, "recipient_native_generation") == static_cast<int64_t>(generation),
                        "Visor final permit incarnation mismatch");
            proof.publicProof(IO::field(input, "recipient"), IO::string(input, "public_quiesced_bytes"));
            std::set<std::string> expected, observed;
            auto roster = array(proof.manifest_.get(), "records");
            for(size_t i = 0; i < json_object_array_length(roster); ++i)
            {
                auto record = json_object_array_get_idx(roster, i);
                auto role = IO::string(record, "component");
                if(role == "keeper" || role == "grapher" || role == "player")
                    expected.insert(IO::key(record));
            }
            auto services = array(input, "services");
            for(size_t i = 0; i < json_object_array_length(services); ++i)
            {
                auto item = json_object_array_get_idx(services, i);
                IO::require(observed.insert(IO::key(IO::field(item, "record"))).second, "Repeated final dependency");
                proof.terminal(item);
            }
            IO::require(expected == observed, "Final permit does not cover every managed dependency");
            return true;
        }
        catch(std::exception const&)
        {
            return false;
        }
    }
    static bool Permit(std::string const& config, uint32_t group, uint64_t generation)
    {
        try
        {
            ManagedStopBarrier proof;
            proof.base(config, group, generation, "keeper");
            auto packet = IO::parse(IO::bytes(proof.config_ + ".retirement-permit.json"));
            auto input = packet.get();
            IO::require(IO::string(input, "deployment_id") == proof.deployment_ &&
                                IO::string(input, "stop_attempt") == proof.stop_ &&
                                LifecycleStatus(config).matches(IO::field(input, "recipient")) &&
                                IO::string(input, "recipient_config_bytes") == IO::bytes(proof.config_),
                        "Permit incarnation mismatch");
            proof.visorProofs(input);
            proof.playerProofs(input);
            auto encoded = IO::string(input, "ready_bytes");
            auto ready = proof.ready(IO::field(input, "recipient"), encoded);
            IO::require(number(ready.get(), "native_generation") == static_cast<int64_t>(generation),
                        "Permit native generation mismatch");
            std::set<std::string> expected, observed;
            auto roster = array(proof.manifest_.get(), "records");
            for(size_t i = 0; i < json_object_array_length(roster); ++i)
            {
                auto record = json_object_array_get_idx(roster, i);
                if(IO::string(record, "component") == "grapher" && proof.recordGroup(record) == group)
                    expected.insert(IO::key(record));
            }
            auto publishers = array(input, "publishers");
            for(size_t i = 0; i < json_object_array_length(publishers); ++i)
            {
                auto publisher = json_object_array_get_idx(publishers, i);
                auto record = IO::field(publisher, "record");
                proof.managed(record, "grapher");
                IO::require(observed.insert(IO::key(record)).second, "Repeated publisher permit");
                auto ackBytes = IO::string(publisher, "ack_bytes");
                IO::require(ackBytes == IO::bytes(IO::string(publisher, "ack_path")) &&
                                    IO::string(publisher, "ack_path") ==
                                            IO::string(record, "config") + ".park-ack.json",
                            "Park ACK provenance changed");
                auto ack = IO::parse(ackBytes);
                IO::identity(IO::field(ack.get(), "recipient"), record);
                IO::require(IO::string(ack.get(), "deployment_id") == proof.deployment_ &&
                                    IO::string(ack.get(), "stop_attempt") == proof.stop_ &&
                                    IO::string(ack.get(), "state") == "Parked" && number(ack.get(), "status") == 0 &&
                                    IO::string(ack.get(), "recipient_config_bytes") ==
                                            IO::bytes(IO::string(record, "config")),
                            "Park ACK identity mismatch");
                auto status = IO::parse(IO::bytes(IO::string(record, "config") + ".lifecycle.json"));
                IO::identity(record, status.get());
                IO::require(number(IO::field(status.get(), "native_proof"), "generation") ==
                                            number(ack.get(), "recipient_native_generation") &&
                                    number(ack.get(), "native_generation") ==
                                            number(ack.get(), "recipient_native_generation"),
                            "Publisher native generation changed");
                bool matched = false;
                auto items = array(ack.get(), "keepers");
                for(size_t n = 0; n < json_object_array_length(items); ++n)
                {
                    auto item = json_object_array_get_idx(items, n);
                    if(IO::string(item, "ready_bytes") == encoded &&
                       IO::string(item, "target") == IO::string(ready.get(), "target"))
                        matched = true;
                }
                IO::require(matched, "Missing matching native-cleaned park ACK");
            }
            IO::require(expected == observed, "Permit does not cover every managed publisher");
            return true;
        }
        catch(std::exception const&)
        {
            return false;
        }
    }
};
} // namespace chronolog::rpc
#endif
