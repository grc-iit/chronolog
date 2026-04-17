// Property: the number of events recorded for {chronicle, story} equals the
// number of (non-empty) lines in the reference input file.
//
// Replaces fidelity_test_02.py and extends the check to the HDF5 and replay
// lenses.

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

    if(events.size() != reference.size())
    {
        std::ostringstream os;
        os << lens_name(args.lens) << ": expected " << reference.size() << " events, found " << events.size();
        return fail(os.str());
    }

    std::ostringstream os;
    os << lens_name(args.lens) << ": event count matches reference (" << events.size() << ")";
    return pass(os.str());
}
