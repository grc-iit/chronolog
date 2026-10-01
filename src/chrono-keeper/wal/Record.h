#pragma once

#include <string>
#include <string_view>

#include "chronolog/types.h"

namespace chronolog::wal
{

std::string encode(const Event& event);
Event decode(std::string_view payload);
std::string reserve(Hlc hlc);
Hlc decodeReserve(std::string_view payload);
std::string frame(std::string_view payload);
uint32_t uint32(std::string_view bytes);

} // namespace chronolog::wal
