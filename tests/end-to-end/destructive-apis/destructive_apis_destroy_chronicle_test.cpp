// End-to-end: DestroyChronicle cascades to every story's HDF5 files (#574).
//
// Flow:
//   1. CreateChronicle, AcquireStory ×2, log events on each, ReleaseStory ×2.
//   2. Verify both stories have HDF5 files on disk after Release (sync flush).
//   3. DestroyChronicle — Visor refuses if any story is held by another
//      client; otherwise auto-Releases sole-caller stories (we already
//      released both) and broadcasts destroy_chronicle to every Grapher.
//   4. Verify no <chronicle>.*.vlen.h5 files remain anywhere.

#include "destructive_apis_common.h"

int main(int argc, char** argv)
{
    using namespace destructive_apis;

    Args args;
    if(!parse_args(argc, argv, args))
        return 2;
    if(args.hdf5Dir.empty())
        return fail("hdf5 archive directory could not be resolved");

    std::string const suffix = unique_suffix();
    std::string const chronicle = "destroy_chr_chr_" + suffix;
    std::string const storyA = "destroy_chr_a_" + suffix;
    std::string const storyB = "destroy_chr_b_" + suffix;

    chronolog::Client client(load_portal_conf(args.clientConf), load_query_conf(args.clientConf));
    if(client.Connect() != chronolog::CL_SUCCESS)
        return fail("Connect failed");

    if(client.CreateChronicle(chronicle) != chronolog::CL_SUCCESS)
        return fail("CreateChronicle failed");

    auto [rcA, handleA] = client.AcquireStory(chronicle, storyA);
    if(rcA != chronolog::CL_SUCCESS || handleA == nullptr)
        return fail("AcquireStory A failed");
    auto [rcB, handleB] = client.AcquireStory(chronicle, storyB);
    if(rcB != chronolog::CL_SUCCESS || handleB == nullptr)
        return fail("AcquireStory B failed");

    if(write_events(handleA, 128, "chr_a") == 0)
        return fail("no events written to story A");
    if(write_events(handleB, 128, "chr_b") == 0)
        return fail("no events written to story B");

    if(client.ReleaseStory(chronicle, storyA) != chronolog::CL_SUCCESS)
        return fail("ReleaseStory A failed");
    if(client.ReleaseStory(chronicle, storyB) != chronolog::CL_SUCCESS)
        return fail("ReleaseStory B failed");

    size_t before = count_chronicle_files(args.hdf5Dir, chronicle);
    if(before == 0)
        return fail("no HDF5 files for chronicle " + chronicle + " after Release in " + args.hdf5Dir);
    std::cout << "[destroy-chronicle-test] after Release: " << before << " chronicle HDF5 file(s)" << std::endl;

    if(client.DestroyChronicle(chronicle) != chronolog::CL_SUCCESS)
        return fail("DestroyChronicle failed");

    size_t after = count_chronicle_files(args.hdf5Dir, chronicle);
    if(after != 0)
        return fail("DestroyChronicle returned but " + std::to_string(after) +
                    " HDF5 file(s) still exist for chronicle " + chronicle);

    client.Disconnect();
    return pass("DestroyChronicle removed " + std::to_string(before) + " HDF5 file(s)");
}
