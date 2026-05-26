// End-to-end: DestroyStory removes HDF5 files (issue #574).
//
// Flow:
//   1. Connect, CreateChronicle, AcquireStory, log events.
//   2. ReleaseStory — the sync-flush path must push remaining chunks to
//      the Grapher's HDF5 archive before Release returns.
//   3. Verify at least one <chronicle>.<story>.*.vlen.h5 file exists.
//   4. DestroyStory — must auto-release-if-sole-acquirer is a no-op here
//      because we already released; Visor then broadcasts destroy_story to
//      every Grapher which deletes the matching files.
//   5. Verify no <chronicle>.<story>.*.vlen.h5 files remain.

#include "destructive_apis_common.h"

int main(int argc, char** argv)
{
    using namespace destructive_apis;

    Args args;
    if(!parse_args(argc, argv, args))
        return 2;
    if(args.hdf5Dir.empty())
    {
        return fail("hdf5 archive directory could not be resolved (set --hdf5-dir or CHRONOLOG_INSTALL_DIR)");
    }

    std::string const suffix = unique_suffix();
    std::string const chronicle = "destroy_story_chr_" + suffix;
    std::string const story = "destroy_story_sty_" + suffix;

    chronolog::Client client(load_portal_conf(args.clientConf), load_query_conf(args.clientConf));
    if(client.Connect() != chronolog::CL_SUCCESS)
        return fail("Connect failed");

    if(client.CreateChronicle(chronicle) != chronolog::CL_SUCCESS)
        return fail("CreateChronicle failed");

    auto [acq_rc, handle] = client.AcquireStory(chronicle, story);
    if(acq_rc != chronolog::CL_SUCCESS || handle == nullptr)
        return fail("AcquireStory failed rc=" + std::to_string(acq_rc));

    size_t const events = 256;
    size_t written = write_events(handle, events, "destroy_story");
    if(written == 0)
        return fail("no events written");
    std::cout << "[destroy-story-test] wrote " << written << " events" << std::endl;

    if(client.ReleaseStory(chronicle, story) != chronolog::CL_SUCCESS)
        return fail("ReleaseStory failed");

    // With the issue-#574 sync flush, the HDF5 file must be on disk by the
    // time ReleaseStory returns. No polling needed.
    size_t files_after_release = count_story_files(args.hdf5Dir, chronicle, story);
    if(files_after_release == 0)
    {
        return fail("ReleaseStory returned but no HDF5 files were written for " + chronicle + "/" + story + " in " +
                    args.hdf5Dir);
    }
    std::cout << "[destroy-story-test] after Release: " << files_after_release << " HDF5 file(s)" << std::endl;

    if(client.DestroyStory(chronicle, story) != chronolog::CL_SUCCESS)
        return fail("DestroyStory failed");

    size_t files_after_destroy = count_story_files(args.hdf5Dir, chronicle, story);
    if(files_after_destroy != 0)
    {
        return fail("DestroyStory returned but " + std::to_string(files_after_destroy) +
                    " HDF5 file(s) still exist for " + chronicle + "/" + story);
    }

    if(client.DestroyChronicle(chronicle) != chronolog::CL_SUCCESS)
        std::cerr << "[destroy-story-test] warning: cleanup DestroyChronicle failed" << std::endl;
    client.Disconnect();

    return pass("DestroyStory removed " + std::to_string(files_after_release) + " HDF5 file(s)");
}
