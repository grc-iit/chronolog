#include <gtest/gtest.h>
#include <private/ManagedStopBarrier.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <unistd.h>
namespace
{
namespace rpc = chronolog::rpc;
using Json = std::shared_ptr<json_object>;
Json parse(std::string const& text) { return Json(json_tokener_parse(text.c_str()), json_object_put); }
Json load(std::string const& path)
{
    std::ifstream in(path);
    return parse(std::string(std::istreambuf_iterator<char>(in), {}));
}
std::string bytes(std::string const& path)
{
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
json_object* get(json_object* object, char const* key)
{
    json_object* value = nullptr;
    json_object_object_get_ex(object, key, &value);
    return value;
}
void string(json_object* object, char const* key, std::string const& value)
{
    json_object_object_add(object, key, json_object_new_string(value.c_str()));
}
void integer(json_object* object, char const* key, int64_t value)
{
    json_object_object_add(object, key, json_object_new_int64(value));
}
void write(std::string const& path, json_object* object)
{
    std::ofstream(path) << json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN);
}
class ManagedBarrier: public testing::Test
{
protected:
    std::filesystem::path work;
    std::string keeper, grapher, endpoint = "ofi+sockets://127.0.0.1:7001@1", previous, previousRoot;
    bool hadPrevious = false, hadRoot = false;
    Json keeperRecord, grapherRecord, manifest;
    rpc::NativeRetirementSnapshot native;
    void SetUp() override
    {
        if(auto old = std::getenv("CHRONOLOG_DEPLOYMENT_ID"))
        {
            hadPrevious = true;
            previous = old;
        }
        if(auto root = std::getenv("CHRONOLOG_DEPLOYMENT_ROOT"))
        {
            hadRoot = true;
            previousRoot = root;
        }
        unsetenv("CHRONOLOG_DEPLOYMENT_ROOT");
        setenv("CHRONOLOG_DEPLOYMENT_ID", "unit-managed-deployment", 1);
        char pattern[] = "/tmp/chronolog-barrier-XXXXXX";
        work = mkdtemp(pattern);
        std::filesystem::create_directories(work / "conf");
        std::filesystem::create_directories(work / "monitor/lifecycle");
        keeper = (work / "conf/keeper.json").string();
        grapher = (work / "conf/grapher.json").string();
        std::ofstream(keeper)
                << R"({"chrono_keeper":{"RecordingGroup":1,"KeeperDataStoreAdminService":{"rpc":{"protocol_conf":"ofi+sockets","service_ip":"127.0.0.1","service_base_port":7001,"service_provider_id":1}}}})";
        std::ofstream(grapher) << R"({"chrono_grapher":{"RecordingGroup":1}})";
        // Pure file-provenance fixture: this synthetic snapshot is not native
        // initialization, physical release, or daemon lifecycle evidence.
        native.generation = 7;
        keeperRecord = record(keeper, "keeper");
        grapherRecord = record(grapher, "grapher");
        manifest = parse(
                R"({"id":"unit-managed-deployment","state":"stopping","stop_attempt":"stable-stop","records":[]})");
        auto roster = get(manifest.get(), "records");
        json_object_array_add(roster, json_object_get(keeperRecord.get()));
        json_object_array_add(roster, json_object_get(grapherRecord.get()));
        write((work / "monitor/lifecycle/deployment.json").string(), manifest.get());
    }
    void TearDown() override
    {
        if(hadPrevious)
            setenv("CHRONOLOG_DEPLOYMENT_ID", previous.c_str(), 1);
        else
            unsetenv("CHRONOLOG_DEPLOYMENT_ID");
        if(hadRoot)
            setenv("CHRONOLOG_DEPLOYMENT_ROOT", previousRoot.c_str(), 1);
        else
            unsetenv("CHRONOLOG_DEPLOYMENT_ROOT");
        std::filesystem::remove_all(work);
    }
    Json record(std::string const& config, char const* role)
    {
        rpc::LifecycleStatus status(config);
        EXPECT_TRUE(status.publish(rpc::ShutdownState::Quiescing, -27, 1, rpc::RpcDeadline::Clock::now(), &native));
        auto result = load(config + ".lifecycle.json");
        string(result.get(), "component", role);
        string(result.get(), "config_bytes", bytes(config));
        return result;
    }
    bool ready()
    {
        return rpc::ManagedStopBarrier::PublishReady(keeper, 1, 7, endpoint, {true, true, true, true, true});
    }
    Json packet()
    {
        auto result = parse(
                R"({"deployment_id":"unit-managed-deployment","stop_attempt":"stable-stop","recipient_native_generation":7,"keepers":[]})");
        json_object_object_add(result.get(), "recipient", json_object_get(grapherRecord.get()));
        string(result.get(), "recipient_config_bytes", bytes(grapher));
        auto item = parse("{}");
        json_object_object_add(item.get(), "record", json_object_get(keeperRecord.get()));
        string(item.get(), "ready_bytes", bytes(keeper + ".ready.json"));
        string(item.get(), "target", endpoint);
        integer(item.get(), "group", 1);
        json_object_array_add(get(result.get(), "keepers"), json_object_get(item.get()));
        return result;
    }
    void permit()
    {
        auto value =
                parse(R"({"deployment_id":"unit-managed-deployment","stop_attempt":"stable-stop","publishers":[]})");
        json_object_object_add(value.get(), "recipient", json_object_get(keeperRecord.get()));
        string(value.get(), "recipient_config_bytes", bytes(keeper));
        string(value.get(), "ready_bytes", bytes(keeper + ".ready.json"));
        auto publisher = parse("{}");
        json_object_object_add(publisher.get(), "record", json_object_get(grapherRecord.get()));
        string(publisher.get(), "ack_path", grapher + ".park-ack.json");
        string(publisher.get(), "ack_bytes", bytes(grapher + ".park-ack.json"));
        json_object_array_add(get(value.get(), "publishers"), json_object_get(publisher.get()));
        write(keeper + ".retirement-permit.json", value.get());
    }
};
TEST_F(ManagedBarrier, EveryDataGateMustCompleteBeforeAnImmutableReadyRecordExists)
{
    for(unsigned missing = 0; missing != 5; ++missing)
    {
        bool gates[5] = {true, true, true, true, true};
        gates[missing] = false;
        EXPECT_FALSE(rpc::ManagedStopBarrier::PublishReady(keeper,
                                                           1,
                                                           7,
                                                           endpoint,
                                                           {gates[0], gates[1], gates[2], gates[3], gates[4]}));
        EXPECT_FALSE(std::filesystem::exists(keeper + ".ready.json"));
    }
    ASSERT_TRUE(ready());
    auto original = bytes(keeper + ".ready.json");
    rpc::LifecycleStatus status(keeper);
    ASSERT_TRUE(status.publish(rpc::ShutdownState::Draining, -27, 2, rpc::RpcDeadline::Clock::now(), &native));
    EXPECT_TRUE(ready());
    EXPECT_EQ(bytes(keeper + ".ready.json"), original);
}
TEST_F(ManagedBarrier, ExactReadyAndNativeCleanupAcksPermitTheSameDrainAcrossCallerAttempts)
{
    ASSERT_TRUE(ready());
    auto request = packet();
    write(grapher + ".publisher-park.json", request.get());
    auto proof = rpc::ManagedStopBarrier::LoadPublisher(grapher, 1, 7);
    ASSERT_TRUE(proof.valid()) << proof.reason();
    ASSERT_EQ(proof.requests().size(), 1u);
    EXPECT_FALSE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    ASSERT_TRUE(proof.acknowledge(proof.requests()));
    permit();
    EXPECT_TRUE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    rpc::LifecycleStatus status(keeper);
    ASSERT_TRUE(status.publish(rpc::ShutdownState::Draining, -27, 9, rpc::RpcDeadline::Clock::now(), &native));
    EXPECT_TRUE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    EXPECT_FALSE(rpc::ManagedStopBarrier::Permit(keeper, 1, 8));
}
TEST_F(ManagedBarrier, ChangedRecipientReadyConfigBootOrStopIdentityCannotPark)
{
    ASSERT_TRUE(ready());
    auto original = packet();
    for(auto field: {"stop_attempt", "recipient_config_bytes"})
    {
        auto value = parse(json_object_to_json_string(original.get()));
        string(value.get(), field, "wrong");
        write(grapher + ".publisher-park.json", value.get());
        EXPECT_FALSE(rpc::ManagedStopBarrier::LoadPublisher(grapher, 1, 7).valid());
    }
    auto value = parse(json_object_to_json_string(original.get()));
    integer(value.get(), "recipient_native_generation", 8);
    write(grapher + ".publisher-park.json", value.get());
    EXPECT_FALSE(rpc::ManagedStopBarrier::LoadPublisher(grapher, 1, 7).valid());
    value = parse(json_object_to_json_string(original.get()));
    string(get(value.get(), "recipient"), "boot_id", "restarted");
    write(grapher + ".publisher-park.json", value.get());
    EXPECT_FALSE(rpc::ManagedStopBarrier::LoadPublisher(grapher, 1, 7).valid());
    write(grapher + ".publisher-park.json", original.get());
    std::ofstream(keeper + ".ready.json", std::ios::app) << " ";
    EXPECT_FALSE(rpc::ManagedStopBarrier::LoadPublisher(grapher, 1, 7).valid());
}
TEST_F(ManagedBarrier, MissingNativeCleanupAckRestartOrIncompletePublisherRosterCannotPermit)
{
    ASSERT_TRUE(ready());
    auto request = packet();
    write(grapher + ".publisher-park.json", request.get());
    auto proof = rpc::ManagedStopBarrier::LoadPublisher(grapher, 1, 7);
    ASSERT_TRUE(proof.valid());
    ASSERT_TRUE(proof.acknowledge({}));
    permit();
    EXPECT_FALSE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    ASSERT_TRUE(proof.acknowledge(proof.requests()));
    permit();
    ASSERT_TRUE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    auto status = load(grapher + ".lifecycle.json");
    string(status.get(), "start_ticks", "new-start");
    write(grapher + ".lifecycle.json", status.get());
    EXPECT_FALSE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    write(grapher + ".lifecycle.json", grapherRecord.get());
    auto missing = parse(json_object_to_json_string(grapherRecord.get()));
    string(missing.get(), "config", grapher + "-another");
    json_object_array_add(get(manifest.get(), "records"), json_object_get(missing.get()));
    write((work / "monitor/lifecycle/deployment.json").string(), manifest.get());
    EXPECT_FALSE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
}
TEST_F(ManagedBarrier, VisorPublicProofIsEarlyStableAndDoesNotGrantFinalRetirement)
{
    auto visor = (work / "conf/visor.json").string();
    std::ofstream(visor)
            << R"({"chrono_visor":{"VisorKeeperRegistryService":{"rpc":{"protocol_conf":"ofi+sockets","service_ip":"127.0.0.1","service_base_port":8888,"service_provider_id":88}}}})";
    native.initializedMids = 2;
    auto visorRecord = record(visor, "visor");
    json_object_array_add(get(manifest.get(), "records"), json_object_get(visorRecord.get()));
    write((work / "monitor/lifecycle/deployment.json").string(), manifest.get());
    auto endpoint = "ofi+sockets://127.0.0.1:8888@88";
    EXPECT_FALSE(rpc::ManagedStopBarrier::PublishPublicQuiesced(visor, 7, endpoint, {true, false, true}));
    EXPECT_FALSE(std::filesystem::exists(visor + ".public-quiesced.json"));
    ASSERT_TRUE(rpc::ManagedStopBarrier::PublishPublicQuiesced(visor, 7, endpoint, {true, true, true}));
    // Both peers are still Quiescing and no terminal permit exists. This early
    // proof must never wait for them or stand in for physical retirement.
    EXPECT_FALSE(rpc::ManagedStopBarrier::PermitVisorRetirement(visor, 7));
    auto original = bytes(visor + ".public-quiesced.json");
    rpc::LifecycleStatus status(visor);
    ASSERT_TRUE(status.publish(rpc::ShutdownState::Draining, -27, 8, rpc::RpcDeadline::Clock::now(), &native));
    EXPECT_TRUE(rpc::ManagedStopBarrier::PublishPublicQuiesced(visor, 7, endpoint, {true, true, true}));
    EXPECT_EQ(bytes(visor + ".public-quiesced.json"), original);
    EXPECT_FALSE(rpc::ManagedStopBarrier::PermitVisorRetirement(visor, 8));
}
TEST_F(ManagedBarrier, KeeperPermitRequiresEveryManagedVisorToCloseFutureDispatch)
{
    auto visor = (work / "conf/visor.json").string();
    std::ofstream(visor)
            << R"({"chrono_visor":{"VisorKeeperRegistryService":{"rpc":{"protocol_conf":"ofi+sockets","service_ip":"127.0.0.1","service_base_port":8888,"service_provider_id":88}}}})";
    native.initializedMids = 2;
    auto visorRecord = record(visor, "visor");
    json_object_array_add(get(manifest.get(), "records"), json_object_get(visorRecord.get()));
    write((work / "monitor/lifecycle/deployment.json").string(), manifest.get());
    ASSERT_TRUE(ready());
    auto request = packet();
    write(grapher + ".publisher-park.json", request.get());
    auto proof = rpc::ManagedStopBarrier::LoadPublisher(grapher, 1, 7);
    ASSERT_TRUE(proof.valid()) << proof.reason();
    ASSERT_TRUE(proof.acknowledge(proof.requests()));
    permit();
    EXPECT_FALSE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    ASSERT_TRUE(rpc::ManagedStopBarrier::PublishPublicQuiesced(visor,
                                                               7,
                                                               "ofi+sockets://127.0.0.1:8888@88",
                                                               {true, true, true}));
    auto value = load(keeper + ".retirement-permit.json");
    auto visors = json_object_new_array();
    auto item = parse("{}");
    json_object_object_add(item.get(), "record", json_object_get(visorRecord.get()));
    string(item.get(), "proof_path", visor + ".public-quiesced.json");
    string(item.get(), "proof_bytes", bytes(visor + ".public-quiesced.json"));
    json_object_array_add(visors, json_object_get(item.get()));
    json_object_object_add(value.get(), "visors", visors);
    write(keeper + ".retirement-permit.json", value.get());
    EXPECT_TRUE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    auto changed = load(visor + ".public-quiesced.json");
    json_object_object_add(changed.get(), "keeper_dispatch_closed", json_object_new_boolean(false));
    write(visor + ".public-quiesced.json", changed.get());
    EXPECT_FALSE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
}
} // namespace

