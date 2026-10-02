#include "raft/RaftMetadataStore.h"
#include <cerrno>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "adapter/Convert.h"
#include "dynamic/MembershipState.h"
namespace chronolog::visor
{
using namespace nuraft;
namespace
{
ptr<buffer> bytes(const std::string& s)
{
    auto b = buffer::alloc(s.size());
    if(!s.empty())
        std::memcpy(b->data_begin(), s.data(), s.size());
    return b;
}
std::string string(const buffer& b) { return {reinterpret_cast<const char*>(b.data_begin()), b.size()}; }
absl::Status status(const v1::ItemStatus& s)
{
    return s.code() ? absl::Status(static_cast<absl::StatusCode>(s.code()), s.message()) : absl::OkStatus();
}
void requireStorage(const absl::Status& s)
{
    if(s.code() == absl::StatusCode::kUnavailable || s.code() == absl::StatusCode::kInternal)
        throw std::runtime_error(std::string(s.message()));
}
Story story(const v1::Story& s) { return {s.story_id(), s.chronicle(), s.name(), s.epoch(), s.tombstoned()}; }
KeeperRef keeper(const v1::KeeperRef& k) { return {k.process_id(), k.endpoint()}; }
Route route(const v1::Route& r)
{
    Route out;
    out.epoch = r.epoch();
    for(const auto& k: r.keepers()) out.keepers.push_back(keeper(k));
    out.grapher = r.grapher();
    out.player = r.player();
    return out;
}
std::string execute(SqliteMetadataStore& store, const internal::v1::CatalogCommand& c)
{
    switch(c.mutation_case())
    {
        case internal::v1::CatalogCommand::kMembership:
            return dynamic::apply(store, c.membership());
        case internal::v1::CatalogCommand::kCreateChronicle:
        {
            const auto& q = c.create_chronicle();
            v1::CreateChronicleResponse r;
            auto value = store.createChronicle(q.name());
            requireStorage(value.status());
            *r.mutable_status() = convert::toProto(value.status());
            if(value.ok())
                *r.mutable_chronicle() = convert::toProto(*value);
            return r.SerializeAsString();
        }
        case internal::v1::CatalogCommand::kDestroyChronicle:
        {
            const auto& q = c.destroy_chronicle();
            v1::DestroyChronicleResponse r;
            auto value = store.destroyChronicle(q.name());
            requireStorage(value);
            *r.mutable_status() = convert::toProto(value);
            return r.SerializeAsString();
        }
        case internal::v1::CatalogCommand::kCreateStory:
        {
            const auto& q = c.create_story();
            v1::CreateStoryResponse r;
            auto value = store.createStory(q.chronicle(), q.name());
            requireStorage(value.status());
            *r.mutable_status() = convert::toProto(value.status());
            if(value.ok())
            {
                *r.mutable_story() = convert::toProto(*value);
            }
            return r.SerializeAsString();
        }
        case internal::v1::CatalogCommand::kDestroyStory:
        {
            const auto& q = c.destroy_story();
            v1::DestroyStoryResponse r;
            auto value = store.destroyStory(q.story_id());
            requireStorage(value);
            *r.mutable_status() = convert::toProto(value);
            return r.SerializeAsString();
        }
        case internal::v1::CatalogCommand::kAcquire:
        {
            const auto& q = c.acquire();
            v1::AcquireResponse r;
            auto value = store.acquireAfterFence(q.story_id(), q.writer_identity());
            requireStorage(value.status());
            *r.mutable_status() = convert::toProto(value.status());
            if(value.ok())
                r = convert::toAcquireResponse(*value);
            return r.SerializeAsString();
        }
        case internal::v1::CatalogCommand::kRelease:
        {
            const auto& q = c.release();
            v1::ReleaseResponse r;
            auto value = store.release(q.story_id(), q.writer_id(), q.incarnation());
            requireStorage(value.status());
            *r.mutable_status() = convert::toProto(value.status());
            if(value.ok())
            {
                r.set_fenced(false);
                r.set_revision(value->revision);
            }
            return r.SerializeAsString();
        }
        case internal::v1::CatalogCommand::kCompareAndSetEpoch:
        {
            const auto& q = c.compare_and_set_epoch();
            v1::CompareAndSetEpochResponse r;
            auto state = store.membershipState();
            requireStorage(state.status());
            bool registered = false;
            if(state.ok())
                for(const auto& m: state->members())
                    if(!m.process().instance().empty())
                        registered = true;
            absl::StatusOr<Epoch> value = registered
                                                  ? absl::StatusOr<Epoch>(absl::FailedPreconditionError(
                                                            "use membership transition for registered Keepers"))
                                                  : store.compareAndSetEpoch(q.story_id(), q.expected(), q.desired());
            requireStorage(value.status());
            *r.mutable_status() = convert::toProto(value.status());
            if(value.ok())
                r.set_epoch(*value);
            return r.SerializeAsString();
        }
        default:
            throw std::runtime_error("empty CatalogCommand");
    }
}
void syncFile(const std::string& path)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if(fd < 0)
        throw std::runtime_error("snapshot open failed");
    int rc = fsync(fd);
    close(fd);
    if(rc)
        throw std::runtime_error("snapshot fsync failed");
}
void syncDirectory(const std::string& path)
{
    auto parent = std::filesystem::path(path).parent_path();
    int fd = ::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY);
    if(fd < 0)
        throw std::runtime_error("snapshot directory open failed");
    int rc = fsync(fd);
    close(fd);
    if(rc)
        throw std::runtime_error("snapshot directory fsync failed");
}

} // namespace
class RaftMetadataStore::Machine final: public state_machine
{
public:
    Machine(SqliteMetadataStore& store, DurableState& durable, std::string path)
        : store_(store)
        , durable_(durable)
        , path_(std::move(path))
    {}
    ptr<buffer> commit(ulong index, buffer& data) override
    {
        std::lock_guard lock(mutex_);
        internal::v1::CatalogCommand c;
        if(!c.ParseFromString(string(data)) || c.mutation_case() == internal::v1::CatalogCommand::MUTATION_NOT_SET)
            throw std::runtime_error("invalid CatalogCommand");
        auto result = store_.applyRaft(index, [&] { return execute(store_, c); });
        if(!result.ok())
            throw std::runtime_error(std::string(result.status().message()));
        return bytes(*result);
    }
    void commit_config(ulong index, ptr<cluster_config>&) override
    {
        std::lock_guard lock(mutex_);
        auto r = store_.applyRaft(index, [] { return std::string(); });
        if(!r.ok())
            throw std::runtime_error(std::string(r.status().message()));
    }
    ulong last_commit_index() override
    {
        auto r = store_.appliedIndex();
        if(!r.ok())
            throw std::runtime_error("applied index unavailable");
        return *r;
    }
    ptr<snapshot> last_snapshot() override
    {
        std::lock_guard lock(mutex_);
        auto b = durable_.get("snapshot");
        return b ? snapshot::deserialize(*b) : nullptr;
    }
    void create_snapshot(snapshot& s, async_result<bool>::handler_type& done) override
    {
        ptr<std::exception> error;
        bool ok = false;
        // done() takes raft_server's lock, which NuRaft holds while calling into this machine: call it unlocked.
        try
        {
            std::lock_guard lock(mutex_);
            if(s.get_last_log_idx() == last_commit_index())
            {
                auto result = store_.backupTo(snapshotPath(s));
                if(!result.ok())
                    throw std::runtime_error(std::string(result.message()));
                syncFile(snapshotPath(s));
                syncDirectory(snapshotPath(s));
                durable_.put("snapshot", *s.serialize());
                ok = true;
            }
        }
        catch(const std::exception& e)
        {
            error = cs_new<std::runtime_error>(e.what());
        }
        done(ok, error);
    }
    int read_logical_snp_obj(snapshot& s, void*&, ulong id, ptr<buffer>& out, bool& last) override
    {
        std::lock_guard lock(mutex_);
        std::ifstream f(snapshotPath(s), std::ios::binary);
        if(!f)
            return -1;
        f.seekg(static_cast<std::streamoff>(id * block));
        std::string chunk(block, '\0');
        f.read(chunk.data(), static_cast<std::streamsize>(block));
        chunk.resize(static_cast<size_t>(f.gcount()));
        out = bytes(chunk);
        last = f.peek() == EOF;
        return 0;
    }
    void save_logical_snp_obj(snapshot& s, ulong& id, buffer& data, bool first, bool last) override
    {
        std::lock_guard lock(mutex_);
        std::ofstream f(snapshotPath(s) + ".recv", std::ios::binary | (first ? std::ios::trunc : std::ios::app));
        auto value = string(data);
        f.write(value.data(), static_cast<std::streamsize>(value.size()));
        f.close();
        if(!f)
            throw std::runtime_error("snapshot write failed");
        if(last)
            syncFile(snapshotPath(s) + ".recv");
        ++id;
    }
    bool apply_snapshot(snapshot& s) override
    {
        std::lock_guard lock(mutex_);
        if(s.get_last_log_idx() < last_commit_index())
            return false;
        auto result = store_.installFrom(snapshotPath(s) + ".recv");
        if(!result.ok())
            return false;
        std::filesystem::rename(snapshotPath(s) + ".recv", snapshotPath(s));
        syncDirectory(snapshotPath(s));
        durable_.put("snapshot", *s.serialize());
        return true;
    }

private:
    std::string snapshotPath(snapshot& s) { return path_ + ".snapshot." + std::to_string(s.get_last_log_idx()); }
    static constexpr size_t block = 1024 * 1024;
    SqliteMetadataStore& store_;
    DurableState& durable_;
    std::string path_;
    std::recursive_mutex mutex_;
};
namespace
{
// NuRaft's launcher reports a failed listener as a null server and nothing else.
std::string listenerFailure(const std::string& endpoint, int port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
        return "raft endpoint " + endpoint + " could not start: socket: " + std::strerror(errno);
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int error = errno;
    ::close(fd);
    if(rc == 0)
        return "raft endpoint " + endpoint + " could not start: NuRaft listener failed although port " +
               std::to_string(port) + " binds";
    if(error == EADDRINUSE)
        return "raft port " + endpoint + " in use";
    return "raft endpoint " + endpoint + " could not bind: " + std::strerror(error);
}
} // namespace
RaftMetadataStore::RaftMetadataStore(std::unique_ptr<SqliteMetadataStore> store, RaftConfig config, FenceWaiter waiter)
    : store_(std::move(store))
    , config_(std::move(config))
    , fence_waiter_(std::move(waiter))
{}
absl::StatusOr<std::unique_ptr<RaftMetadataStore>>
RaftMetadataStore::open(const std::string& path, Topology topology, RaftConfig config, FenceWaiter waiter)
{
    auto store = SqliteMetadataStore::open(path, std::move(topology));
    if(!store.ok())
        return store.status();
    auto out = std::unique_ptr<RaftMetadataStore>(new RaftMetadataStore(std::move(*store), config, std::move(waiter)));
    try
    {
        out->durable_ = cs_new<DurableState>(path + ".raft", config);
        out->machine_ = cs_new<Machine>(*out->store_, *out->durable_, path);
        // NuRaft 3.0.0 uses inconsistent lost-peer thresholds with a custom leadership expiry.
        raft_params p;
        p.with_election_timeout_lower(static_cast<int32_t>(config.election_lower_ms))
                .with_election_timeout_upper(static_cast<int32_t>(config.election_upper_ms))
                .with_hb_interval(static_cast<int32_t>(config.heartbeat_ms))
                .with_client_req_timeout(3000)
                .with_snapshot_enabled(64);
        asio_service::options options;
        options.thread_pool_size_ = 2;
        auto colon = config.raft_endpoint.rfind(':');
        if(colon == std::string::npos)
            return absl::InvalidArgumentError("raft endpoint " + config.raft_endpoint + " has no port");
        int port = std::stoi(config.raft_endpoint.substr(colon + 1));
        out->server_ = out->launcher_.init(out->machine_, out->durable_, nullptr, port, options, p);
        if(!out->server_)
            return absl::UnavailableError("Raft initialization failed: " + listenerFailure(config.raft_endpoint, port));
    }
    catch(const std::exception& e)
    {
        return absl::UnavailableError("Raft initialization failed for " + config.raft_endpoint + ": " + e.what());
    }
    return out;
}
RaftMetadataStore::~RaftMetadataStore() { launcher_.shutdown(5); }
int RaftMetadataStore::leaderId() const { return server_->get_leader(); }
std::string RaftMetadataStore::leaderEndpoint(bool internal) const
{
    for(const auto& p: config_.peers)
        if(p.id == leaderId())
            return internal ? p.internal_endpoint : p.catalog_endpoint;
    return {};
}
bool RaftMetadataStore::leaderLease() const
{
    if(!server_->is_leader() || server_->get_log_idx_at_becoming_leader() == 0 ||
       store_->appliedIndex().value_or(0) < server_->get_log_idx_at_becoming_leader())
        return false;
    size_t live = 1;
    for(const auto& p: server_->get_peer_info_all())
        if(p.last_log_idx_ >= server_->get_log_idx_at_becoming_leader() &&
           p.last_succ_resp_us_ < static_cast<ulong>(config_.election_lower_ms) * 500)
            ++live;
    return live > config_.peers.size() / 2;
}
absl::StatusOr<std::string> RaftMetadataStore::propose(const internal::v1::CatalogCommand& c)
{
    if(!leaderLease())
        return absl::UnavailableError("no leader lease");
    auto result = server_->append_entries({bytes(c.SerializeAsString())});
    if(!result->get_accepted() || result->get_result_code() != cmd_result_code::OK || !result->get())
        return absl::UnavailableError("Raft proposal failed");
    return string(*result->get());
}
// A restarted replica holds only what it applied before it was killed until a leader of the new term commits.
// A watch snapshot from that state can sit below revisions a Keeper already applied, and the Keeper rejects it and
// keeps its admission gate closed for as long as the stream stays open. The state is current once it holds an entry
// of the present term that the leader has declared committed, because every earlier entry precedes that one.
bool RaftMetadataStore::appliedStateCurrent() const
{
    if(leaderLease())
        return true;
    const auto leader = server_->get_leader();
    if(leader < 0 || leader == config_.server_id || !server_->is_leader_alive())
        return false;
    auto applied = store_->appliedIndex();
    return applied.ok() && *applied >= server_->get_target_committed_log_idx() &&
           durable_->term_at(*applied) == server_->get_term();
}
absl::StatusOr<AcquisitionSnapshot> RaftMetadataStore::snapshotAcquisitions() const
{
    return store_->snapshotAcquisitions();
}
void RaftMetadataStore::setObserver(AcquisitionObserver* observer) { store_->setObserver(observer); }
absl::StatusOr<Chronicle> RaftMetadataStore::createChronicle(std::string name)
{
    internal::v1::CatalogCommand c;
    auto* q = c.mutable_create_chronicle();
    q->set_name(name);
    auto result = propose(c);
    if(!result.ok())
        return result.status();
    v1::CreateChronicleResponse r;
    if(!r.ParseFromString(*result))
        return absl::InternalError("invalid apply response");
    auto st = status(r.status());
    if(!st.ok())
        return st;
    return Chronicle{r.chronicle().name(), r.chronicle().tombstoned()};
}
absl::Status RaftMetadataStore::destroyChronicle(std::string name)
{
    internal::v1::CatalogCommand c;
    auto* q = c.mutable_destroy_chronicle();
    q->set_name(name);
    auto result = propose(c);
    if(!result.ok())
        return result.status();
    v1::DestroyChronicleResponse r;
    if(!r.ParseFromString(*result))
        return absl::InternalError("invalid apply response");
    return status(r.status());
}
absl::StatusOr<Story> RaftMetadataStore::createStory(std::string chronicle, std::string name)
{
    internal::v1::CatalogCommand c;
    auto* q = c.mutable_create_story();
    q->set_chronicle(chronicle);
    q->set_name(name);
    auto result = propose(c);
    if(!result.ok())
        return result.status();
    v1::CreateStoryResponse r;
    if(!r.ParseFromString(*result))
        return absl::InternalError("invalid apply response");
    auto st = status(r.status());
    if(!st.ok())
        return st;
    return story(r.story());
}
absl::Status RaftMetadataStore::destroyStory(StoryId id)
{
    internal::v1::CatalogCommand c;
    auto* q = c.mutable_destroy_story();
    q->set_story_id(id);
    auto result = propose(c);
    if(!result.ok())
        return result.status();
    v1::DestroyStoryResponse r;
    if(!r.ParseFromString(*result))
        return absl::InternalError("invalid apply response");
    return status(r.status());
}
absl::StatusOr<Acquisition> RaftMetadataStore::acquire(StoryId id, std::string identity)
{
    // Only the leader proposes, and apply must not wait, so the old owner's fence is awaited before the proposal.
    if(auto fenced = store_->awaitOldOwnerFence(id, identity); !fenced.ok())
        return fenced;
    internal::v1::CatalogCommand c;
    auto* q = c.mutable_acquire();
    q->set_story_id(id);
    q->set_writer_identity(identity);
    auto result = propose(c);
    if(!result.ok())
        return result.status();
    v1::AcquireResponse r;
    if(!r.ParseFromString(*result))
        return absl::InternalError("invalid apply response");
    auto st = status(r.status());
    if(!st.ok())
        return st;
    return Acquisition{id, r.writer_id(), r.incarnation(), route(r.route()), keeper(r.assigned_keeper())};
}
absl::StatusOr<ReleaseResult> RaftMetadataStore::release(StoryId id, uint64_t writer, uint64_t incarnation)
{
    internal::v1::CatalogCommand c;
    auto* q = c.mutable_release();
    q->set_story_id(id);
    q->set_writer_id(writer);
    q->set_incarnation(incarnation);
    auto result = propose(c);
    if(!result.ok())
        return result.status();
    v1::ReleaseResponse r;
    if(!r.ParseFromString(*result))
        return absl::InternalError("invalid apply response");
    auto st = status(r.status());
    if(!st.ok())
        return st;
    auto k = store_->releasedKeeper(id, writer, incarnation);
    bool fenced = k.ok() && fence_waiter_ && fence_waiter_(*k, r.revision());
    return ReleaseResult{fenced, r.revision()};
}
absl::StatusOr<Epoch> RaftMetadataStore::compareAndSetEpoch(StoryId id, Epoch expected, Epoch desired)
{
    internal::v1::CatalogCommand c;
    auto* q = c.mutable_compare_and_set_epoch();
    q->set_story_id(id);
    q->set_expected(expected);
    q->set_desired(desired);
    auto result = propose(c);
    if(!result.ok())
        return result.status();
    v1::CompareAndSetEpochResponse r;
    if(!r.ParseFromString(*result))
        return absl::InternalError("invalid apply response");
    auto st = status(r.status());
    if(!st.ok())
        return st;
    return r.epoch();
}
absl::StatusOr<Chronicle> RaftMetadataStore::getChronicle(std::string name) const
{
    if(!leaderLease())
        return absl::UnavailableError("no leader lease");
    auto term = server_->get_term();
    auto result = store_->getChronicle(name);
    if(!leaderLease() || term != server_->get_term())
        return absl::UnavailableError("leader lease expired during read");
    return result;
}
absl::StatusOr<std::vector<Chronicle>> RaftMetadataStore::listChronicles() const
{
    if(!leaderLease())
        return absl::UnavailableError("no leader lease");
    auto term = server_->get_term();
    auto result = store_->listChronicles();
    if(!leaderLease() || term != server_->get_term())
        return absl::UnavailableError("leader lease expired during read");
    return result;
}
absl::StatusOr<Story> RaftMetadataStore::getStory(StoryId id) const
{
    if(!leaderLease())
        return absl::UnavailableError("no leader lease");
    auto term = server_->get_term();
    auto result = store_->getStory(id);
    if(!leaderLease() || term != server_->get_term())
        return absl::UnavailableError("leader lease expired during read");
    return result;
}
absl::StatusOr<std::vector<Story>> RaftMetadataStore::listStories(std::string chronicle) const
{
    if(!leaderLease())
        return absl::UnavailableError("no leader lease");
    auto term = server_->get_term();
    auto result = store_->listStories(chronicle);
    if(!leaderLease() || term != server_->get_term())
        return absl::UnavailableError("leader lease expired during read");
    return result;
}
} // namespace chronolog::visor
