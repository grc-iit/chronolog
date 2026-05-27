// End-to-end: DestroyStory removes HDF5 files (issue #574).
//
// Flow:
//   1. Connect, CreateChronicle, AcquireStory, log events.
//   2. ReleaseStory. The post-#574 contract is async: chunks are persisted
//      via the normal extraction-queue path on the Grapher, not by Release
//      itself. Poll until at least one HDF5 file appears.
//   3. DestroyStory. The Visor removes metadata synchronously and fans out
//      destroy RPCs to every Grapher; each Grapher's destroy is internally
//      async (it waits for extraction-queue drain, then deletes files). Poll
//      until no HDF5 files remain.

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

    // Release is async: poll until the Grapher's extraction-queue drain
    // writes at least one HDF5 file for this story.
    bool persisted = wait_for([&] { return count_story_files(args.hdf5Dir, chronicle, story) > 0; });
    size_t files_after_release = count_story_files(args.hdf5Dir, chronicle, story);
    if(!persisted)
    {
        return fail("Timed out waiting for HDF5 files to appear for " + chronicle + "/" + story + " in " +
                    args.hdf5Dir);
    }
    std::cout << "[destroy-story-test] after Release (polled): " << files_after_release << " HDF5 file(s)" << std::endl;

    if(client.DestroyStory(chronicle, story) != chronolog::CL_SUCCESS)
        return fail("DestroyStory failed");

    // Destroy is async on the Grapher: poll until the worker has waited for
    // extraction drain and deleted the matching HDF5 files.
    bool removed = wait_for([&] { return count_story_files(args.hdf5Dir, chronicle, story) == 0; });
    if(!removed)
    {
        return fail("Timed out waiting for HDF5 files to be removed for " + chronicle + "/" + story);
    }

    if(client.DestroyChronicle(chronicle) != chronolog::CL_SUCCESS)
        std::cerr << "[destroy-story-test] warning: cleanup DestroyChronicle failed" << std::endl;
    client.Disconnect();

    return pass("DestroyStory removed " + std::to_string(files_after_release) + " HDF5 file(s)");
}
