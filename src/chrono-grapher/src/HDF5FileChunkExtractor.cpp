#include <filesystem>
#include <json-c/json.h>
#include <regex>
#include <string>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <StoryChunk.h>
#include <StoryChunkWriter.h>
#include <HDF5FileChunkExtractor.h>
#include <set>

#include <H5Cpp.h>

#include <ArchiveManifest.h>
#include <StoryWatermarkRegistry.h>

namespace tl = thallium;

namespace chl = chronolog;

chronolog::HDF5FileChunkExtractor::HDF5FileChunkExtractor(const std::string& hdf5_files_root_dir)
    : rootDirectory(hdf5_files_root_dir)
{
    LOG_TRACE("[HDF5FileChunkExtractor] Destructor called. Cleaning up...");
}

chronolog::HDF5FileChunkExtractor::~HDF5FileChunkExtractor()
{
    LOG_TRACE("[HDF5FileChunkExtractor] Destructor called. Cleaning up...");
}
//////

int chronolog::HDF5FileChunkExtractor::reset(std::string const& new_archive_dir)
{
    rootDirectory = new_archive_dir;
    LOG_INFO("HDF5FileChunkExtractor] Reset success: using directory :", rootDirectory);
    return chl::CL_SUCCESS;
}

// json block for HDF5 File Chunk Extractor looks like this
//
//  "extractor_name": {
//                "type": "hdf5_extractor",
//                "hdf5_archive_dir": "/tmp/hdf5_archive"
//               }
//
//////////

int chronolog::HDF5FileChunkExtractor::reset(json_object* json_block)
{
    if((json_block == nullptr) || !json_object_is_type(json_block, json_type_object) ||
       (json_object_object_get(json_block, "type") == nullptr) ||
       !json_object_is_type(json_object_object_get(json_block, "type"), json_type_string) ||
       (std::string("hdf5_extractor").compare(json_object_get_string(json_object_object_get(json_block, "type"))) != 0))
    {
        rootDirectory = "/tmp";
        LOG_ERROR("HDF5FileChunkExtractor] Reset failure: invalid json_conf; using {}", rootDirectory);
        return chl::CL_ERR_INVALID_CONF;
    }

    if((json_object_object_get(json_block, "hdf5_archive_dir") == nullptr) ||
       !json_object_is_type(json_object_object_get(json_block, "hdf5_archive_dir"), json_type_string))
    {
        rootDirectory = "/tmp";
        LOG_ERROR("HDF5FileChunkExtractor] Reset failure: invalid json_conf; using {} ", rootDirectory);
        return chl::CL_ERR_INVALID_CONF;
    }

    rootDirectory = json_object_get_string(json_object_object_get(json_block, "hdf5_archive_dir"));

    // check if archive directory exists and is writable by the extractor process
    if(!std::filesystem::exists(rootDirectory))
    {
        rootDirectory = "/tmp";
        LOG_ERROR("HDF5FileChunkExtractor] Reset failure: hdf5_archive_dir doesn't exist or not writable; using {} ",
                  rootDirectory);
        return chl::CL_ERR_INVALID_CONF;
    }

    LOG_INFO("HDF5FileChunkExtractor] Reset success: using {}", rootDirectory);
    return chl::CL_SUCCESS;
}

//////


namespace
{
// Escape characters that have special meaning in std::regex so chronicle/story
// names containing them (e.g. dots) are matched literally rather than as
// metacharacters.
std::string regex_escape(std::string const& in)
{
    return std::regex_replace(in, std::regex(R"([.^$|()\\*+?{}\[\]])"), R"(\$&)");
}

int delete_matching_files(std::string const& root_directory,
                          std::regex const& filename_pattern,
                          std::string const& what,
                          size_t* deleted_count,
                          std::vector<std::string>* deleted_files)
{
    size_t local_count = 0;
    if(!std::filesystem::exists(root_directory))
    {
        LOG_DEBUG("[HDF5FileChunkExtractor] Archive directory {} does not exist; nothing to delete for {}",
                  root_directory,
                  what);
        if(deleted_count != nullptr)
        {
            *deleted_count = 0;
        }
        return chl::CL_SUCCESS;
    }

    // std::filesystem::directory_iterator's constructor reports an open
    // failure (e.g. EACCES on the archive directory) by setting `ec` and
    // returning an end-iterator. Previously we only checked `ec` inside the
    // loop body, so a failed open looked like an empty directory and
    // delete_*_files returned CL_SUCCESS with zero deletions -- the metadata
    // would be gone but the files would still be on disk. Check `ec`
    // immediately after construction.
    std::error_code ec;
    std::filesystem::directory_iterator it(root_directory, ec);
    if(ec)
    {
        LOG_ERROR("[HDF5FileChunkExtractor] Failed to open directory {}: {}", root_directory, ec.message());
        return chl::CL_ERR_UNKNOWN;
    }
    for(auto const& entry: it)
    {
        if(!entry.is_regular_file())
        {
            continue;
        }
        std::string const filename = entry.path().filename().string();
        if(!std::regex_match(filename, filename_pattern))
        {
            continue;
        }
        std::error_code rm_ec;
        std::filesystem::remove(entry.path(), rm_ec);
        if(rm_ec)
        {
            LOG_ERROR("[HDF5FileChunkExtractor] Failed to delete {}: {}", entry.path().string(), rm_ec.message());
            return chl::CL_ERR_UNKNOWN;
        }
        LOG_INFO("[HDF5FileChunkExtractor] Deleted {} ({})", entry.path().string(), what);
        if(deleted_files != nullptr)
        {
            deleted_files->push_back(filename);
        }
        ++local_count;
    }
    if(deleted_count != nullptr)
    {
        *deleted_count = local_count;
    }
    return chl::CL_SUCCESS;
}
} // namespace

