#include "keeper/adapter/Convert.h"

namespace chronolog::keeper::convert
{

v1::ItemStatus toProto(const absl::Status& status)
{
    v1::ItemStatus out;
    out.set_code(static_cast<int32_t>(status.code()));
    out.set_message(std::string(status.message()));
    return out;
}

v1::Hlc toProto(const Hlc& hlc)
{
    v1::Hlc out;
    out.set_physical_ns(hlc.physical_ns);
    out.set_logical(hlc.logical);
    return out;
}

Hlc fromProto(const v1::Hlc& hlc) { return Hlc{hlc.physical_ns(), hlc.logical()}; }

v1::Route toProto(const Route& route)
{
    v1::Route out;
    out.set_epoch(route.epoch);
    for(const auto& keeper: route.keepers)
    {
        auto* k = out.add_keepers();
        k->set_process_id(keeper.process_id);
        k->set_endpoint(keeper.endpoint);
    }
    out.set_grapher(route.grapher);
    out.set_player(route.player);
    return out;
}

Route fromProto(const v1::Route& route)
{
    Route out;
    out.epoch = route.epoch();
    for(const auto& keeper: route.keepers()) out.keepers.push_back(KeeperRef{keeper.process_id(), keeper.endpoint()});
    out.grapher = route.grapher();
    out.player = route.player();
    return out;
}

namespace
{

v1::Durability toProto(Durability durability)
{
    switch(durability)
    {
        case Durability::Accepted:
            return v1::DURABILITY_ACCEPTED;
        case Durability::Durable:
            return v1::DURABILITY_DURABLE;
        default:
            return v1::DURABILITY_UNSPECIFIED;
    }
}

v1::ClockStatus toProto(ClockStatus status)
{
    switch(status)
    {
        case ClockStatus::Synced:
            return v1::CLOCK_STATUS_SYNCED;
        case ClockStatus::Unsynced:
            return v1::CLOCK_STATUS_UNSYNCED;
        default:
            return v1::CLOCK_STATUS_UNAVAILABLE;
    }
}

v1::TimeReading toProto(const TimeReading& reading)
{
    v1::TimeReading out;
    out.set_physical_ns(reading.physical_ns);
    if(reading.uncertainty_ns && reading.status == ClockStatus::Synced)
        out.set_uncertainty_ns(*reading.uncertainty_ns);
    out.set_status(toProto(reading.status));
    return out;
}

v1::EventId toProto(const EventId& id)
{
    v1::EventId out;
    out.set_story_id(id.story_id);
    out.set_writer_id(id.writer_id);
    out.set_incarnation(id.incarnation);
    out.set_sequence(id.sequence);
    return out;
}

v1::Envelope toProto(const Envelope& envelope)
{
    v1::Envelope out;
    out.set_content_type(envelope.content_type);
    out.set_payload(envelope.payload);
    out.set_trace_id(envelope.trace_id);
    out.set_span_id(envelope.span_id);
    for(const auto& [key, value]: envelope.attributes) (*out.mutable_attributes())[key] = value;
    out.set_kind(envelope.kind);
    out.set_actor(envelope.actor);
    for(const auto& link: envelope.links)
    {
        auto* l = out.add_links();
        l->set_type(link.type);
        *l->mutable_target() = toProto(link.target);
        if(link.target_hlc)
            *l->mutable_target_hlc() = convert::toProto(*link.target_hlc);
    }
    return out;
}

template <class Req>
absl::StatusOr<ParsedAppend> parseAppend(const Req& request)
{
    if(request.story_id() == 0)
        return absl::InvalidArgumentError("story_id is required");
    if(request.epoch() == 0)
        return absl::InvalidArgumentError("epoch is required");
    if(request.items().empty())
        return absl::InvalidArgumentError("items is empty");
    ParsedAppend out;
    switch(request.durability())
    {
        case v1::DURABILITY_UNSPECIFIED:
            out.durability = Durability::Unspecified;
            break;
        case v1::DURABILITY_ACCEPTED:
            out.durability = Durability::Accepted;
            break;
        case v1::DURABILITY_DURABLE:
            out.durability = Durability::Durable;
            break;
        default:
            return absl::InvalidArgumentError("unknown durability");
    }
    out.batch.story_id = request.story_id();
    out.batch.epoch = request.epoch();
    out.batch.items.reserve(request.items().size());
    for(const auto& item: request.items()) out.batch.items.push_back(fromProto(item));
    out.batch_id = request.batch_id();
    return out;
}

template <class Resp>
void fillResponse(Resp& response, const ParsedAppend& parsed, const std::vector<AppendResult>& results)
{
    response.set_batch_id(parsed.batch_id);
    bool all_redirected = !results.empty();
    for(const auto& result: results)
    {
        *response.add_results() = convert::toProto(result);
        if(!result.current_route || result.current_route->epoch == parsed.batch.epoch)
            all_redirected = false;
    }
    if(all_redirected)
        *response.mutable_current_route() = convert::toProto(*results.front().current_route);
}

} // namespace

v1::Event toProto(const Event& event)
{
    v1::Event out;
    out.set_durability(toProto(event.durability));
    *out.mutable_id() = toProto(event.id);
    *out.mutable_physical() = toProto(event.physical);
    *out.mutable_hlc() = toProto(event.hlc);
    *out.mutable_envelope() = toProto(event.envelope);
    return out;
}

v1::AppendResult toProto(const AppendResult& result)
{
    v1::AppendResult out;
    *out.mutable_status() = toProto(result.status);
    out.set_achieved_durability(toProto(result.achieved));
    out.set_rejection(static_cast<v1::AppendRejection>(result.rejection));
    *out.mutable_assigned_hlc() = toProto(result.hlc);
    *out.mutable_id() = toProto(result.id);
    if(result.current_route)
        *out.mutable_current_route() = toProto(*result.current_route);
    return out;
}

v1::WriterFrontier toProto(const Frontier& frontier)
{
    v1::WriterFrontier out;
    out.set_writer_id(frontier.writer_id);
    out.set_incarnation(frontier.incarnation);
    *out.mutable_frontier() = toProto(frontier.frontier);
    return out;
}

TimeReading fromProto(const v1::TimeReading& reading)
{
    TimeReading out;
    out.physical_ns = reading.physical_ns();
    switch(reading.status())
    {
        case v1::CLOCK_STATUS_SYNCED:
            if(reading.has_uncertainty_ns())
            {
                out.status = ClockStatus::Synced;
                out.uncertainty_ns = reading.uncertainty_ns();
            }
            else
            {
                out.status = ClockStatus::Unsynced;
            }
            break;
        case v1::CLOCK_STATUS_UNSYNCED:
            out.status = ClockStatus::Unsynced;
            break;
        default:
            out.status = ClockStatus::Unavailable;
            break;
    }
    return out;
}

Envelope fromProto(const v1::Envelope& envelope)
{
    Envelope out;
    out.content_type = envelope.content_type();
    out.payload = envelope.payload();
    out.trace_id = envelope.trace_id();
    out.span_id = envelope.span_id();
    for(const auto& [key, value]: envelope.attributes()) out.attributes[key] = value;
    out.kind = envelope.kind();
    out.actor = envelope.actor();
    out.links.reserve(envelope.links().size());
    for(const auto& link: envelope.links())
    {
        Link l;
        l.type = link.type();
        l.target = {link.target().story_id(),
                    link.target().writer_id(),
                    link.target().incarnation(),
                    link.target().sequence()};
        if(link.has_target_hlc())
            l.target_hlc = fromProto(link.target_hlc());
        out.links.push_back(std::move(l));
    }
    return out;
}

AppendItem fromProto(const v1::AppendItem& item)
{
    AppendItem out;
    out.writer_id = item.writer_id();
    out.incarnation = item.incarnation();
    out.sequence = item.sequence();
    out.physical = fromProto(item.physical());
    out.causal_floor = fromProto(item.causal_floor());
    out.envelope = fromProto(item.envelope());
    return out;
}

absl::StatusOr<ParsedAppend> parse(const v1::AppendRequest& request) { return parseAppend(request); }
absl::StatusOr<ParsedAppend> parse(const v1::AppendStreamRequest& request) { return parseAppend(request); }

void fill(v1::AppendResponse& response, const ParsedAppend& parsed, const std::vector<AppendResult>& results)
{
    fillResponse(response, parsed, results);
}

void fill(v1::AppendStreamResponse& response, const ParsedAppend& parsed, const std::vector<AppendResult>& results)
{
    fillResponse(response, parsed, results);
}

grpc::Status toGrpc(const absl::Status& status)
{
    return grpc::Status(static_cast<grpc::StatusCode>(status.code()), std::string(status.message()));
}

} // namespace chronolog::keeper::convert