TEST_F(ManagedBarrier, ExplicitDeploymentRootBindsConfiguredPathsOutsideTheDefaultConfDirectory)
{
    auto configuration = bytes(keeper);
    auto directory = work / "separate-config" / "nested";
    std::filesystem::create_directories(directory);
    keeper = (directory / "keeper.json").string();
    std::ofstream(keeper) << configuration;
    keeperRecord = record(keeper, "keeper");
    json_object_array_put_idx(get(manifest.get(), "records"), 0, json_object_get(keeperRecord.get()));
    write((work / "monitor/lifecycle/deployment.json").string(), manifest.get());
    EXPECT_FALSE(ready());
    setenv("CHRONOLOG_DEPLOYMENT_ROOT", work.c_str(), 1);
    ASSERT_TRUE(ready());
    auto request = packet();
    write(grapher + ".publisher-park.json", request.get());
    auto proof = rpc::ManagedStopBarrier::LoadPublisher(grapher, 1, 7);
    ASSERT_TRUE(proof.valid()) << proof.reason();
    ASSERT_TRUE(proof.acknowledge(proof.requests()));
    permit();
    EXPECT_TRUE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    auto wrongRoot = work / "unrelated";
    setenv("CHRONOLOG_DEPLOYMENT_ROOT", wrongRoot.c_str(), 1);
    EXPECT_FALSE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
}

