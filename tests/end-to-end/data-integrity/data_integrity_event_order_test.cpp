// Property: events recorded for {chronicle, story} appear in the same order
// as the lines of the reference input file.
//
// Replaces fidelity_test_03.py, reading from the on-disk HDF5 archive.
// Records are matched by their logRecord payload — collect_events already
// sorts the recorded events by (eventTime, clientId, eventIndex), so this
// check verifies the ChronoLog-side timestamping preserved the input order.

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
        return fail("no events found in " + args.hdf5Dir + " for " + args.chronicle + "/" + args.story);
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
            os << "reference line " << (i + 1) << " not found in order: '" << reference[i] << "'";
            return fail(os.str());
        }
    }

    std::ostringstream os;
    os << "event order matches reference (" << reference.size() << " lines)";
    return pass(os.str());
}
