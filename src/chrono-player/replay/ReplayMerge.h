#pragma once

#include <vector>
#include "chronolog/types.h"

namespace chronolog::player
{

// K-way merge of inputs each sorted by ReplayLess. Events with equal EventId collapse to one,
// keeping the highest achieved durability.
std::vector<Event> mergeReplay(std::vector<std::vector<Event>> inputs);

} // namespace chronolog::player