int chronolog::HDF5FileChunkExtractor::delete_story_files(std::string const& chronicle_name,
                                                          std::string const& story_name,
                                                          size_t* deleted_count)
{
    // Matches the filename layout produced by StoryChunkWriter::writeStoryChunk:
    //   <chronicle>.<story>.<startSec>.vlen.h5 (and rotated <...>.<n>.vlen.h5).
    std::string const pattern_str =
            regex_escape(chronicle_name) + "\\." + regex_escape(story_name) + "\\.[0-9]+(\\.[0-9]+)?\\.vlen\\.h5";
    std::regex const filename_pattern(pattern_str);
    std::string const what = "story " + chronicle_name + "/" + story_name;
    std::vector<std::string> deleted_files;
    int const ret = delete_matching_files(rootDirectory, filename_pattern, what, deleted_count, &deleted_files);
    recordDeletions(chronicle_name, story_name, deleted_files);
    return ret;
}

int chronolog::HDF5FileChunkExtractor::delete_chronicle_files(std::string const& chronicle_name, size_t* deleted_count)
{
    // Matches any story under the chronicle:
    //   <chronicle>.<anyStory>.<startSec>(.<n>)?.vlen.h5
    std::string const pattern_str = regex_escape(chronicle_name) + "\\.[^.]+\\.[0-9]+(\\.[0-9]+)?\\.vlen\\.h5";
    std::regex const filename_pattern(pattern_str);
    std::string const what = "chronicle " + chronicle_name;
    std::vector<std::string> deleted_files;
    int const ret = delete_matching_files(rootDirectory, filename_pattern, what, deleted_count, &deleted_files);
    recordDeletions(chronicle_name, std::string(), deleted_files);
    return ret;
}

int chronolog::HDF5FileChunkExtractor::process_chunk(chl::StoryChunk* story_chunk)
{
    LOG_INFO("[HDF5FileChunkExtractor] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} eventCount {}",
             thallium::thread::self_id(),
             story_chunk->getStoryId(),
             story_chunk->getChronicleName(),
             story_chunk->getStoryName(),
             story_chunk->getStartTime(),
             story_chunk->getEndTime(),
             story_chunk->getEventCount());

    if(story_chunk->empty())
    {
        // an idle-gap window: nothing to write, but the interval is vacuously
        // durable and must extend the persisted watermark or the contiguous
        // prefix would hold at the gap forever
        if(watermarkRegistry != nullptr && !story_chunk->isWatermarkExempt())
        {
            watermarkRegistry->advancePersisted(story_chunk->getStoryId(),
                                                story_chunk->getStartTime(),
                                                story_chunk->getEndTime());
        }
        recordInManifest(story_chunk, chl::ManifestState::EMPTY, "", 0);
        return chl::CL_SUCCESS;
    }

    StoryChunkWriter chunkWriter(rootDirectory, "story_chunks", "data");
    // write_result carries the published name and rotation index as well as the
    // size; the archive manifest records those in a later step.
    StoryChunkWriteResult const write_result = chunkWriter.writeStoryChunk(*story_chunk);
    hsize_t size = write_result.file_size;
    if(size == 0)
    {
        LOG_ERROR("[HDF5FileChunkExtractor] Error writing StoryChunk to file: StoryId={} {}-{} {}-{} eventCount {}",
                  story_chunk->getStoryId(),
                  story_chunk->getChronicleName(),
                  story_chunk->getStoryName(),
                  story_chunk->getStartTime(),
                  story_chunk->getEndTime(),
                  story_chunk->getEventCount());
        if(watermarkRegistry != nullptr && !story_chunk->isWatermarkExempt())
        {
            watermarkRegistry->persistFailed(story_chunk->getStoryId());
        }
        // FAILED, not EMPTY: nothing was written, so this window must never
        // advance W. It is still recorded so the gap is explicable afterwards.
        recordInManifest(story_chunk, chl::ManifestState::FAILED, "", 0);
        return chl::CL_ERR_UNKNOWN;
    }
    else
    {
        LOG_INFO("[HDF5FileChunkExtractor] StoryChunk written to file: StoryId={} {}-{} {}-{} eventCount {}",
                 story_chunk->getStoryId(),
                 story_chunk->getChronicleName(),
                 story_chunk->getStoryName(),
                 story_chunk->getStartTime(),
                 story_chunk->getEndTime(),
                 story_chunk->getEventCount());
        // StoryChunkWriter flushes H5F_SCOPE_GLOBAL before returning, so the
        // window counts as persisted here. Salvage chunks are exempt: they are
        // one keeper's rescued events, not a merged timeline window.
        if(watermarkRegistry != nullptr && !story_chunk->isWatermarkExempt())
        {
            watermarkRegistry->advancePersisted(story_chunk->getStoryId(),
                                                story_chunk->getStartTime(),
                                                story_chunk->getEndTime());
        }
        // Appended only after the file is published, so the manifest can never
        // name a file that is not fully on disk.
        recordInManifest(story_chunk, chl::ManifestState::PUBLISHED, write_result.file_name, write_result.seq);
        return chl::CL_SUCCESS;
    }
}

