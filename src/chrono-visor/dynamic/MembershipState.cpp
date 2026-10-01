#include "dynamic/MembershipState.h"
#include "adapter/Convert.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <set>
#include <unordered_map>
namespace chronolog::visor::dynamic
{
namespace
{
namespace wire = internal::v1;
Hlc hlc(const v1::Hlc& h) { return {h.physical_ns(), h.logical()}; }
void set(v1::Hlc* h, Hlc value)
{
    h->set_physical_ns(value.physical_ns);
    h->set_logical(value.logical);
}
void require(const absl::Status& s)
{
    if(!s.ok())
        throw std::runtime_error(std::string(s.message()));
}
int64_t add(int64_t a, int64_t b)
{
    return a > std::numeric_limits<int64_t>::max() - b ? std::numeric_limits<int64_t>::max() : a + b;
}
wire::MemberState* member(wire::MembershipState& state, const std::string& id)
{
    for(auto& m: *state.mutable_members())
        if(m.process().process_id() == id)
            return &m;
    return nullptr;
}
wire::InstanceState* instance(wire::MemberState& m, const std::string& id)
{
    for(auto& i: *m.mutable_instances())
        if(i.instance() == id)
            return &i;
    return nullptr;
}
bool lists(const v1::Route& r, const std::string& id)
{
    for(const auto& k: r.keepers())
        if(k.process_id() == id)
            return true;
    return false;
}
Hlc maximum(const wire::MemberState& m)
{
    Hlc result{};
    for(const auto& i: m.instances()) result = std::max(result, hlc(i.ceiling()));
    return result;
}
int64_t physicalMaximum(const wire::MemberState& m)
{
    int64_t result = 0;
    for(const auto& i: m.instances()) result = std::max(result, i.physical_ceiling_ns());
    return result;
}
void predecessor(wire::RouteUpdate& r, const wire::MemberState& m, const wire::InstanceState& i)
{
    for(const auto& p: r.predecessors())
        if(p.keeper().process_id() == m.process().process_id() && p.instance() == i.instance() &&
           p.epoch() == r.route().epoch())
            return;
    auto* p = r.add_predecessors();
    p->mutable_keeper()->set_process_id(m.process().process_id());
    p->mutable_keeper()->set_endpoint(m.process().endpoint());
    p->set_instance(i.instance());
    p->set_epoch(r.route().epoch());
    *p->mutable_own_cut() = i.ceiling();
    p->set_own_physical_ceiling_ns(i.physical_ceiling_ns());
}
void defaults(wire::MembershipState& state)
{
    if(!state.has_policy())
    {
        auto* p = state.mutable_policy();
        p->set_ceiling_ahead_ns(5000000000LL);
        p->set_physical_ahead_ns(2000000000LL);
        p->set_acceptance_budget_ns(3000000000LL);
        p->set_hlc_budget_ns(30000000000LL);
        p->set_version(1);
        p->set_acceptance_window_ns(15000000000LL);
        p->set_skew_limit_ns(60000000000LL);
        p->set_hlc_lead_ns(61000000000LL);
        p->set_uncertainty_cap_ns(1000000000LL);
    }
}
} // namespace
wire::MembershipState snapshot(SqliteMetadataStore& store)
{
    std::lock_guard lock(store.mutex_);
    auto loaded = store.membershipState();
    require(loaded.status());
    auto state = *loaded;
    defaults(state);
    return state;
}
bool wouldEmptyRoute(const wire::MembershipState& state, const std::string& id)
{
    for(const auto& r: state.routes())
        if(r.route().keepers_size() == 1 && lists(r.route(), id))
            return true;
    return false;
}
bool heartbeatChanges(const wire::MembershipState& state, const wire::HeartbeatRequest& q)
{
    for(const auto& m: state.members())
        if(m.process().process_id() == q.process_id())
        {
            if(m.process().instance() != q.instance())
                return !q.story_frontiers().empty();
            std::unordered_map<uint64_t, const wire::SettlementProof*> proofs;
            for(const auto& i: m.instances())
                if(i.instance() == q.instance())
                    for(const auto& p: i.proofs()) proofs[p.story_id()] = &p.proof();
            for(const auto& f: q.story_frontiers())
            {
                if(!f.drained_instance().empty())
                    return true;
                if(!f.has_settlement())
                    continue;
                for(const auto& i: m.instances())
                    if(i.instance() == q.instance() && f.settlement().instance() == i.instance() &&
                       hlc(f.settlement().coverage_start()) <= hlc(f.settlement().settled_through()) &&
                       hlc(f.settlement().settled_through()) <= hlc(i.ceiling()))
                    {
                        const auto found = proofs.find(f.story_id());
                        const auto* previous = found == proofs.end() ? nullptr : found->second;
                        if(!previous || (hlc(f.settlement().coverage_start()) == hlc(previous->coverage_start()) &&
                                         hlc(f.settlement().settled_through()) >= hlc(previous->settled_through()) &&
                                         f.settlement().SerializeAsString() != previous->SerializeAsString()))
                            return true;
                    }
            }
        }
    return false;
}
RouteState routeState(const wire::RouteUpdate& u)
{
    RouteState s;
    const auto& r = u.route();
    s.route = {r.epoch(), {}, r.grapher(), r.player()};
    for(const auto& k: r.keepers()) s.route.keepers.push_back({k.process_id(), k.endpoint()});
    s.ordering_cut = hlc(u.ordering_cut());
    s.physical_floor = u.physical_floor_ns();
    s.archived_below = hlc(u.archived_below());
    for(const auto& p: u.predecessors())
        s.predecessors.push_back({{p.keeper().process_id(), p.keeper().endpoint()},
                                  p.instance(),
                                  p.epoch(),
                                  hlc(p.own_cut()),
                                  p.own_physical_ceiling_ns()});
    for(const auto& a: u.abandoned()) s.abandoned.push_back({Range::Axis::Hlc, hlc(a.start()), hlc(a.end())});
    return s;
}
std::string apply(SqliteMetadataStore& store, const wire::MembershipCommand& q)
{
    auto loaded = store.membershipCommandState(q);
    require(loaded.status());
    auto state = *loaded;
    defaults(state);
    const auto before = state;
    const uint64_t revision = state.revision();
    std::string id;
    switch(q.operation_case())
    {
        case wire::MembershipCommand::kRegister:
            id = q.register_().process().process_id();
            break;
        case wire::MembershipCommand::kHeartbeat:
            id = q.heartbeat().process_id();
            break;
        case wire::MembershipCommand::kExtend:
            id = q.extend().process_id();
            break;
        case wire::MembershipCommand::kDrain:
            id = q.drain().process_id();
            break;
        case wire::MembershipCommand::kJoin:
            id = q.join().process_id();
            break;
        case wire::MembershipCommand::kAbandon:
            id = q.abandon().process_id();
            break;
        default:
            throw std::runtime_error("empty membership command");
    }
    auto* m = member(state, id);
    absl::Status status = absl::OkStatus();
    bool transition = false, replacement = false, total_loss = false;
    std::string previous;
    if(id.empty())
        status = absl::InvalidArgumentError("empty process id");
    else if(q.has_register_())
    {
        auto process = convert::fromProto(q.register_().process());
        if(!process.ok())
            status = process.status();
        else if(q.register_().policy_version() != 0 && q.register_().policy_version() != state.policy().version())
            status = absl::FailedPreconditionError("policy version mismatch");
        else
        {
            if(!m)
            {
                m = state.add_members();
            }
            previous = m->process().instance();
            if(!previous.empty() && m->process().role() != q.register_().process().role())
                status = absl::FailedPreconditionError("process role cannot change");
            if(process->instance != previous && instance(*m, process->instance))
                status = absl::FailedPreconditionError("revoked instance cannot register again");
            else
            {
                replacement = !previous.empty() && previous != process->instance &&
                              q.register_().recovered_instance() != previous && process->role == ProcessRole::Keeper;
                transition = replacement;
            }
        }
    }
    else if(!m)
        status = absl::NotFoundError("unknown process");
    else if(q.has_extend())
    {
        const auto& e = q.extend();
        if(m->process().instance() != e.instance() || m->process().role() != wire::PROCESS_ROLE_KEEPER)
            status = absl::FailedPreconditionError("obsolete or non Keeper instance");
        else if(e.applied_route_revision() < m->fence_revision())
            status = absl::FailedPreconditionError("missing route fence");
        else if(e.realtime_ns() < 0 || e.wanted_hlc().physical_ns() < 0)
            status = absl::InvalidArgumentError("negative ceiling input");
    }
    else if(q.has_heartbeat())
    {
        if(m->process().instance() != q.heartbeat().instance())
        {
            bool evidence = !q.heartbeat().story_frontiers().empty() && instance(*m, q.heartbeat().instance());
            for(const auto& f: q.heartbeat().story_frontiers())
                if((!f.has_settlement() || f.settlement().instance() != q.heartbeat().instance()) &&
                   f.drained_instance() != q.heartbeat().instance())
                    evidence = false;
            if(!evidence)
                status = absl::FailedPreconditionError("obsolete process instance");
        }
    }
    else
    {
        if(m->process().role() != wire::PROCESS_ROLE_KEEPER)
            status = absl::InvalidArgumentError("process is not a Keeper");
        transition = q.has_abandon() || (q.has_join() ? !m->joined() : m->joined());
    }
    if(status.ok() && transition && !replacement && !q.has_join())
    {
        std::string stories;
        for(const auto& r: state.routes())
            if(r.route().keepers_size() == 1 && lists(r.route(), id))
                stories += " " + std::to_string(r.story_id());
        if(!stories.empty())
            status = absl::FailedPreconditionError("would empty routes for stories:" + stories);
    }
    if(status.ok() && transition)
    {
        auto everGranted = [](const wire::MemberState* member)
        {
            if(member)
                for(const auto& instance: member->instances())
                    if(instance.granted())
                        return true;
            return false;
        };
        total_loss = q.has_abandon() && !everGranted(m);
        for(const auto& r: state.routes())
        {
            if(!q.has_join() && !lists(r.route(), id))
                continue;
            for(const auto& k: r.route().keepers())
            {
                if(!everGranted(member(state, k.process_id())) && !(total_loss && k.process_id() == id))
                    status = absl::FailedPreconditionError("epoch change needs ceilings for every old Keeper");
            }
        }
    }
    if(status.ok() && transition)
    {
        for(auto& r: *state.mutable_routes())
        {
            if(!q.has_join() && !lists(r.route(), id))
                continue;
            Hlc cut = hlc(r.ordering_cut());
            int64_t pf = std::numeric_limits<int64_t>::max();
            std::vector<std::string> fenced;
            for(const auto& k: r.route().keepers())
            {
                fenced.push_back(k.process_id());
                auto* old = member(state, k.process_id());
                if(old)
                {
                    cut = std::max(cut, maximum(*old));
                    auto* i = instance(*old, old->process().instance());
                    pf = std::min(pf, i ? i->physical_ceiling_ns() : int64_t{0});
                    if(k.process_id() == id && !q.has_join() && i)
                        predecessor(r, *old, *i);
                }
            }
            set(r.mutable_ordering_cut(), cut);
            if(pf != std::numeric_limits<int64_t>::max())
                r.set_physical_floor_ns(std::max(r.physical_floor_ns(), pf));
            r.clear_observe_floor();
            if(q.has_join())
            {
                auto* k = r.mutable_route()->add_keepers();
                k->set_process_id(id);
                k->set_endpoint(m->process().endpoint());
                r.add_observe_floor(id);
            }
            else if(replacement)
            {
                r.add_observe_floor(id);
                for(auto& k: *r.mutable_route()->mutable_keepers())
                    if(k.process_id() == id)
                        k.set_endpoint(q.register_().process().endpoint());
            }
            else
            {
                auto* ks = r.mutable_route()->mutable_keepers();
                for(int n = ks->size() - 1; n >= 0; --n)
                    if(ks->Get(n).process_id() == id)
                        ks->DeleteSubrange(n, 1);
                auto acquisitions = store.storyAcquisitions(r.story_id());
                require(acquisitions.status());
                for(const auto& a: *acquisitions)
                    if(a.assigned_keeper.process_id == id && !ks->empty())
                    {
                        auto target =
                                ks->Get(static_cast<int>(a.writer_id % static_cast<uint64_t>(ks->size()))).process_id();
                        if(std::find(r.observe_floor().begin(), r.observe_floor().end(), target) ==
                           r.observe_floor().end())
                            r.add_observe_floor(target);
                    }
            }
            auto bumped = store.compareAndSetEpoch(r.story_id(), r.route().epoch(), r.route().epoch() + 1);
            require(bumped.status());
            r.mutable_route()->set_epoch(*bumped);
            for(const auto& k: r.route().keepers())
            {
                auto* keeper = member(state, k.process_id());
                if(!keeper || keeper->policy_version() != state.policy().version())
                    r.set_physical_policy(false);
            }
            r.set_revision(revision);
            for(const auto& k: r.route().keepers()) fenced.push_back(k.process_id());
            for(const auto& f: fenced)
            {
                auto* keeper = member(state, f);
                if(keeper)
                    keeper->set_fence_revision(revision);
            }
            require(store.fenceRemovedWriters(r.story_id(), routeState(r).route, revision, replacement ? id : ""));
            if(total_loss)
            {
                auto* a = r.add_abandoned();
                set(a->mutable_start(), {});
                set(a->mutable_end(), {std::numeric_limits<int64_t>::max(), std::numeric_limits<uint32_t>::max()});
            }
        }
        if(!replacement)
            m->set_joined(q.has_join());
    }
    if(status.ok() && q.has_register_())
    {
        if(previous != q.register_().process().instance())
            m->set_applied_route_revision(0);
        *m->mutable_process() = q.register_().process();
        m->set_policy_version(q.register_().policy_version());
        if(m->policy_version() != state.policy().version())
            for(auto& r: *state.mutable_routes())
                if(lists(r.route(), id) && r.physical_policy())
                {
                    r.set_physical_policy(false);
                    r.set_revision(revision);
                }
        if(!instance(*m, m->process().instance()))
        {
            auto* fresh = m->add_instances();
            fresh->set_instance(m->process().instance());
            set(fresh->mutable_ceiling(), maximum(*m));
            fresh->set_physical_ceiling_ns(physicalMaximum(*m));
        }
        if(!previous.empty() && previous != m->process().instance() && q.register_().recovered_instance() == previous)
        {
            for(auto& r: *state.mutable_routes())
            {
                bool changed = false;
                for(auto& k: *r.mutable_route()->mutable_keepers())
                    if(k.process_id() == id)
                    {
                        k.set_endpoint(m->process().endpoint());
                        changed = true;
                    }
                for(auto& p: *r.mutable_predecessors())
                    if(p.keeper().process_id() == id && p.instance() == previous)
                    {
                        p.set_instance(m->process().instance());
                        p.mutable_keeper()->set_endpoint(m->process().endpoint());
                        changed = true;
                    }
                if(changed)
                    r.set_revision(revision);
            }
        }
    }
    if(status.ok() && q.has_extend())
    {
        auto* i = instance(*m, m->process().instance());
        if(!i)
            throw std::runtime_error("registered instance missing");
        Hlc wanted = hlc(q.extend().wanted_hlc());
        wanted.physical_ns = add(wanted.physical_ns, state.policy().ceiling_ahead_ns());
        set(i->mutable_ceiling(), std::max(wanted, maximum(*m)));
        i->set_physical_ceiling_ns(
                std::max(physicalMaximum(*m),
                         add(q.extend().realtime_ns(),
                             state.policy().acceptance_budget_ns() + state.policy().physical_ahead_ns())));
        i->set_granted(true);
    }
    if(status.ok() && q.has_heartbeat())
    {
        auto* i = instance(*m, q.heartbeat().instance());
        std::unordered_map<uint64_t, wire::InstanceProof*> proofs;
        if(i)
            for(auto& p: *i->mutable_proofs()) proofs[p.story_id()] = &p;
        std::unordered_map<uint64_t, wire::RouteUpdate*> routes;
        for(auto& r: *state.mutable_routes()) routes[r.story_id()] = &r;
        for(const auto& f: q.heartbeat().story_frontiers())
        {
            if(i && f.has_settlement() && f.settlement().instance() == i->instance() &&
               hlc(f.settlement().coverage_start()) <= hlc(f.settlement().settled_through()) &&
               hlc(f.settlement().settled_through()) <= hlc(i->ceiling()))
            {
                auto* proof = proofs[f.story_id()];
                if(!proof)
                {
                    proof = i->add_proofs();
                    proof->set_story_id(f.story_id());
                    proofs[f.story_id()] = proof;
                }
                if(!proof->has_proof() ||
                   (hlc(f.settlement().coverage_start()) == hlc(proof->proof().coverage_start()) &&
                    hlc(f.settlement().settled_through()) >= hlc(proof->proof().settled_through())))
                    *proof->mutable_proof() = f.settlement();
            }
            if(auto found = routes.find(f.story_id()); found != routes.end())
            {
                auto& r = *found->second;
                auto* ps = r.mutable_predecessors();
                for(int n = ps->size() - 1; n >= 0; --n)
                {
                    const auto& p = ps->Get(n);
                    if(p.keeper().process_id() == id && p.instance() == f.drained_instance() &&
                       p.instance() == q.heartbeat().instance() && p.epoch() == f.drained_epoch() &&
                       hlc(f.evicted_below()) >= hlc(p.own_cut()) && hlc(f.sealed_frontier()) >= hlc(p.own_cut()))
                    {
                        set(r.mutable_archived_below(), std::max(hlc(r.archived_below()), hlc(p.own_cut())));
                        ps->DeleteSubrange(n, 1);
                        r.set_revision(revision);
                    }
                }
            }
        }
    }
    if(status.ok() && q.has_abandon())
    {
        std::unordered_map<std::string, std::unordered_map<uint64_t, const wire::SettlementProof*>> proofs;
        for(const auto& i: m->instances())
            for(const auto& proof: i.proofs()) proofs[i.instance()][proof.story_id()] = &proof.proof();
        for(auto& r: *state.mutable_routes())
        {
            auto* ps = r.mutable_predecessors();
            for(int n = ps->size() - 1; n >= 0; --n)
            {
                const auto& p = ps->Get(n);
                if(p.keeper().process_id() != id)
                    continue;
                Hlc start{};
                const auto foundInstance = proofs.find(p.instance());
                if(foundInstance != proofs.end())
                    if(auto found = foundInstance->second.find(r.story_id()); found != foundInstance->second.end())
                    {
                        const auto& proof = *found->second;
                        if(proof.instance() == p.instance() && proof.has_first_event() &&
                           hlc(proof.coverage_start()) <= hlc(proof.first_event()))
                            start = hlc(proof.settled_through());
                    }
                set(r.mutable_archived_below(), std::max(hlc(r.archived_below()), std::min(start, hlc(p.own_cut()))));
                if(start < hlc(p.own_cut()))
                {
                    auto* a = r.add_abandoned();
                    set(a->mutable_start(), start);
                    *a->mutable_end() = p.own_cut();
                }
                ps->DeleteSubrange(n, 1);
                r.set_revision(revision);
            }
        }
    }
    if(status.ok())
    {
        std::set<std::pair<uint64_t, uint64_t>> history;
        for(const auto& old: state.route_history()) history.emplace(old.revision(), old.story_id());
        for(const auto& r: state.routes())
            if(r.revision() == revision && history.emplace(revision, r.story_id()).second)
                *state.add_route_history() = r;
        if(m && q.has_extend() && status.ok())
            m->set_applied_route_revision(std::max(m->applied_route_revision(), q.extend().applied_route_revision()));
        if(m && q.has_heartbeat() && m->process().instance() == q.heartbeat().instance())
            m->set_applied_route_revision(
                    std::max(m->applied_route_revision(), q.heartbeat().applied_route_revision()));
        for(const auto& applied: q.applied_routes())
            if(auto* current = member(state, applied.process_id());
               current && current->process().instance() == applied.instance())
                current->set_applied_route_revision(std::max(current->applied_route_revision(), applied.revision()));
        require(store.saveMembershipChanges(before, state));
    }
    if(q.has_register_())
    {
        wire::RegisterResponse r;
        *r.mutable_status() = convert::toProto(status);
        if(status.ok())
        {
            set(r.mutable_ceiling_floor(), maximum(*m));
            r.set_physical_ceiling_floor_ns(physicalMaximum(*m));
            *r.mutable_policy() = state.policy();
        }
        return r.SerializeAsString();
    }
    if(q.has_extend())
    {
        wire::ExtendCeilingResponse r;
        *r.mutable_status() = convert::toProto(status);
        if(m)
        {
            r.set_fence_revision(m->fence_revision());
            if(status.ok())
            {
                auto* i = instance(*m, m->process().instance());
                *r.mutable_ceiling() = i->ceiling();
                r.set_physical_ceiling_ns(i->physical_ceiling_ns());
            }
            else if(q.extend().applied_route_revision() < m->fence_revision())
            {
                if(q.extend().applied_route_revision() < state.route_history_floor())
                    for(const auto& route: state.routes()) *r.add_routes() = route;
                else
                    for(const auto& route: state.route_history())
                        if(route.revision() > q.extend().applied_route_revision() &&
                           route.revision() <= m->fence_revision())
                            *r.add_routes() = route;
            }
        }
        return r.SerializeAsString();
    }
    if(q.has_heartbeat())
    {
        wire::HeartbeatResponse r;
        *r.mutable_status() = convert::toProto(status);
        return r.SerializeAsString();
    }
    wire::MembershipResponse r;
    *r.mutable_status() = convert::toProto(status);
    return r.SerializeAsString();
}
} // namespace chronolog::visor::dynamic
