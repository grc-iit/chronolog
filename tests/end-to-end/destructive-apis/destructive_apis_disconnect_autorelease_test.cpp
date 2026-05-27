// End-to-end: Disconnect auto-releases acquired stories (originally landed
// in #642).
//
// Flow:
//   1. Client A connects, creates chronicle, acquires story, logs events,
//      then Disconnects without calling ReleaseStory.
//   2. Visor's ClientDisconnect must auto-Release for A. Release is async
//      under the post-#574 design: persistence runs through the normal
//      extraction-queue path on the Grapher. Poll until HDF5 files appear.
//
// We don't instantiate a second Client in this same process to clean up:
// ClientId is derived from PID, so two Client instances in one process get
// the same id and confuse the server-side state. A separate-process clean-up
// would over-test what the property here actually demands.

#include "destructive_apis_common.h"

#include <client_errcode.h>

int main(int argc, char** argv)
{
    using namespace destructive_apis;

    Args args;
    if(!parse_args(argc, argv, args))
        return 2;
    if(args.hdf5Dir.empty())
        return fail("hdf5 archive directory could not be resolved");

    std::string const suffix = unique_suffix();
    std::string const chronicle = "discon_chr_" + suffix;
    std::string const story = "discon_sty_" + suffix;

    {
        chronolog::Client clientA(load_portal_conf(args.clientConf), load_query_conf(args.clientConf));
        if(clientA.Connect() != chronolog::CL_SUCCESS)
            return fail("Client A Connect failed");
        if(clientA.CreateChronicle(chronicle) != chronolog::CL_SUCCESS)
            return fail("CreateChronicle failed");
        auto [rcA, handleA] = clientA.AcquireStory(chronicle, story);
        if(rcA != chronolog::CL_SUCCESS || handleA == nullptr)
            return fail("AcquireStory failed");
        if(write_events(handleA, 128, "discon") == 0)
            return fail("no events written");

        // No ReleaseStory call. Disconnect must auto-release; persistence
        // is then async via the extraction-queue path.
        int dis_rc = clientA.Disconnect();
        if(dis_rc != chronolog::CL_SUCCESS)
            return fail("Disconnect returned " + std::to_string(dis_rc) +
                        " (expected CL_SUCCESS; legacy behavior was CL_ERR_ACQUIRED)");
    }

    if(!wait_for([&] { return count_story_files(args.hdf5Dir, chronicle, story) > 0; }))
    {
        return fail("Timed out waiting for Disconnect auto-release to persist HDF5 files for " + chronicle + "/" +
                    story + " in " + args.hdf5Dir);
    }
    size_t files = count_story_files(args.hdf5Dir, chronicle, story);

    return pass("Disconnect auto-released; " + std::to_string(files) + " HDF5 file(s) persisted");
}
