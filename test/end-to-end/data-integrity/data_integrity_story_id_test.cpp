// Property: every event recorded for {chronicle, story} carries the same
// StoryID — and that StoryID equals the value derived from the chronicle and
// story names (CityHash64(chronicle + story)).
//
// Replaces fidelity_test_01.py and extends the check to the HDF5 and replay
// lenses.

#include "data_integrity_common.h"

int main(int argc, char** argv)
{
    using namespace data_integrity;
    Args args;
    if(!parse_args(argc, argv, args))
        return 2;

    auto events = collect_events(args);
    if(events.empty())
    {
        return skip(std::string("no events found via ") + lens_name(args.lens) + " lens for " + args.chronicle + "/" +
                    args.story);
    }

    uint64_t expected = expected_story_id(args.chronicle, args.story);
    for(auto const& ev: events)
    {
        if(ev.storyId != expected)
        {
            std::ostringstream os;
            os << lens_name(args.lens) << ": event has storyId=" << ev.storyId << ", expected " << expected
               << " (chronicle=" << args.chronicle << ", story=" << args.story << ")";
            return fail(os.str());
        }
    }

    std::ostringstream os;
    os << lens_name(args.lens) << ": all " << events.size() << " events carry storyId=" << expected;
    return pass(os.str());
}