void chronolog::HDF5FileChunkExtractor::recordInManifest(chl::StoryChunk const* story_chunk,
                                                         chl::ManifestState state,
                                                         std::string const& file_path,
                                                         uint32_t seq)
{
    if(archiveManifest == nullptr)
    {
        return;
    }
    chl::ArchiveManifestRecord record;
    record.chronicle = story_chunk->getChronicleName();
    record.story = story_chunk->getStoryName();
    record.story_id = story_chunk->getStoryId();
    // The base name only: the manifest is relative to the archive root it lives
    // in, so it stays valid if the archive is moved.
    record.file = file_path.empty() ? std::string() : std::filesystem::path(file_path).filename().string();
    record.start = story_chunk->getStartTime();
    record.end = story_chunk->getEndTime();
    record.seq = seq;
    record.events = story_chunk->getEventCount();
    record.state = state;
    record.exempt = story_chunk->isWatermarkExempt();
    archiveManifest->append(record);
}

void chronolog::HDF5FileChunkExtractor::recordDeletions(std::string const& chronicle_name,
                                                        std::string const& story_name,
                                                        std::vector<std::string> const& deleted_files)
{
    if(archiveManifest == nullptr || deleted_files.empty())
    {
        return;
    }
    // The log is append-only, so a deletion is a new record naming the removed
    // file rather than an edit of the old one. deriveWatermarks() matches them by
    // file name, which is why the window fields are left at zero here: the
    // superseded record already carries the window.
    for(std::string const& file: deleted_files)
    {
        chl::ArchiveManifestRecord record;
        record.chronicle = chronicle_name;
        record.story = story_name;
        record.file = file;
        record.state = chl::ManifestState::DELETED;
        archiveManifest->append(record);
    }
}

std::string chronolog::HDF5FileChunkExtractor::archiveDirectoryFromConf(json_object* json_block)
{
    if((json_block == nullptr) || !json_object_is_type(json_block, json_type_object))
    {
        return std::string();
    }
    json_object* dir = json_object_object_get(json_block, "hdf5_archive_dir");
    if((dir == nullptr) || !json_object_is_type(dir, json_type_string))
    {
        return std::string();
    }
    return json_object_get_string(dir);
}

