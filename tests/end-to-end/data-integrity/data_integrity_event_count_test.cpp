// Property: the number of events recorded for {chronicle, story} equals the
// number of (non-empty) lines in the reference input file.
//
// Replaces fidelity_test_02.py, reading from the on-disk HDF5 archive.

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

    if(events.size() != reference.size())
    {
        std::ostringstream os;
        os << "expected " << reference.size() << " events, found " << events.size();
        return fail(os.str());
    }

    std::ostringstream os;
    os << "event count matches reference (" << events.size() << ")";
    return pass(os.str());
}
