// Property: events recorded for {chronicle, story} appear in the same order
// as the lines of the reference input file.
//
// Replaces fidelity_test_03.py and extends the check to the HDF5 and replay
// lenses. Records are matched by their logRecord payload — collect_events
// already sorts the recorded events by (eventTime, clientId, eventIndex), so
// this check verifies the ChronoLog-side timestamping preserved the input
// order.

#include "data_integrity_common.h"

int main(int argc, char** argv)
{
    using namespace data_integrity;
    Args args;
    if(!parse_args(argc, argv, args))
        return 2;

    auto reference = read_reference_lines(args.referenceFile);
    if(reference.empty())
    {
        return fail("reference input is empty or missing: " + args.referenceFile);
    }

    auto events = collect_events(args);
    if(events.empty())
    {
        return skip(std::string("no events found via ") + lens_name(args.lens) + " lens for " + args.chronicle + "/" +
                    args.story);
    }

    // Walk the reference lines in order; each must appear in the events list
    // at or after the cursor position.
    size_t cursor = 0;
    for(size_t i = 0; i < reference.size(); ++i)
    {
        bool found = false;
        for(size_t j = cursor; j < events.size(); ++j)
        {
            if(events[j].logRecord == reference[i])
            {
                cursor = j + 1;
                found = true;
                break;
            }
        }
        if(!found)
        {
            std::ostringstream os;
            os << lens_name(args.lens) << ": reference line " << (i + 1) << " not found in order: '" << reference[i]
               << "'";
            return fail(os.str());
        }
    }

    std::ostringstream os;
    os << lens_name(args.lens) << ": event order matches reference (" << reference.size() << " lines)";
    return pass(os.str());
}
