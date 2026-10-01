#include "catalog/SqliteMetadataStore.h"
#include "adapter/Convert.h"
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace chronolog::visor
{
namespace
{
namespace wire = internal::v1;
class Query
{
public:
    Query(sqlite3* db, const char* sql)
        : db_(db)
    {
        if(sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Query() { sqlite3_finalize(stmt_); }
    Query& number(int n, uint64_t v)
    {
        sqlite3_bind_int64(stmt_, n, static_cast<sqlite3_int64>(v));
        return *this;
    }
    Query& text(int n, const std::string& v)
    {
        sqlite3_bind_text(stmt_, n, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Query& blob(int n, const std::string& v)
    {
        sqlite3_bind_blob(stmt_, n, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    void reset()
    {
        if(sqlite3_reset(stmt_) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db_));
        sqlite3_clear_bindings(stmt_);
    }
    bool next()
    {
        int rc = sqlite3_step(stmt_);
        if(rc != SQLITE_ROW && rc != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(db_));
        return rc == SQLITE_ROW;
    }
    uint64_t number(int n) const { return static_cast<uint64_t>(sqlite3_column_int64(stmt_, n)); }
    std::string bytes(int n) const
    {
        const auto* p = static_cast<const char*>(sqlite3_column_blob(stmt_, n));
        return p ? std::string(p, sqlite3_column_bytes(stmt_, n)) : std::string{};
    }
    template <class T>
    T message(int n) const
    {
        T result;
        if(!result.ParseFromString(bytes(n)))
            throw std::runtime_error("invalid membership row");
        return result;
    }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_{};
};
void sql(sqlite3* db, const char* command)
{
    if(sqlite3_exec(db, command, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
uint64_t revision(sqlite3* db)
{
    Query q(db, "SELECT value FROM counters WHERE name='acquisition_revision'");
    if(!q.next())
        throw std::runtime_error("missing revision");
    return q.number(0);
}
void metadata(sqlite3* db, wire::MembershipState& state)
{
    state.set_revision(revision(db));
    Query q(db, "SELECT policy,history_floor FROM membership_meta WHERE id=1");
    if(q.next())
    {
        if(!q.bytes(0).empty())
            *state.mutable_policy() = q.message<wire::MembershipPolicy>(0);
        state.set_route_history_floor(q.number(1));
    }
}
void instances(sqlite3* db, wire::MemberState& m)
{
    Query q(db, "SELECT value FROM membership_instances WHERE process_id=?1 ORDER BY instance");
    q.text(1, m.process().process_id());
    while(q.next())
    {
        auto* i = m.add_instances();
        *i = q.message<wire::InstanceState>(0);
        Query proofs(db, "SELECT value FROM membership_proofs WHERE process_id=?1 AND instance=?2 ORDER BY story_id");
        proofs.text(1, m.process().process_id()).text(2, i->instance());
        while(proofs.next()) *i->add_proofs() = proofs.message<wire::InstanceProof>(0);
    }
}
using Refs = std::set<std::pair<std::string, int>>;
Refs refs(const wire::RouteUpdate& r)
{
    Refs result;
    for(const auto& k: r.route().keepers()) result.emplace(k.process_id(), 0);
    for(const auto& p: r.predecessors()) result.emplace(p.keeper().process_id(), 1);
    return result;
}
void writeRoute(sqlite3* db, const wire::RouteUpdate& r, const wire::RouteUpdate* old)
{
    Query q(db,
            "INSERT INTO membership_routes(story_id,revision,keeper_count,value) VALUES(?1,?2,?3,?4) "
            "ON CONFLICT(story_id) DO UPDATE SET "
            "revision=excluded.revision,keeper_count=excluded.keeper_count,value=excluded.value");
    q.number(1, r.story_id())
            .number(2, r.revision())
            .number(3, r.route().keepers_size())
            .blob(4, r.SerializeAsString())
            .next();
    auto previous = old ? refs(*old) : Refs{};
    auto current = refs(r);
    for(const auto& [id, kind]: previous)
        if(!current.contains({id, kind}))
        {
            Query d(db, "DELETE FROM membership_route_refs WHERE process_id=?1 AND story_id=?2 AND kind=?3");
            d.text(1, id).number(2, r.story_id()).number(3, kind).next();
        }
    Query insert(db, "INSERT OR IGNORE INTO membership_route_refs(process_id,story_id,kind) VALUES(?1,?2,?3)");
    for(const auto& [id, kind]: current)
        if(!previous.contains({id, kind}))
        {
            insert.text(1, id).number(2, r.story_id()).number(3, kind).next();
            insert.reset();
        }
}
void writeChanges(sqlite3* db, const wire::MembershipState& before, const wire::MembershipState& after)
{
    std::unordered_map<std::string, const wire::MemberState*> oldMembers;
    for(const auto& m: before.members()) oldMembers.emplace(m.process().process_id(), &m);
    for(const auto& m: after.members())
    {
        const auto id = m.process().process_id();
        auto meta = m;
        meta.clear_instances();
        const auto previous = oldMembers.find(id);
        std::string oldMeta;
        if(previous != oldMembers.end())
        {
            auto p = *previous->second;
            p.clear_instances();
            oldMeta = p.SerializeAsString();
        }
        if(meta.SerializeAsString() != oldMeta)
        {
            Query q(db,
                    "INSERT INTO membership_members(process_id,instance,role,joined,applied,value) "
                    "VALUES(?1,?2,?3,?4,?5,?6) "
                    "ON CONFLICT(process_id) DO UPDATE SET "
                    "instance=excluded.instance,role=excluded.role,joined=excluded.joined,applied=excluded.applied,"
                    "value=excluded.value");
            q.text(1, id)
                    .text(2, m.process().instance())
                    .number(3, m.process().role())
                    .number(4, m.joined())
                    .number(5, m.applied_route_revision())
                    .blob(6, meta.SerializeAsString())
                    .next();
        }
        std::unordered_map<std::string, const wire::InstanceState*> oldInstances;
        if(previous != oldMembers.end())
            for(const auto& i: previous->second->instances()) oldInstances.emplace(i.instance(), &i);
        for(const auto& i: m.instances())
        {
            auto scalar = i;
            scalar.clear_proofs();
            auto old = oldInstances.find(i.instance());
            std::string oldScalar;
            if(old != oldInstances.end())
            {
                auto p = *old->second;
                p.clear_proofs();
                oldScalar = p.SerializeAsString();
            }
            if(scalar.SerializeAsString() != oldScalar)
            {
                Query q(db,
                        "INSERT INTO membership_instances(process_id,instance,granted,value) VALUES(?1,?2,?3,?4) "
                        "ON CONFLICT(process_id,instance) DO UPDATE SET granted=excluded.granted,value=excluded.value");
                q.text(1, id).text(2, i.instance()).number(3, i.granted()).blob(4, scalar.SerializeAsString()).next();
            }
            std::unordered_map<uint64_t, std::string> oldProofs;
            if(old != oldInstances.end())
                for(const auto& p: old->second->proofs()) oldProofs.emplace(p.story_id(), p.SerializeAsString());
            for(const auto& p: i.proofs())
                if(!oldProofs.contains(p.story_id()) || oldProofs[p.story_id()] != p.SerializeAsString())
                {
                    Query q(db,
                            "INSERT INTO membership_proofs(process_id,instance,story_id,value) VALUES(?1,?2,?3,?4) "
                            "ON CONFLICT(process_id,instance,story_id) DO UPDATE SET value=excluded.value");
                    q.text(1, id).text(2, i.instance()).number(3, p.story_id()).blob(4, p.SerializeAsString()).next();
                }
        }
    }
    std::unordered_map<uint64_t, const wire::RouteUpdate*> oldRoutes;
    for(const auto& r: before.routes()) oldRoutes.emplace(r.story_id(), &r);
    for(const auto& r: after.routes())
    {
        auto old = oldRoutes.find(r.story_id());
        if(old == oldRoutes.end() || old->second->SerializeAsString() != r.SerializeAsString())
            writeRoute(db, r, old == oldRoutes.end() ? nullptr : old->second);
    }
    for(const auto& r: after.route_history())
    {
        Query q(db, "INSERT OR IGNORE INTO membership_history(revision,story_id,value) VALUES(?1,?2,?3)");
        q.number(1, r.revision()).number(2, r.story_id()).blob(3, r.SerializeAsString()).next();
    }
    Query meta(db,
               "INSERT INTO membership_meta(id,policy,history_floor) VALUES(1,?1,?2) "
               "ON CONFLICT(id) DO UPDATE SET policy=excluded.policy,history_floor=excluded.history_floor "
               "WHERE policy<>excluded.policy OR history_floor<>excluded.history_floor");
    meta.blob(1, after.has_policy() ? after.policy().SerializeAsString() : std::string{})
            .number(2, after.route_history_floor())
            .next();
}
} // namespace
#define MEMBERSHIP_CATCH                                                                                               \
    catch(const std::exception& e) { return absl::InternalError(e.what()); }

absl::Status SqliteMetadataStore::initializeMembership()
try
{
    sql(db_,
        "CREATE TABLE IF NOT EXISTS physical_constants(id INTEGER PRIMARY KEY,version INTEGER NOT NULL,acceptance "
        "INTEGER NOT NULL,skew INTEGER NOT NULL,lead INTEGER NOT NULL,cap INTEGER NOT NULL);"
        "INSERT OR IGNORE INTO physical_constants VALUES(1,1,15000000000,60000000000,61000000000,1000000000);"
        "CREATE TABLE IF NOT EXISTS static_physical_members(process_id TEXT PRIMARY KEY,version INTEGER NOT NULL);");
    sql(db_,
        "CREATE TABLE IF NOT EXISTS membership_meta(id INTEGER PRIMARY KEY,policy BLOB NOT NULL,history_floor INTEGER "
        "NOT NULL);"
        "CREATE TABLE IF NOT EXISTS membership_members(process_id TEXT PRIMARY KEY,instance TEXT NOT NULL,role INTEGER "
        "NOT NULL,joined INTEGER NOT NULL,applied INTEGER NOT NULL,value BLOB NOT NULL);"
        "CREATE INDEX IF NOT EXISTS membership_applied ON membership_members(role,applied) WHERE instance<>'';"
        "CREATE TABLE IF NOT EXISTS membership_instances(process_id TEXT NOT NULL,instance TEXT NOT NULL,granted "
        "INTEGER NOT NULL,value BLOB "
        "NOT NULL,PRIMARY KEY(process_id,instance));"
        "CREATE TABLE IF NOT EXISTS membership_proofs(process_id TEXT NOT NULL,instance TEXT NOT NULL,story_id INTEGER "
        "NOT NULL,value BLOB NOT NULL,PRIMARY KEY(process_id,instance,story_id));"
        "CREATE TABLE IF NOT EXISTS membership_routes(story_id INTEGER PRIMARY KEY,revision INTEGER NOT "
        "NULL,keeper_count INTEGER NOT NULL,value BLOB NOT NULL);"
        "CREATE TABLE IF NOT EXISTS membership_route_refs(process_id TEXT NOT NULL,story_id INTEGER NOT NULL,kind "
        "INTEGER NOT NULL,PRIMARY KEY(process_id,kind,story_id));"
        "CREATE INDEX IF NOT EXISTS membership_refs_story ON membership_route_refs(story_id);"
        "CREATE TABLE IF NOT EXISTS membership_history(revision INTEGER NOT NULL,story_id INTEGER NOT NULL,value BLOB "
        "NOT NULL,PRIMARY KEY(revision,story_id));");
    bool hasGranted = false;
    {
        Query columns(db_, "PRAGMA table_info(membership_instances)");
        while(columns.next())
            if(columns.bytes(1) == "granted")
                hasGranted = true;
    }
    if(!hasGranted)
    {
        sql(db_, "ALTER TABLE membership_instances ADD COLUMN granted INTEGER NOT NULL DEFAULT 0");
        Query rows(db_, "SELECT process_id,instance,value FROM membership_instances ORDER BY process_id,instance");
        while(rows.next())
        {
            const auto value = rows.message<wire::InstanceState>(2);
            Query update(db_, "UPDATE membership_instances SET granted=?3 WHERE process_id=?1 AND instance=?2");
            update.text(1, rows.bytes(0)).text(2, rows.bytes(1)).number(3, value.granted()).next();
        }
    }
    sql(db_,
        "CREATE INDEX IF NOT EXISTS membership_granted ON membership_instances(process_id,instance) WHERE granted=1");
    Query ready(db_, "SELECT id FROM membership_meta WHERE id=1");
    if(!ready.next())
    {
        Query legacy(db_, "SELECT value FROM membership_state LIMIT 1");
        wire::MembershipState state;
        if(legacy.next())
            state = legacy.message<wire::MembershipState>(0);
        else
            for(const auto& keeper: topology_.keepers)
            {
                auto* member = state.add_members();
                member->set_joined(true);
                member->mutable_process()->set_process_id(keeper.process_id);
                member->mutable_process()->set_endpoint(keeper.endpoint);
                member->mutable_process()->set_role(wire::PROCESS_ROLE_KEEPER);
            }
        writeChanges(db_, {}, state);
        sql(db_, "DELETE FROM membership_state");
    }
    Query stories(db_,
                  "SELECT id FROM stories WHERE tombstoned=0 AND id NOT IN(SELECT story_id FROM membership_routes) "
                  "ORDER BY id");
    while(stories.next())
    {
        auto status = seedMembershipStory(stories.number(0));
        if(!status.ok())
            return status;
    }
    return absl::OkStatus();
}
MEMBERSHIP_CATCH

absl::StatusOr<PhysicalPolicy> SqliteMetadataStore::physicalPolicy() const
try
{
    std::lock_guard lock(mutex_);
    Query q(db_, "SELECT version,acceptance,skew,lead,cap FROM physical_constants WHERE id=1");
    if(!q.next())
        return absl::UnavailableError("physical policy is missing");
    PhysicalPolicy policy;
    policy.version = q.number(0);
    policy.acceptance_window_ns = static_cast<int64_t>(q.number(1));
    policy.skew_limit_ns = static_cast<int64_t>(q.number(2));
    policy.hlc_lead_ns = static_cast<int64_t>(q.number(3));
    policy.uncertainty_cap_ns = q.number(4);
    return policy;
}
MEMBERSHIP_CATCH

absl::Status SqliteMetadataStore::clearPhysicalPolicy(const std::vector<StoryId>& stories)
try
{
    std::lock_guard lock(mutex_);
    sql(db_, "BEGIN IMMEDIATE");
    try
    {
        for(const auto story: stories)
        {
            auto old = membershipRouteUpdate(story);
            if(absl::IsNotFound(old.status()))
                continue;
            if(!old.ok())
                throw std::runtime_error(std::string(old.status().message()));
            if(!old->physical_policy())
                continue;
            auto route = *old;
            route.set_physical_policy(false);
            writeRoute(db_, route, &*old);
        }
        sql(db_, "COMMIT");
    }
    catch(...)
    {
        sql(db_, "ROLLBACK");
        throw;
    }
    return absl::OkStatus();
}
MEMBERSHIP_CATCH

absl::Status SqliteMetadataStore::registerStaticPolicy(const std::string& process, uint64_t version)
try
{
    std::lock_guard lock(mutex_);
    sql(db_, "BEGIN IMMEDIATE");
    try
    {
        Query entry(db_,
                    "INSERT INTO static_physical_members VALUES(?1,?2) ON CONFLICT(process_id) DO UPDATE SET "
                    "version=excluded.version");
        entry.text(1, process).number(2, version).next();
        if(version != 1)
        {
            auto before = membershipState();
            if(!before.ok())
                throw std::runtime_error(std::string(before.status().message()));
            auto after = *before;
            for(auto& r: *after.mutable_routes())
                for(const auto& k: r.route().keepers())
                    if(k.process_id() == process)
                        r.set_physical_policy(false);
            writeChanges(db_, *before, after);
        }
        sql(db_, "COMMIT");
    }
    catch(...)
    {
        sql(db_, "ROLLBACK");
        throw;
    }
    return absl::OkStatus();
}
MEMBERSHIP_CATCH

absl::Status SqliteMetadataStore::seedMembershipStory(StoryId id)
try
{
    auto story = getStory(id);
    if(!story.ok())
        return story.status();
    wire::MembershipState state;
    metadata(db_, state);
    wire::RouteUpdate route;
    route.set_story_id(id);
    route.set_revision(state.revision());
    *route.mutable_route() = convert::toProto(topology_.routeFor(story->epoch, id));
    if(state.has_policy())
    {
        route.mutable_route()->clear_keepers();
        Query members(db_, "SELECT value FROM membership_members WHERE joined=1 ORDER BY process_id");
        bool physical = true;
        while(members.next())
        {
            auto m = members.message<wire::MemberState>(0);
            if(m.process().role() != wire::PROCESS_ROLE_KEEPER)
                continue;
            auto* k = route.mutable_route()->add_keepers();
            k->set_process_id(m.process().process_id());
            k->set_endpoint(m.process().endpoint());
            physical = physical && m.policy_version() == state.policy().version();
        }
        route.set_physical_policy(physical && !route.route().keepers().empty());
    }
    else
    {
        bool physical = !route.route().keepers().empty();
        for(const auto& k: route.route().keepers())
        {
            Query policy(db_, "SELECT version FROM static_physical_members WHERE process_id=?1");
            policy.text(1, k.process_id());
            physical = physical && policy.next() && policy.number(0) == 1;
            Query exists(db_, "SELECT 1 FROM membership_members WHERE process_id=?1");
            exists.text(1, k.process_id());
            if(exists.next())
                continue;
            auto* m = state.add_members();
            m->set_joined(true);
            m->mutable_process()->set_process_id(k.process_id());
            m->mutable_process()->set_endpoint(k.endpoint());
            m->mutable_process()->set_role(wire::PROCESS_ROLE_KEEPER);
        }
        route.set_physical_policy(physical);
    }
    *state.add_routes() = route;
    if(route.revision() > state.route_history_floor())
        *state.add_route_history() = route;
    writeChanges(db_, {}, state);
    return absl::OkStatus();
}
MEMBERSHIP_CATCH

absl::StatusOr<uint64_t> SqliteMetadataStore::membershipRevision() const
try
{
    std::lock_guard lock(mutex_);
    return revision(db_);
}
MEMBERSHIP_CATCH

absl::StatusOr<wire::MembershipState> SqliteMetadataStore::membershipLivenessState() const
try
{
    std::lock_guard lock(mutex_);
    wire::MembershipState state;
    metadata(db_, state);
    Query q(db_, "SELECT value FROM membership_members ORDER BY process_id");
    while(q.next()) *state.add_members() = q.message<wire::MemberState>(0);
    return state;
}
MEMBERSHIP_CATCH

absl::StatusOr<wire::MembershipState> SqliteMetadataStore::membershipRouteChanges(uint64_t cursor) const
try
{
    std::lock_guard lock(mutex_);
    wire::MembershipState state;
    metadata(db_, state);
    if(cursor < state.route_history_floor())
    {
        Query q(db_,
                "SELECT r.value,s.epoch FROM membership_routes r JOIN stories s ON s.id=r.story_id WHERE "
                "s.tombstoned=0 ORDER BY r.story_id");
        while(q.next())
        {
            auto* r = state.add_routes();
            *r = q.message<wire::RouteUpdate>(0);
            r->mutable_route()->set_epoch(q.number(1));
        }
    }
    else
    {
        Query q(db_, "SELECT value FROM membership_history WHERE revision>?1 ORDER BY revision,story_id");
        q.number(1, cursor);
        while(q.next()) *state.add_route_history() = q.message<wire::RouteUpdate>(0);
    }
    return state;
}
MEMBERSHIP_CATCH

absl::StatusOr<wire::RouteUpdate> SqliteMetadataStore::membershipRouteUpdate(StoryId id) const
try
{
    std::lock_guard lock(mutex_);
    Query q(db_,
            "SELECT r.value,s.epoch FROM membership_routes r JOIN stories s ON s.id=r.story_id WHERE r.story_id=?1 AND "
            "s.tombstoned=0");
    q.number(1, id);
    if(!q.next())
        return absl::NotFoundError("unknown membership route");
    auto r = q.message<wire::RouteUpdate>(0);
    r.mutable_route()->set_epoch(q.number(1));
    return r;
}
MEMBERSHIP_CATCH

absl::StatusOr<wire::MembershipState> SqliteMetadataStore::membershipState() const
try
{
    std::lock_guard lock(mutex_);
    auto loaded = membershipLivenessState();
    if(!loaded.ok())
        return loaded.status();
    auto state = *loaded;
    for(auto& m: *state.mutable_members()) instances(db_, m);
    Query routes(db_,
                 "SELECT r.value,s.epoch FROM membership_routes r JOIN stories s ON s.id=r.story_id WHERE "
                 "s.tombstoned=0 ORDER BY r.story_id");
    while(routes.next())
    {
        auto* r = state.add_routes();
        *r = routes.message<wire::RouteUpdate>(0);
        r->mutable_route()->set_epoch(routes.number(1));
    }
    Query history(db_, "SELECT value FROM membership_history ORDER BY revision,story_id");
    while(history.next()) *state.add_route_history() = history.message<wire::RouteUpdate>(0);
    return state;
}
MEMBERSHIP_CATCH

absl::Status SqliteMetadataStore::saveMembership(const wire::MembershipState& state)
try
{
    std::lock_guard lock(mutex_);
    const bool own = sqlite3_get_autocommit(db_);
    if(own)
        sql(db_, "BEGIN IMMEDIATE");
    try
    {
        sql(db_,
            "DELETE FROM membership_members; DELETE FROM membership_instances; DELETE FROM membership_proofs; DELETE "
            "FROM membership_routes; DELETE FROM membership_route_refs; DELETE FROM membership_history; DELETE FROM "
            "membership_meta;");
        writeChanges(db_, {}, state);
        if(own)
            sql(db_, "COMMIT");
    }
    catch(...)
    {
        if(own)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    return absl::OkStatus();
}
MEMBERSHIP_CATCH

absl::Status SqliteMetadataStore::saveMembershipChanges(const wire::MembershipState& before,
                                                        wire::MembershipState& after)
try
{
    std::lock_guard lock(mutex_);
    writeChanges(db_, before, after);
    Query minimum(db_, "SELECT MIN(applied) FROM membership_members WHERE role=?1 AND instance<>''");
    minimum.number(1, wire::PROCESS_ROLE_KEEPER);
    uint64_t floor = after.revision();
    if(minimum.next() && !minimum.bytes(0).empty())
        floor = std::min(floor, minimum.number(0));
    floor = std::max(floor, after.route_history_floor());
    after.set_route_history_floor(floor);
    Query update(db_, "UPDATE membership_meta SET history_floor=?1 WHERE id=1 AND history_floor<?1");
    update.number(1, floor).next();
    Query trim(db_, "DELETE FROM membership_history WHERE revision<=?1");
    trim.number(1, floor).next();
    return absl::OkStatus();
}
MEMBERSHIP_CATCH

absl::StatusOr<wire::MembershipState>
SqliteMetadataStore::membershipCommandState(const wire::MembershipCommand& command) const
try
{
    std::lock_guard lock(mutex_);
    wire::MembershipState state;
    metadata(db_, state);
    std::string id;
    if(command.has_register_())
        id = command.register_().process().process_id();
    else if(command.has_extend())
        id = command.extend().process_id();
    else if(command.has_heartbeat())
        id = command.heartbeat().process_id();
    else if(command.has_drain())
        id = command.drain().process_id();
    else if(command.has_join())
        id = command.join().process_id();
    else if(command.has_abandon())
        id = command.abandon().process_id();
    std::set<std::string> loaded;
    auto loadMember = [&](const std::string& process, bool evidence)
    {
        if(!loaded.insert(process).second)
            return;
        Query member(db_, "SELECT value FROM membership_members WHERE process_id=?1");
        member.text(1, process);
        if(!member.next())
            return;
        auto* m = state.add_members();
        *m = member.message<wire::MemberState>(0);
        std::string requested;
        if(process == id)
        {
            if(command.has_register_())
                requested = command.register_().process().instance();
            if(command.has_extend())
                requested = command.extend().instance();
            if(command.has_heartbeat())
                requested = command.heartbeat().instance();
        }
        std::set<std::string> seen;
        auto loadInstance = [&](Query& rows)
        {
            while(rows.next())
            {
                auto value = rows.message<wire::InstanceState>(0);
                if(!seen.insert(value.instance()).second)
                    continue;
                auto* i = m->add_instances();
                *i = value;
                if(evidence && command.has_heartbeat() && i->instance() == command.heartbeat().instance())
                {
                    std::set<uint64_t> proofs;
                    for(const auto& f: command.heartbeat().story_frontiers())
                        if(proofs.insert(f.story_id()).second)
                        {
                            Query p(db_,
                                    "SELECT value FROM membership_proofs WHERE process_id=?1 AND instance=?2 AND "
                                    "story_id=?3");
                            p.text(1, process).text(2, i->instance()).number(3, f.story_id());
                            if(p.next())
                                *i->add_proofs() = p.message<wire::InstanceProof>(0);
                        }
                }
            }
        };
        Query current(db_, "SELECT value FROM membership_instances WHERE process_id=?1 AND instance=?2");
        current.text(1, process).text(2, m->process().instance());
        loadInstance(current);
        if(!requested.empty())
        {
            Query requestedRow(db_, "SELECT value FROM membership_instances WHERE process_id=?1 AND instance=?2");
            requestedRow.text(1, process).text(2, requested);
            loadInstance(requestedRow);
        }
        Query granted(
                db_,
                "SELECT value FROM membership_instances WHERE process_id=?1 AND granted=1 ORDER BY instance LIMIT 1");
        granted.text(1, process);
        loadInstance(granted);
    };
    loadMember(id, true);
    // Repeated-field growth can invalidate pointers; take a value before loading peers.
    wire::MemberState target;
    if(!state.members().empty())
        target = state.members(0);
    bool all = command.has_join() && !target.joined();
    bool affected = command.has_drain() || command.has_abandon();
    bool predecessors = command.has_abandon();
    if(command.has_register_())
    {
        const auto& q = command.register_();
        affected = (!target.process().instance().empty() && target.process().instance() != q.process().instance()) ||
                   (target.policy_version() != q.policy_version() &&
                    q.policy_version() != (state.has_policy() ? state.policy().version() : 1));
        predecessors = !q.recovered_instance().empty();
    }
    if(command.has_extend() && command.extend().applied_route_revision() < target.fence_revision())
    {
        if(command.extend().applied_route_revision() < state.route_history_floor())
            all = true;
        else
        {
            Query history(db_,
                          "SELECT value FROM membership_history WHERE revision>?1 AND revision<=?2 ORDER BY "
                          "revision,story_id");
            history.number(1, command.extend().applied_route_revision()).number(2, target.fence_revision());
            while(history.next()) *state.add_route_history() = history.message<wire::RouteUpdate>(0);
        }
    }
    std::set<uint64_t> stories;
    if(all)
    {
        Query q(db_, "SELECT id FROM stories WHERE tombstoned=0 ORDER BY id");
        while(q.next()) stories.insert(q.number(0));
    }
    else if(affected)
    {
        Query q(db_, "SELECT story_id FROM membership_route_refs WHERE process_id=?1 AND kind<=?2 ORDER BY story_id");
        q.text(1, id).number(2, predecessors ? 1 : 0);
        while(q.next()) stories.insert(q.number(0));
    }
    if(command.has_heartbeat())
    {
        for(const auto& f: command.heartbeat().story_frontiers())
            if(!f.drained_instance().empty())
                stories.insert(f.story_id());
        for(const auto story: command.heartbeat().stories_without_physical_policy()) stories.insert(story);
    }
    for(auto story: stories)
    {
        auto route = membershipRouteUpdate(story);
        if(absl::IsNotFound(route.status()))
            continue;
        if(!route.ok())
            return route.status();
        *state.add_routes() = *route;
        for(const auto& k: route->route().keepers()) loadMember(k.process_id(), false);
    }
    if(command.has_abandon())
        for(auto& m: *state.mutable_members())
            if(m.process().process_id() == id)
            {
                std::set<std::pair<std::string, uint64_t>> proofs;
                for(const auto& r: state.routes())
                {
                    std::set<std::string> owners;
                    for(const auto& k: r.route().keepers())
                        if(k.process_id() == id)
                            owners.insert(m.process().instance());
                    for(const auto& p: r.predecessors())
                        if(p.keeper().process_id() == id)
                            owners.insert(p.instance());
                    for(const auto& owner: owners)
                    {
                        wire::InstanceState* instance = nullptr;
                        for(auto& i: *m.mutable_instances())
                            if(i.instance() == owner)
                                instance = &i;
                        if(!instance)
                        {
                            Query q(db_, "SELECT value FROM membership_instances WHERE process_id=?1 AND instance=?2");
                            q.text(1, id).text(2, owner);
                            if(q.next())
                            {
                                instance = m.add_instances();
                                *instance = q.message<wire::InstanceState>(0);
                            }
                        }
                        if(instance && proofs.emplace(owner, r.story_id()).second)
                        {
                            Query q(db_,
                                    "SELECT value FROM membership_proofs WHERE process_id=?1 AND instance=?2 AND "
                                    "story_id=?3");
                            q.text(1, id).text(2, owner).number(3, r.story_id());
                            if(q.next())
                                *instance->add_proofs() = q.message<wire::InstanceProof>(0);
                        }
                    }
                }
            }
    for(const auto& applied: command.applied_routes()) loadMember(applied.process_id(), false);
    return state;
}
MEMBERSHIP_CATCH

bool SqliteMetadataStore::membershipWouldEmpty(const std::string& id) const
{
    std::lock_guard lock(mutex_);
    Query q(db_,
            "SELECT 1 FROM membership_route_refs f JOIN membership_routes r ON r.story_id=f.story_id JOIN stories s ON "
            "s.id=r.story_id WHERE f.process_id=?1 AND f.kind=0 AND r.keeper_count=1 AND s.tombstoned=0 LIMIT 1");
    q.text(1, id);
    return q.next();
}

absl::StatusOr<std::vector<AcquisitionChange>> SqliteMetadataStore::storyAcquisitions(StoryId id) const
try
{
    std::lock_guard lock(mutex_);
    std::vector<AcquisitionChange> result;
    const auto rev = revision(db_);
    Query q(db_,
            "SELECT writer_id,incarnation,keeper_id,keeper_endpoint FROM acquisitions WHERE story_id=?1 AND released=0 "
            "ORDER BY writer_id");
    q.number(1, id);
    while(q.next())
        result.push_back({rev, id, q.number(0), q.number(1), {q.bytes(2), q.bytes(3)}, AcquisitionState::Acquired});
    return result;
}
MEMBERSHIP_CATCH
#undef MEMBERSHIP_CATCH
} // namespace chronolog::visor