TEST_F(ManagedBarrier, EveryPlayerQueryProofRetainsThreeMidsAndBindsTheSameKeeperDrain)
{
    auto player = (work / "conf/player.json").string();
    std::ofstream(player)
            << R"({"chrono_player":{"RecordingGroup":1,"PlaybackQueryService":{"rpc":{"protocol_conf":"ofi+sockets","service_ip":"127.0.0.1","service_base_port":7003,"service_provider_id":3}}}})";
    native.initializedMids = 3;
    auto playerRecord = record(player, "player");
    json_object_array_add(get(manifest.get(), "records"), json_object_get(playerRecord.get()));
    write((work / "monitor/lifecycle/deployment.json").string(), manifest.get());
    auto request = parse(
            R"({"deployment_id":"unit-managed-deployment","stop_attempt":"stable-stop","recipient_native_generation":7})");
    json_object_object_add(request.get(), "recipient", json_object_get(playerRecord.get()));
    string(request.get(), "recipient_config_bytes", bytes(player));
    write(player + ".queries-quiesce.json", request.get());
    ASSERT_TRUE(rpc::ManagedStopBarrier::QueriesRequested(player, 1, 7));
    auto endpoint = "ofi+sockets://127.0.0.1:7003@3";
    EXPECT_FALSE(rpc::ManagedStopBarrier::PublishQueriesQuiesced(player, 1, 7, endpoint, {true, false, true}));
    ASSERT_TRUE(rpc::ManagedStopBarrier::PublishQueriesQuiesced(player, 1, 7, endpoint, {true, true, true}));
    auto original = bytes(player + ".queries-quiesced.json");
    ASSERT_TRUE(ready());
    auto batch = packet();
    write(grapher + ".publisher-park.json", batch.get());
    auto publisher = rpc::ManagedStopBarrier::LoadPublisher(grapher, 1, 7);
    ASSERT_TRUE(publisher.valid());
    ASSERT_TRUE(publisher.acknowledge(publisher.requests()));
    permit();
    EXPECT_FALSE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    auto value = load(keeper + ".retirement-permit.json");
    auto players = json_object_new_array();
    auto item = parse("{}");
    json_object_object_add(item.get(), "record", json_object_get(playerRecord.get()));
    string(item.get(), "proof_path", player + ".queries-quiesced.json");
    string(item.get(), "proof_bytes", original);
    json_object_array_add(players, json_object_get(item.get()));
    json_object_object_add(value.get(), "players", players);
    write(keeper + ".retirement-permit.json", value.get());
    EXPECT_TRUE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    rpc::LifecycleStatus playerStatus(player);
    ASSERT_TRUE(playerStatus.publish(rpc::ShutdownState::Draining, -27, 8, rpc::RpcDeadline::Clock::now(), &native));
    EXPECT_TRUE(rpc::ManagedStopBarrier::PublishQueriesQuiesced(player, 1, 7, endpoint, {true, true, true}));
    EXPECT_EQ(original, bytes(player + ".queries-quiesced.json"));
    EXPECT_TRUE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    native.finalizedMids = 1;
    ASSERT_TRUE(playerStatus.publish(rpc::ShutdownState::Draining, -27, 9, rpc::RpcDeadline::Clock::now(), &native));
    EXPECT_FALSE(rpc::ManagedStopBarrier::Permit(keeper, 1, 7));
    native.finalizedMids = 0;
    ASSERT_TRUE(playerStatus.publish(rpc::ShutdownState::Draining, -27, 10, rpc::RpcDeadline::Clock::now(), &native));
    string(request.get(), "stop_attempt", "different-drain");
    write(player + ".queries-quiesce.json", request.get());
    EXPECT_FALSE(rpc::ManagedStopBarrier::QueriesRequested(player, 1, 7));
}
TEST_F(ManagedBarrier, EmptyKeeperPartialPlayerNeedsCompleteHostInventoryAndClosedStartFence)
{
    auto player = (work / "conf/partial-player.json").string();
    std::ofstream(player) << R"({"chrono_player":{"RecordingGroup":1}})";
    native.initializedMids = 1; // Synthetic file eligibility, NOT native cleanup proof.
    auto playerRecord = record(player, "player");
    auto roster = json_object_new_array();
    json_object_array_add(roster, json_object_get(playerRecord.get()));
    json_object_object_add(manifest.get(), "records", roster);
    json_object_object_add(manifest.get(), "keepers", json_object_new_array());
    auto hosts = json_object_new_array();
    json_object_array_add(hosts, json_object_new_string("local"));
    json_object_object_add(manifest.get(), "hosts", hosts);
    json_object_object_add(manifest.get(), "stop_inventory_routes", json_object_get(hosts));
    for(auto flag: {"inventory_complete", "stop_inventory_complete", "keeper_launches_closed"})
        json_object_object_add(manifest.get(), flag, json_object_new_boolean(true));
    write((work / "monitor/lifecycle/deployment.json").string(), manifest.get());
    auto packet = parse(
            R"({"deployment_id":"unit-managed-deployment","stop_attempt":"stable-stop","recipient_native_generation":7,"services":[],"empty_keeper_roster":true,"keeper_launches_closed":true})");
    json_object_object_add(packet.get(), "recipient", json_object_get(playerRecord.get()));
    string(packet.get(), "recipient_config_bytes", bytes(player));
    write(player + ".ingestion-stop-permit.json", packet.get());
    auto lockPath = work / "monitor/lifecycle/control.lock";
    int held = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    ASSERT_GE(held, 0);
    struct Close
    {
        int fd;
        ~Close() { close(fd); }
    } closeLock{held};
    ASSERT_EQ(flock(held, LOCK_EX | LOCK_NB), 0);
    EXPECT_TRUE(rpc::ManagedStopBarrier::PermitPlayerIngestionStop(player, 1, 7));
    EXPECT_FALSE(std::filesystem::exists(player + ".queries-quiesced.json"));
    json_object_object_add(manifest.get(), "pending_launch", json_object_new_object());
    write((work / "monitor/lifecycle/deployment.json").string(), manifest.get());
    EXPECT_FALSE(rpc::ManagedStopBarrier::PermitPlayerIngestionStop(player, 1, 7));
    json_object_object_del(manifest.get(), "pending_launch");
    json_object_object_add(manifest.get(), "stop_inventory_complete", json_object_new_boolean(false));
    write((work / "monitor/lifecycle/deployment.json").string(), manifest.get());
    EXPECT_FALSE(rpc::ManagedStopBarrier::PermitPlayerIngestionStop(player, 1, 7));
    json_object_object_add(manifest.get(), "stop_inventory_complete", json_object_new_boolean(true));
    write((work / "monitor/lifecycle/deployment.json").string(), manifest.get());
    ASSERT_EQ(flock(held, LOCK_UN), 0);
    EXPECT_FALSE(rpc::ManagedStopBarrier::PermitPlayerIngestionStop(player, 1, 7));
}
