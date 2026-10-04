#pragma once

#include "visor/catalog/AcquisitionLedger.h"
#include "chronolog/types.h"
#include "chronolog/v1/chronolog.pb.h"
#include "chronolog/internal/v1/internal.pb.h"

// Conversions between contract types and generated protobuf types. The two service
// adapters and this file are the only visor code that includes generated headers
// (W10.7, M11.1).
namespace chronolog::visor::convert
{

// absl status codes carry the same integers as google.rpc.Code.
v1::ItemStatus toProto(const absl::Status& status);
// I9.2: the request and row messages carry the same three optional property fields.
template <class Message>
Properties propertiesOf(const Message& message)
{
    Properties out;
    if(message.has_tier_policy())
        out.tier_policy = message.tier_policy();
    if(message.has_retention_ns())
        out.retention_ns = message.retention_ns();
    out.granularity = static_cast<Granularity>(message.granularity());
    return out;
}
template <class Message>
void setProperties(const Properties& properties, Message* message)
{
    if(properties.tier_policy)
        message->set_tier_policy(*properties.tier_policy);
    if(properties.retention_ns)
        message->set_retention_ns(*properties.retention_ns);
    message->set_granularity(static_cast<v1::Granularity>(properties.granularity));
}

v1::Chronicle toProto(const Chronicle& chronicle);
v1::Story toProto(const Story& story);
v1::KeeperRef toProto(const KeeperRef& keeper);
v1::Route toProto(const Route& route);
v1::AcquireResponse toAcquireResponse(const Acquisition& acquisition);
Acquisition fromAcquireResponse(const v1::AcquireResponse& response);
AcquireOptions fromAcquireRequest(const v1::AcquireRequest& request);
void acquireRefusal(const absl::Status& status, v1::AcquireResponse& response);
absl::Status acquireStatus(const v1::AcquireResponse& response);
internal::v1::AcquisitionUpdate toProto(const AcquisitionChange& change);
internal::v1::AcquisitionSnapshot toProto(const AcquisitionSnapshot& snapshot);

absl::StatusOr<Process> fromProto(const internal::v1::Process& process);

} // namespace chronolog::visor::convert
