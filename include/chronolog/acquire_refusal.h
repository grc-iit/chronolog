#pragma once

#include <array>
#include <limits>
#include <absl/strings/cord.h>
#include "chronolog/types.h"

namespace chronolog
{
inline constexpr char kAcquireRefusalPayload[] = "type.chronolog.org/chronolog.AcquireRefusal.v1";

inline void setAcquireRefusal(absl::Status& status, const AcquireRefusal& refusal)
{
    const uint64_t flags = (refusal.current_incarnation ? 1u : 0u) | (refusal.matched_incarnation ? 2u : 0u) |
                           (refusal.termination_cause ? 4u : 0u);
    const std::array<uint64_t, 7> fields{
            static_cast<uint64_t>(refusal.refusal_reason),
            flags,
            refusal.current_incarnation.value_or(0),
            refusal.matched_incarnation.value_or(0),
            static_cast<uint64_t>(refusal.remaining_ns),
            static_cast<uint64_t>(refusal.termination_cause.value_or(AcquisitionTerminationCause::Unspecified)),
            1};
    std::string bytes;
    bytes.reserve(fields.size() * 8);
    for(auto value: fields)
        for(unsigned shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<char>((value >> shift) & 0xff));
    status.SetPayload(kAcquireRefusalPayload, absl::Cord(bytes));
}

inline std::optional<AcquireRefusal> getAcquireRefusal(const absl::Status& status)
{
    auto payload = status.GetPayload(kAcquireRefusalPayload);
    if(!payload)
        return std::nullopt;
    const std::string bytes = std::string(payload->Flatten());
    std::array<uint64_t, 7> fields{};
    if(bytes.size() != fields.size() * 8)
        return std::nullopt;
    for(size_t i = 0; i < fields.size(); ++i)
        for(unsigned byte = 0; byte < 8; ++byte)
            fields[i] |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[i * 8 + byte])) << (byte * 8);
    if(fields[0] > 2 || fields[1] > 7 || fields[4] > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
       fields[5] > 4 || fields[6] != 1 || ((fields[1] & 1) ? fields[2] == 0 : fields[2] != 0) ||
       ((fields[1] & 2) ? fields[3] == 0 : fields[3] != 0) || (!(fields[1] & 4) && fields[5] != 0))
        return std::nullopt;
    AcquireRefusal refusal;
    refusal.refusal_reason = static_cast<AcquireRefusalReason>(fields[0]);
    if(fields[1] & 1)
        refusal.current_incarnation = fields[2];
    if(fields[1] & 2)
        refusal.matched_incarnation = fields[3];
    refusal.remaining_ns = static_cast<int64_t>(fields[4]);
    if(fields[1] & 4)
        refusal.termination_cause = static_cast<AcquisitionTerminationCause>(fields[5]);
    return refusal;
}
} // namespace chronolog
