#include "visor/adapter/Convert.h"
#include "chronolog/acquire_refusal.h"

namespace chronolog::visor::convert
{

v1::ItemStatus toProto(const absl::Status& status)
{
    v1::ItemStatus out;
    out.set_code(static_cast<int32_t>(status.code()));
    out.set_message(std::string(status.message()));
    return out;
}

v1::Chronicle toProto(const Chronicle& chronicle)
{
    v1::Chronicle out;
    out.set_name(chronicle.name);
    out.set_tombstoned(chronicle.tombstoned);
    setProperties(chronicle.properties, &out);
    return out;
}

v1::Story toProto(const Story& story)
{
    v1::Story out;
    out.set_story_id(story.id);
    out.set_chronicle(story.chronicle);
    out.set_name(story.name);
    out.set_epoch(story.epoch);
    out.set_tombstoned(story.tombstoned);
    setProperties(story.properties, &out);
    return out;
}

v1::KeeperRef toProto(const KeeperRef& keeper)
{
    v1::KeeperRef out;
    out.set_process_id(keeper.process_id);
    out.set_endpoint(keeper.endpoint);
    return out;
}

v1::Route toProto(const Route& route)
{
    v1::Route out;
    out.set_epoch(route.epoch);
    for(const auto& keeper: route.keepers) *out.add_keepers() = toProto(keeper);
    out.set_grapher(route.grapher);
    out.set_player(route.player);
    return out;
}

v1::AcquireResponse toAcquireResponse(const Acquisition& acquisition)
{
    v1::AcquireResponse out;
    out.set_story_id(acquisition.story_id);
    out.set_writer_id(acquisition.writer_id);
    out.set_incarnation(acquisition.incarnation);
    *out.mutable_route() = toProto(acquisition.route);
    *out.mutable_assigned_keeper() = toProto(acquisition.assigned_keeper);
    out.mutable_lease()->set_duration_ns(acquisition.lease.duration_ns);
    out.mutable_lease()->set_remaining_ns(acquisition.lease.remaining_ns);
    if(acquisition.keeper_preference)
        out.set_keeper_preference(static_cast<v1::KeeperPreferenceResult>(*acquisition.keeper_preference));
    return out;
}
Acquisition fromAcquireResponse(const v1::AcquireResponse& r)
{
    Route route;
    route.epoch = r.route().epoch();
    route.grapher = r.route().grapher();
    route.player = r.route().player();
    for(const auto& k: r.route().keepers()) route.keepers.push_back({k.process_id(), k.endpoint()});
    Acquisition out{r.story_id(),
                    r.writer_id(),
                    r.incarnation(),
                    route,
                    {r.assigned_keeper().process_id(), r.assigned_keeper().endpoint()},
                    {r.lease().duration_ns(), r.lease().remaining_ns()}};
    if(r.has_keeper_preference())
        out.keeper_preference = static_cast<KeeperPreferenceResult>(r.keeper_preference());
    return out;
}
AcquireOptions fromAcquireRequest(const v1::AcquireRequest& r)
{
    AcquireOptions out;
    if(r.has_lease_duration_ns())
        out.lease_duration_ns = r.lease_duration_ns();
    if(r.has_preferred_keeper_process_id())
        out.preferred_keeper_process_id = r.preferred_keeper_process_id();
    if(r.has_expected_prior_incarnation())
        out.expected_prior_incarnation = r.expected_prior_incarnation();
    out.takeover = r.takeover();
    out.acquire_request_id = r.acquire_request_id();
    return out;
}
void acquireRefusal(const absl::Status& status, v1::AcquireResponse& r)
{
    auto detail = getAcquireRefusal(status);
    if(!detail)
        return;
    r.set_refusal_reason(static_cast<v1::AcquireRefusalReason>(detail->refusal_reason));
    r.set_remaining_ns(detail->remaining_ns);
    if(detail->current_incarnation)
        r.set_current_incarnation(*detail->current_incarnation);
    if(detail->matched_incarnation)
        r.set_incarnation(*detail->matched_incarnation);
    if(detail->termination_cause)
        r.set_termination_cause(static_cast<v1::AcquisitionTerminationCause>(*detail->termination_cause));
}
absl::Status acquireStatus(const v1::AcquireResponse& r)
{
    absl::Status status(static_cast<absl::StatusCode>(r.status().code()), r.status().message());
    if(r.refusal_reason() || r.has_termination_cause() || (r.status().code() && r.incarnation()))
    {
        AcquireRefusal detail;
        detail.refusal_reason = static_cast<AcquireRefusalReason>(r.refusal_reason());
        detail.remaining_ns = r.remaining_ns();
        if(r.has_current_incarnation())
            detail.current_incarnation = r.current_incarnation();
        if(r.incarnation())
            detail.matched_incarnation = r.incarnation();
        if(r.has_termination_cause())
            detail.termination_cause = static_cast<AcquisitionTerminationCause>(r.termination_cause());
        setAcquireRefusal(status, detail);
    }
    return status;
}

internal::v1::AcquisitionUpdate toProto(const AcquisitionChange& change)
{
    internal::v1::AcquisitionUpdate out;
    out.set_termination_cause(static_cast<v1::AcquisitionTerminationCause>(change.termination_cause));
    out.set_revision(change.revision);
    out.set_story_id(change.story_id);
    out.set_writer_id(change.writer_id);
    out.set_incarnation(change.incarnation);
    *out.mutable_assigned_keeper() = toProto(change.assigned_keeper);
    out.set_state(change.state == AcquisitionState::Acquired ? internal::v1::ACQUISITION_STATE_ACQUIRED
                                                             : internal::v1::ACQUISITION_STATE_RELEASED);
    return out;
}

internal::v1::AcquisitionSnapshot toProto(const AcquisitionSnapshot& snapshot)
{
    internal::v1::AcquisitionSnapshot out;
    out.set_revision(snapshot.revision);
    for(const auto& change: snapshot.active) *out.add_acquisitions() = toProto(change);
    return out;
}

absl::StatusOr<Process> fromProto(const internal::v1::Process& process)
{
    Process out;
    out.id = process.process_id();
    out.instance = process.instance();
    out.endpoint = process.endpoint();
    switch(process.role())
    {
        case internal::v1::PROCESS_ROLE_KEEPER:
            out.role = ProcessRole::Keeper;
            break;
        case internal::v1::PROCESS_ROLE_GRAPHER:
            out.role = ProcessRole::Grapher;
            break;
        case internal::v1::PROCESS_ROLE_PLAYER:
            out.role = ProcessRole::Player;
            break;
        default:
            return absl::InvalidArgumentError("process role is required");
    }
    return out;
}

} // namespace chronolog::visor::convert
