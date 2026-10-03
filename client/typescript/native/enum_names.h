#pragma once
#include <initializer_list>
#include <string>
#include <type_traits>
#include "absl/status/status.h"
#include "chronolog/types.h"

namespace binding
{
template <class E>
std::string enumName(E value, std::initializer_list<const char*> names)
{
    const auto number = static_cast<std::underlying_type_t<E>>(value);
    const auto index = static_cast<size_t>(number);
    return index < names.size() ? names.begin()[index] : "UNKNOWN_" + std::to_string(number);
}
inline std::string codeName(absl::StatusCode code)
{
    static const std::initializer_list<const char*> names = {"OK",
                                                             "CANCELLED",
                                                             "UNKNOWN",
                                                             "INVALID_ARGUMENT",
                                                             "DEADLINE_EXCEEDED",
                                                             "NOT_FOUND",
                                                             "ALREADY_EXISTS",
                                                             "PERMISSION_DENIED",
                                                             "RESOURCE_EXHAUSTED",
                                                             "FAILED_PRECONDITION",
                                                             "ABORTED",
                                                             "OUT_OF_RANGE",
                                                             "UNIMPLEMENTED",
                                                             "INTERNAL",
                                                             "UNAVAILABLE",
                                                             "DATA_LOSS",
                                                             "UNAUTHENTICATED"};
    return enumName(code, names);
}
inline std::string rejectionName(chronolog::AppendRejection value)
{
    static const std::initializer_list<const char*> names = {"UNSPECIFIED",
                                                             "FENCED_RELEASED",
                                                             "FENCED_SUPERSEDED",
                                                             "SEQUENCE_GAP",
                                                             "DEDUPE_WINDOW",
                                                             "EARLIER_ITEM_FAILED",
                                                             "NOT_REGISTERED",
                                                             "STALE_EPOCH",
                                                             "UNASSIGNED_KEEPER",
                                                             "KEEPER_NOT_IN_ROUTE",
                                                             "STORY_TOMBSTONED",
                                                             "FENCED_EXPIRED",
                                                             "FENCED_OWNER_REMOVED"};
    return enumName(value, names);
}
inline std::string causeName(chronolog::AcquisitionTerminationCause value)
{
    static const std::initializer_list<const char*> names = {"UNSPECIFIED",
                                                             "EXPIRED",
                                                             "RELEASED",
                                                             "SUPERSEDED",
                                                             "OWNER_REMOVED"};
    return enumName(value, names);
}
} // namespace binding
