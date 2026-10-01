#include "adapter/Convert.h"

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
    return out;
}

v1::Route toProto(const Route& route)
{
    v1::Route out;
    out.set_epoch(route.epoch);
    for(const auto& keeper: route.keepers)
        out.add_keepers(keeper);
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
    out.set_epoch(acquisition.route.epoch);
    out.set_assigned_keeper(acquisition.assigned_keeper);
    return out;
}

internal::v1::AcquisitionUpdate toProto(const AcquisitionChange& change)
{
    internal::v1::AcquisitionUpdate out;
    out.set_revision(change.revision);
    out.set_story_id(change.story_id);
    out.set_writer_id(change.writer_id);
    out.set_incarnation(change.incarnation);
    out.set_assigned_keeper(change.assigned_keeper);
    out.set_state(change.state == AcquisitionState::Acquired ? internal::v1::ACQUIRED : internal::v1::RELEASED);
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
        case internal::v1::KEEPER:
            out.role = ProcessRole::Keeper;
            break;
        case internal::v1::GRAPHER:
            out.role = ProcessRole::Grapher;
            break;
        case internal::v1::PLAYER:
            out.role = ProcessRole::Player;
            break;
        default:
            return absl::InvalidArgumentError("process role is required");
    }
    return out;
}

} // namespace chronolog::visor::convert