namespace
{

// The identity fields of the stored compound type. Only these are read: pulling the
// variable-length payload would make HDF5 allocate a buffer per event that the
// caller then owns, and nothing here needs payloads.
struct StoredIdentity
{
    uint64_t event_time;
    uint32_t client_id;
    uint32_t event_index;
    uint64_t story_id;
};

// Recovers the story id and last event time from a published archive file. Returns
// false when the file cannot be read as one, which is how non-archive files and
// in-progress writes are rejected.
bool probe_archive_file(std::string const& path, chl::StoryId& story_id, uint64_t& last_event_time)
{
    try
    {
        H5::Exception::dontPrint();
        H5::H5File file(path, H5F_ACC_RDONLY);
        H5::DataSet dataset = file.openDataSet("/story_chunks/data.vlen_bytes");

        hsize_t dims[1] = {0};
        dataset.getSpace().getSimpleExtentDims(dims, nullptr);
        if(dims[0] == 0)
        {
            return false;
        }

        H5::CompType identity_type(sizeof(StoredIdentity));
        identity_type.insertMember("eventTime", HOFFSET(StoredIdentity, event_time), H5::PredType::NATIVE_UINT64);
        identity_type.insertMember("clientId", HOFFSET(StoredIdentity, client_id), H5::PredType::NATIVE_UINT32);
        identity_type.insertMember("eventIndex", HOFFSET(StoredIdentity, event_index), H5::PredType::NATIVE_UINT32);
        identity_type.insertMember("storyId", HOFFSET(StoredIdentity, story_id), H5::PredType::NATIVE_UINT64);

        std::vector<StoredIdentity> raw(dims[0]);
        dataset.read(raw.data(), identity_type);

        story_id = raw.front().story_id;
        last_event_time = 0;
        for(StoredIdentity const& event: raw)
        {
            if(event.event_time > last_event_time)
            {
                last_event_time = event.event_time;
            }
        }
        return true;
    }
    catch(H5::Exception const&)
    {
        return false;
    }
}

// "<chronicle>.<story>.<startSec>.vlen.h5", or "...vlen.<n>.h5" for a rotation.
bool parse_archive_file_name(std::string const& base_name,
                             std::string& chronicle,
                             std::string& story,
                             uint64_t& start_ns,
                             uint32_t& seq)
{
    if(base_name.size() < 4 || base_name.substr(base_name.size() - 3) != ".h5")
    {
        return false;
    }
    std::size_t const first = base_name.find('.');
    if(first == std::string::npos)
    {
        return false;
    }
    std::size_t const second = base_name.find('.', first + 1);
    if(second == std::string::npos)
    {
        return false;
    }
    std::size_t const third = base_name.find('.', second + 1);
    if(third == std::string::npos)
    {
        return false;
    }
    chronicle = base_name.substr(0, first);
    story = base_name.substr(first + 1, second - first - 1);
    std::string const start_text = base_name.substr(second + 1, third - second - 1);
    if(start_text.empty() || start_text.find_first_not_of("0123456789") != std::string::npos)
    {
        return false;
    }
    try
    {
        start_ns = std::stoull(start_text) * 1000000000ULL;
    }
    catch(std::exception const&)
    {
        return false;
    }
    seq = chl::StoryChunkWriter::rotationIndexOf(base_name);
    return true;
}

} // namespace

int chronolog::HDF5FileChunkExtractor::reconcileManifestWithDirectory()
{
    if(archiveManifest == nullptr)
    {
        return 0;
    }

    std::set<std::string> referenced;
    for(chl::ArchiveManifestRecord const& record: archiveManifest->records())
    {
        if(!record.file.empty())
        {
            // Both published and deleted names count as "the manifest knows about
            // this file": a deleted one must never be adopted back.
            referenced.insert(record.file);
        }
    }

    std::error_code ec;
    std::filesystem::directory_iterator dir(rootDirectory, ec);
    if(ec)
    {
        LOG_WARNING("[HDF5FileChunkExtractor] Cannot reconcile {}: {}", rootDirectory, ec.message());
        return 0;
    }

    int adopted = 0;
    for(auto const& entry: dir)
    {
        if(!entry.is_regular_file(ec))
        {
            continue;
        }
        std::string const base_name = entry.path().filename().string();
        if(referenced.count(base_name) > 0)
        {
            continue;
        }

        chl::ArchiveManifestRecord record;
        if(!parse_archive_file_name(base_name, record.chronicle, record.story, record.start, record.seq))
        {
            continue; // not an archive file name (stray files, in-progress .tmp writes)
        }
        uint64_t last_event_time = 0;
        if(!probe_archive_file(entry.path().string(), record.story_id, last_event_time))
        {
            LOG_WARNING("[HDF5FileChunkExtractor] {} looks like an archive file but could not be read; not adopting",
                        base_name);
            continue;
        }

        record.file = base_name;
        // Only what the events prove; see the header note on why this is not the
        // window's true end.
        record.end = last_event_time + 1;
        record.state = chl::ManifestState::PUBLISHED;

        if(archiveManifest->append(record) == chl::CL_SUCCESS)
        {
            adopted++;
            LOG_WARNING("[HDF5FileChunkExtractor] Adopted unreferenced archive file {} ({}-{}, story {}): it was "
                        "published but its manifest record was lost, most likely to an unclean shutdown",
                        base_name,
                        record.start,
                        record.end,
                        record.story_id);
        }
    }

    if(adopted > 0)
    {
        LOG_WARNING("[HDF5FileChunkExtractor] Adopted {} unreferenced archive file(s) in {}", adopted, rootDirectory);
    }
    return adopted;
}
