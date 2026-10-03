#pragma once

#include <array>
#include <limits>
#include <sys/timex.h>
#include "chronolog/types.h"

namespace chronolog::clock_test
{
struct NtpCase
{
    const char* name;
    int result;
    int flags;
    long maxerror_us;
    ClockStatus status;
    std::optional<uint64_t> bound_ns;
};
inline constexpr std::array cases{
        NtpCase{"SmallBound", TIME_OK, 0, 17, ClockStatus::Synced, 17'000},
        NtpCase{"ZeroBound", TIME_OK, 0, 0, ClockStatus::Synced, 0},
        NtpCase{"BelowCap", TIME_OK, 0, 999'999, ClockStatus::Synced, 999'999'000},
        NtpCase{"AtCap", TIME_OK, 0, 1'000'000, ClockStatus::Synced, 1'000'000'000},
        NtpCase{"AboveCap", TIME_OK, 0, 1'000'001, ClockStatus::Unsynced, std::nullopt},
        NtpCase{"UnsyncedFlag", TIME_OK, STA_UNSYNC, 17, ClockStatus::Unsynced, std::nullopt},
        NtpCase{"OtherFlags", TIME_OK, STA_PLL, 17, ClockStatus::Synced, 17'000},
        NtpCase{"LeapPending", TIME_INS, 0, 17, ClockStatus::Synced, 17'000},
        NtpCase{"TimeErrorUnsynced", TIME_ERROR, STA_UNSYNC, 17, ClockStatus::Unsynced, std::nullopt},
        NtpCase{"SyscallError", -1, 0, 17, ClockStatus::Unavailable, std::nullopt},
        NtpCase{"ErrorAtCap", -1, 0, 1'000'000, ClockStatus::Unavailable, std::nullopt},
        NtpCase{"NegativeBound", TIME_OK, 0, -1, ClockStatus::Unsynced, std::nullopt},
        NtpCase{"MinimumLong", TIME_OK, 0, std::numeric_limits<long>::min(), ClockStatus::Unsynced, std::nullopt},
        NtpCase{"MaximumLong", TIME_OK, 0, std::numeric_limits<long>::max(), ClockStatus::Unsynced, std::nullopt},
        NtpCase{"MultiplicationBoundary",
                TIME_OK,
                0,
                std::numeric_limits<int64_t>::max() / 1000,
                ClockStatus::Unsynced,
                std::nullopt},
        NtpCase{"MultiplicationOverflow",
                TIME_OK,
                0,
                std::numeric_limits<int64_t>::max() / 1000 + 1,
                ClockStatus::Unsynced,
                std::nullopt}};
} // namespace chronolog::clock_test
