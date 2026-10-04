#pragma once

#include "chronolog/types.h"
#include "chronolog/v1/chronolog.pb.h"

namespace chronolog::player::convert
{

Hlc fromProto(const v1::Hlc& hlc);
v1::Hlc toProto(const Hlc& hlc);
Event fromProto(const v1::Event& event);
v1::Event toProto(const Event& event);
// toProto(event).ByteSizeLong() without building the message or copying the payload.
size_t encodedSize(const Event& event);

} // namespace chronolog::player::convert
