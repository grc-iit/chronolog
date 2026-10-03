#pragma once

#include <cstddef>

namespace chronolog
{

inline constexpr int kKeeperAppendReceiveBytes = 64 << 20;
inline constexpr int kEventStreamReceiveBytes = kKeeperAppendReceiveBytes + (64 << 10);
inline constexpr std::size_t kEventBatchBytes = 2 << 20;

} // namespace chronolog
