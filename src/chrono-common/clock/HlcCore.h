#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <mutex>

#include "chronolog/types.h"

namespace chronolog
{

// Standard hybrid logical clock. Physical time is supplied by the caller so one core serves every Clock.
class HlcCore
{
public:
    Hlc tick(int64_t physical_ns)
    {
        std::lock_guard lock(mutex_);
        return advance(physical_ns, Hlc{});
    }

    Hlc observe(int64_t physical_ns, Hlc remote)
    {
        std::lock_guard lock(mutex_);
        return advance(physical_ns, remote);
    }

private:
    Hlc advance(int64_t physical_ns, const Hlc& remote)
    {
        int64_t physical = std::max({physical_ns, last_.physical_ns, remote.physical_ns});
        uint32_t logical = 0;
        if(physical == last_.physical_ns || physical == remote.physical_ns)
        {
            uint32_t base = std::max(physical == last_.physical_ns ? last_.logical : 0u,
                                     physical == remote.physical_ns ? remote.logical : 0u);
            if(base == std::numeric_limits<uint32_t>::max())
                ++physical;
            else
                logical = base + 1;
        }
        last_ = Hlc{physical, logical};
        return last_;
    }

    std::mutex mutex_;
    Hlc last_{};
};

} // namespace chronolog
