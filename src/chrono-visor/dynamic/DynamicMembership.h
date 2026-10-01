#pragma once
#include "chronolog/membership.h"
#include "dynamic/MembershipState.h"
#include "adapter/Convert.h"
namespace chronolog::visor
{
class DynamicMembership final: public Membership
{
public:
    using Submit = std::function<absl::StatusOr<std::string>(const internal::v1::CatalogCommand&)>;
    DynamicMembership(SqliteMetadataStore& store, Submit submit)
        : store_(store)
        , submit_(std::move(submit))
    {}
    absl::StatusOr<RouteState> routeState(StoryId id) const override
    {
        auto story = store_.getStory(id);
        if(!story.ok())
            return story.status();
        if(story->tombstoned)
            return absl::NotFoundError("destroyed story");
        auto state = dynamic::snapshot(store_);
        for(const auto& r: state.routes())
            if(r.story_id() == id)
                return dynamic::routeState(r);
        return absl::NotFoundError("unknown story");
    }
    absl::StatusOr<Route> route(StoryId id) const override
    {
        auto state = routeState(id);
        if(!state.ok())
            return state.status();
        return state->route;
    }
    absl::Status validateEpoch(StoryId id, Epoch epoch) const override
    {
        auto r = route(id);
        if(!r.ok())
            return r.status();
        return r->epoch == epoch ? absl::OkStatus() : absl::FailedPreconditionError("stale epoch");
    }
    absl::Status registerProcess(Process process) override
    {
        internal::v1::CatalogCommand c;
        auto* p = c.mutable_membership()->mutable_register_()->mutable_process();
        p->set_process_id(process.id);
        p->set_instance(process.instance);
        p->set_endpoint(process.endpoint);
        p->set_role(static_cast<internal::v1::ProcessRole>(static_cast<int>(process.role) + 1));
        auto result = submit_(c);
        if(!result.ok())
            return result.status();
        internal::v1::RegisterResponse r;
        if(!r.ParseFromString(*result))
            return absl::InternalError("invalid response");
        return decode(r.status());
    }
    absl::Status heartbeat(std::string id, std::string instance, uint64_t applied_revision = 0) override
    {
        internal::v1::CatalogCommand c;
        auto* q = c.mutable_membership()->mutable_heartbeat();
        q->set_process_id(id);
        q->set_instance(instance);
        q->set_applied_revision(applied_revision);
        auto result = submit_(c);
        if(!result.ok())
            return result.status();
        internal::v1::HeartbeatResponse r;
        if(!r.ParseFromString(*result))
            return absl::InternalError("invalid response");
        return decode(r.status());
    }

private:
    static absl::Status decode(const v1::ItemStatus& s)
    {
        return absl::Status(static_cast<absl::StatusCode>(s.code()), s.message());
    }
    SqliteMetadataStore& store_;
    Submit submit_;
};
} // namespace chronolog::visor
