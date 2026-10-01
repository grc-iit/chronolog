#include "chrono-player/replay/ReplayMerge.h"
#include <queue>
#include <utility>

namespace chronolog::player
{

std::vector<Event> mergeReplay(std::vector<std::vector<Event>> inputs)
{
    struct Head
    {
        size_t input;
        size_t pos;
    };
    auto after = [&](const Head& a, const Head& b)
    { return ReplayLess(inputs[b.input][b.pos], inputs[a.input][a.pos]); };
    std::priority_queue<Head, std::vector<Head>, decltype(after)> heads(after);
    size_t total = 0;
    for(size_t i = 0; i < inputs.size(); ++i)
    {
        total += inputs[i].size();
        if(!inputs[i].empty())
            heads.push({i, 0});
    }
    std::vector<Event> out;
    out.reserve(total);
    while(!heads.empty())
    {
        Head h = heads.top();
        heads.pop();
        Event& e = inputs[h.input][h.pos];
        // Equal identities sort adjacent, but a story_id mismatch can interleave, so scan the run
        // of events that compare equal under ReplayLess.
        Event* dup = nullptr;
        for(size_t i = out.size(); i-- > 0 && !ReplayLess(out[i], e);)
        {
            if(out[i].id == e.id)
            {
                dup = &out[i];
                break;
            }
        }
        if(!dup)
            out.push_back(std::move(e));
        else if(e.durability > dup->durability)
            dup->durability = e.durability;
        if(++h.pos < inputs[h.input].size())
            heads.push(h);
    }
    return out;
}

} // namespace chronolog::player
