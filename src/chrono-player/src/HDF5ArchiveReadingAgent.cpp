#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <H5Cpp.h>

#include <ArchiveLayout.h>
#include <chronolog_errcode.h>
#include <StoryChunkWriter.h>
#include <HDF5FileAccess.h>
#include <HDF5ArchiveReadingAgent.h>

// Helper function to format uint64_t with comma separators
std::string formatWithCommas(uint64_t value)
{
    std::string str = std::to_string(value);
    int pos = str.length() - 3;
    while(pos > 0)
    {
        str.insert(pos, ",");
        pos -= 3;
    }
    return str;
}

namespace chronolog
{
namespace
{
// how much of a manifest log one read takes at most
constexpr std::size_t kManifestPieceBytes = 4 * 1024 * 1024;
} // namespace

struct ErrorReport
{
    std::vector<std::string> messages;
};

herr_t error_walker(unsigned int n, const H5E_error2_t* err_desc, void* client_data)
{
    // Cast the client_data back to our ErrorReport struct
    auto* report = static_cast<ErrorReport*>(client_data);

    // Get the major and minor error strings
    char maj[256], min[256];
    H5Eget_msg(err_desc->maj_num, nullptr, maj, 256);
    H5Eget_msg(err_desc->min_num, nullptr, min, 256);

    // Format the detailed error message
    std::string msg = "HDF5 Error #" + std::to_string(n) + ":\n" + "  File: " + err_desc->file_name + "\n" +
                      "  Line: " + std::to_string(err_desc->line) + "\n" + "  Function: " + err_desc->func_name + "\n" +
                      "  Major Error: " + maj + "\n" + "  Minor Error: " + min + "\n" +
                      "  Description: " + err_desc->desc;

    // Add the formatted message to our report
    report->messages.push_back(msg);

    // Return 0 to continue walking the stack
    return 0;
}

int chronolog::HDF5ArchiveReadingAgent::readStoryChunkFile(const ChronicleName& chronicleName,
                                                           const StoryName& storyName,
                                                           uint64_t startTime,
                                                           uint64_t endTime,
                                                           std::list<StoryChunk*>& listOfChunks,
                                                           const std::string& file_name)
{
    std::unique_ptr<H5::H5File> file;
    StoryChunk* story_chunk = nullptr;
    bool has_events_outside_range = false;
    try
    {
        H5::Exception::dontPrint();

        LOG_DEBUG("[HDF5ArchiveReadingAgent] Opening file {}", file_name);
        // locking off: the grapher writes these files from another node while
        // this read is in flight (see HDF5FileAccess.h)
        file = std::make_unique<H5::H5File>(file_name,
                                            H5F_ACC_SWMR_READ,
                                            H5::FileCreatPropList::DEFAULT,
                                            archiveFileAccess());

        std::string dataset_name = "/story_chunks/data.vlen_bytes";
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Opening dataset {}", dataset_name);
        H5::DataSet dataset = file->openDataSet(dataset_name);

        H5::DataSpace dataspace = dataset.getSpace();
        hsize_t dims_out[2] = {0, 0};
        dataspace.getSimpleExtentDims(dims_out, nullptr);
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading dataset {} with {} events",
                  dataset_name,
                  formatWithCommas(dims_out[0]));

        H5::CompType defined_comp_type = StoryChunkWriter::createEventCompoundType();
        H5::CompType probed_data_type = dataset.getCompType();
        if(probed_data_type.getNmembers() != defined_comp_type.getNmembers())
        {
            LOG_WARNING(
                    "[HDF5ArchiveReadingAgent] Error reading dataset {} : Not a compound type with the same #members",
                    file_name);
            return CL_ERR_UNKNOWN;
        }
        // a file written before client ids were widened has a 32-bit clientId;
        // reading it through the current type widens the id
        if(probed_data_type != defined_comp_type &&
           probed_data_type != StoryChunkWriter::createLegacyEventCompoundType())
        {
            LOG_WARNING("[HDF5ArchiveReadingAgent]Error reading dataset {} : Compound type mismatch", file_name);
            return CL_ERR_UNKNOWN;
        }

        LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading data from dataset {}", dataset_name);
        std::vector<LogEventHVL> data;
        data.resize(dims_out[0]);
        dataset.read(data.data(), defined_comp_type);
        // HDF5 allocates each variable-length record with malloc. Give the
        // buffers back to HDF5 on the way out of this scope and detach them,
        // or ~LogEventHVL frees them with delete[].
        struct VlenRecordReclaim
        {
            std::vector<LogEventHVL>& records;
            H5::CompType const& type;
            H5::DataSpace const& space;

            ~VlenRecordReclaim()
            {
                if(records.empty())
                {
                    return;
                }
                try
                {
                    H5::DataSet::vlenReclaim(records.data(), type, space);
                }
                catch(H5::Exception const&)
                {
                    LOG_ERROR("[HDF5ArchiveReadingAgent] Failed to reclaim variable-length records");
                }
                for(auto& record: records)
                {
                    record.logRecord.p = nullptr;
                    record.logRecord.len = 0;
                }
            }
        } reclaim_records{data, defined_comp_type, dataspace};

        LOG_DEBUG("[HDF5ArchiveReadingAgent] Creating StoryChunk {}-{} range {}-{}...",
                  chronicleName,
                  storyName,
                  formatWithCommas(startTime),
                  formatWithCommas(endTime));
        uint64_t story_id = 0;
        if(!data.empty())
        {
            story_id = data[0].storyId;
        }
        story_chunk = new StoryChunk(chronicleName, storyName, story_id, startTime, endTime);
        for(auto const& event_hvl: data)
        {
            if(event_hvl.eventTime < startTime)
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] Skipping event with time {} outside range {}-{}",
                          formatWithCommas(event_hvl.eventTime),
                          formatWithCommas(startTime),
                          formatWithCommas(endTime));
                continue;
            }
            if(event_hvl.eventTime >= endTime)
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] Stopping reading events with time {} outside range {}-{}",
                          formatWithCommas(event_hvl.eventTime),
                          formatWithCommas(startTime),
                          formatWithCommas(endTime));
                has_events_outside_range = true;
                break;
            }

            LogEvent event(event_hvl.storyId,
                           event_hvl.eventTime,
                           event_hvl.clientId,
                           event_hvl.eventIndex,
                           std::string(static_cast<char*>(event_hvl.logRecord.p), event_hvl.logRecord.len));
            if(story_chunk->insertEvent(event) == 0)
            {
                LOG_WARNING("[HDF5ArchiveReadingAgent] Failed to insert event with time {} into StoryChunk {}-{}",
                            formatWithCommas(event_hvl.eventTime),
                            chronicleName,
                            storyName);
            }
            else
            {
                LOG_DEBUG("[HDF5ArchiveReadingAgent] Inserted a new event with time {} into StoryChunk {}-{}, "
                          "the chunk has {} events now",
                          formatWithCommas(event_hvl.eventTime),
                          chronicleName,
                          storyName,
                          story_chunk->getEventCount());
            }
        }

        if(story_chunk->getEventCount() > 0)
        {
            listOfChunks.emplace_back(story_chunk);
            LOG_DEBUG("[HDF5ArchiveReadingAgent] Inserted a StoryChunk with {} events {}-{} range {}-{} into list",
                      formatWithCommas(story_chunk->getEventCount()),
                      chronicleName,
                      storyName,
                      formatWithCommas(startTime),
                      formatWithCommas(endTime));
        }
        else
        {
            LOG_DEBUG("[HDF5ArchiveReadingAgent] No events in {}-{} are in range {}-{}, "
                      "no StoryChunk added to list",
                      chronicleName,
                      storyName,
                      formatWithCommas(startTime),
                      formatWithCommas(endTime));
            delete story_chunk;
        }
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] reading file {} : FileIException: {} in C Function: {}",
                  file_name,
                  error.getCDetailMsg(),
                  error.getCFuncName());
        ErrorReport report;
        H5::Exception::walkErrorStack(H5E_WALK_UPWARD, error_walker, &report);
        for(const auto& msg: report.messages) { LOG_ERROR("[HDF5ArchiveReadingAgent] {}", msg); }

        delete story_chunk;
        return -1;
    }
    catch(H5::Exception& error)
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] reading file {} : Exception: {} in C Function: {}",
                  file_name,
                  error.getCDetailMsg(),
                  error.getCFuncName());
        ErrorReport report;
        H5::Exception::walkErrorStack(H5E_WALK_UPWARD, error_walker, &report);
        for(const auto& msg: report.messages) { LOG_ERROR("[HDF5ArchiveReadingAgent] {}", msg); }
        delete story_chunk;
        return -1;
    }
    if(has_events_outside_range)
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] Some events are outside the range {}-{}, returning 1 to indicate no more "
                  "files to read",
                  formatWithCommas(startTime),
                  formatWithCommas(endTime));
        return 1;
    }
    else
    {
        LOG_DEBUG("[HDF5ArchiveReadingAgent] All events are within the range {}-{}, returning 0",
                  formatWithCommas(startTime),
                  formatWithCommas(endTime));
        return 0;
    }
}

void chronolog::HDF5ArchiveReadingAgent::applyDeletion(ArchiveManifestRecord const& record, uint32_t log)
{
    // Takes the deletion's log off every file of one story, and drops the
    // story's entry once nothing is left in it. Returns the next story.
    // A deletion that carries a bound covers only the files of its writer's
    // destroyed story: files of the writer process that started at
    // writer_start with an incarnation up to the bound, and any of an earlier
    // process. A file that process wrote later belongs to the story created
    // again (see HDF5FileChunkExtractor's DestroyScope).
    auto const covers = [&record](RecordedFile const& file)
    {
        if(record.incarnation_bound == UINT64_MAX)
        {
            return true;
        }
        WindowFileName parsed;
        if(!parseWindowFileName(fs::path(file.path).filename().string(), parsed))
        {
            return true;
        }
        return parsed.writer_start != record.writer_start || parsed.incarnation <= record.incarnation_bound;
    };
    auto const release_story = [this, log, &covers](auto story_it)
    {
        auto& ranges = story_it->second.ranges;
        for(auto range_it = ranges.begin(); range_it != ranges.end();)
        {
            auto& files = range_it->second.files;
            // Every grapher runs a destroy and records it in its own log,
            // after that log's records of the files, so each log's deletion
            // lets go of its own files, and nothing needs looking up. The log
            // comes off first: a remove_if predicate may not change elements.
            for(RecordedFile& file: files)
            {
                if(covers(file))
                {
                    file.logs.erase(std::remove(file.logs.begin(), file.logs.end(), log), file.logs.end());
                }
            }
            files.erase(std::remove_if(files.begin(),
                                       files.end(),
                                       [](RecordedFile const& file) { return file.logs.empty(); }),
                        files.end());
            range_it = files.empty() ? ranges.erase(range_it) : std::next(range_it);
        }
        return ranges.empty() ? recorded_files_.erase(story_it) : std::next(story_it);
    };
    if(record.whole_chronicle)
    {
        // a chronicle's stories sort together, from its empty story name on
        for(auto story_it = recorded_files_.lower_bound(StoryKey(record.chronicle, StoryName()));
            story_it != recorded_files_.end() && story_it->first.first == record.chronicle;)
        {
            story_it = release_story(story_it);
        }
    }
    else
    {
        auto const story_it = recorded_files_.find(StoryKey(record.chronicle, record.story));
        if(story_it != recorded_files_.end())
        {
            release_story(story_it);
        }
    }
}

void chronolog::HDF5ArchiveReadingAgent::applyRecord(ArchiveManifestRecord const& record, uint32_t log)
{
    if(record.op == ArchiveManifestRecord::Op::DELETE)
    {
        applyDeletion(record, log);
        return;
    }
    std::string const path = (fs::path(archive_path_) / record.file).string();
    RecordedStory& story = recorded_files_[StoryKey(record.chronicle, record.story)];
    RecordedRange& range = story.ranges[record.start];
    range.end = std::max(range.end, record.end);
    if(record.end > record.start)
    {
        story.max_span = std::max(story.max_span, record.end - record.start);
    }
    for(RecordedFile& file: range.files)
    {
        if(file.path == path)
        {
            // the same path again, from a re-read log or a second log naming
            // it: remember every log that recorded it, so a deletion in one of
            // them leaves the others' record standing
            if(std::find(file.logs.begin(), file.logs.end(), log) == file.logs.end())
            {
                file.logs.push_back(log);
            }
            return;
        }
    }
    range.files.push_back(RecordedFile{path, {log}});
}

bool chronolog::HDF5ArchiveReadingAgent::refreshFromManifest(std::size_t* log_count)
{
    bool listing_failed = false;
    std::vector<std::string> const logs = listArchiveManifestLogs(archive_path_, &listing_failed);
    bool complete = !listing_failed;
    for(std::string const& log: logs)
    {
        auto const [id_it, added] = log_ids_.emplace(log, static_cast<uint32_t>(manifest_tails_.size()));
        if(added)
        {
            manifest_tails_.emplace_back(log);
        }
        uint32_t const id = id_it->second;
        ArchiveManifestTail& tail = manifest_tails_[id];
        // in pieces, so a long log is never held whole
        uint64_t offset_before = 0;
        do {
            offset_before = tail.offset();
            std::vector<ArchiveManifestRecord> records;
            if(tail.readNew(records, kManifestPieceBytes) != CL_SUCCESS)
            {
                LOG_WARNING("[HDF5ArchiveReadingAgent] Could not read the archive manifest log {}", log);
                complete = false;
                break;
            }
            for(ArchiveManifestRecord const& record: records) { applyRecord(record, id); }
        } while(tail.offset() != offset_before);
    }
    if(log_count != nullptr)
    {
        *log_count = logs.size();
    }
    return complete;
}

int chronolog::HDF5ArchiveReadingAgent::initialize()
{
    std::lock_guard<std::mutex> lock(index_mutex_);
    std::error_code ec;
    if(!fs::is_directory(archive_path_, ec))
    {
        LOG_ERROR("[HDF5ArchiveReadingAgent] The archive directory {} does not exist", archive_path_);
        return CL_ERR_UNKNOWN;
    }
    std::size_t log_count = 0;
    refreshFromManifest(&log_count);
    if(log_count == 0)
    {
        // the graphers write their logs into their hdf5_archive_dir: a player
        // whose story_files_dir differs finds none, ever
        LOG_WARNING("[HDF5ArchiveReadingAgent] No archive manifest in {} yet. If no grapher has started, that is "
                    "expected; otherwise check that chrono_player's story_files_dir names the same directory as "
                    "chrono_grapher's hdf5_archive_dir",
                    archive_path_);
    }
    return CL_SUCCESS;
}

int chronolog::HDF5ArchiveReadingAgent::readArchivedStory(const ChronicleName& chronicleName,
                                                          const StoryName& storyName,
                                                          uint64_t startTime,
                                                          uint64_t endTime,
                                                          std::list<StoryChunk*>& listOfChunks)
{
    std::vector<std::string> files_to_read;
    // a manifest that could not be read in full may hide files of the range
    bool index_complete = true;
    {
        std::lock_guard<std::mutex> lock(index_mutex_);
        std::error_code ec;
        if(!fs::is_directory(archive_path_, ec))
        {
            // the player cannot tell what is archived, so "nothing" would be a guess
            LOG_ERROR("[HDF5ArchiveReadingAgent] The archive directory {} does not exist", archive_path_);
            return CL_ERR_UNKNOWN;
        }
        index_complete = refreshFromManifest();

        auto const story_it = recorded_files_.find(StoryKey(chronicleName, storyName));
        if(story_it != recorded_files_.end())
        {
            // ranges can start before startTime and reach into it, but by at
            // most the story's longest range; each is checked against its end
            RecordedStory const& story = story_it->second;
            uint64_t const first_start = (startTime > story.max_span) ? startTime - story.max_span : 0;
            for(auto range_it = story.ranges.lower_bound(first_start); range_it != story.ranges.end(); ++range_it)
            {
                auto const& [start, range] = *range_it;
                if(start >= endTime)
                {
                    break;
                }
                if(range.end > startTime)
                {
                    for(RecordedFile const& file: range.files) { files_to_read.push_back(file.path); }
                }
            }
        }
    }

    LOG_DEBUG("[HDF5ArchiveReadingAgent] Reading {} archive file(s) for story {}-{} range {}-{}",
              files_to_read.size(),
              chronicleName,
              storyName,
              formatWithCommas(startTime),
              formatWithCommas(endTime));

    // a file that cannot be read leaves its events out of the replay, so the
    // range comes back as an error even though the readable files are returned
    int read_status = index_complete ? CL_SUCCESS : CL_ERR_UNKNOWN;
    for(std::string const& file: files_to_read)
    {
        std::error_code ec;
        if(!fs::exists(file, ec) && !ec)
        {
            // Deleted with its story, and the deletion's record is not in yet.
            // Kept in the index all the same: the deletion's record, which
            // can arrive after the files are gone, removes it.
            LOG_DEBUG("[HDF5ArchiveReadingAgent] {} no longer exists; skipping it", file);
            continue;
        }
        if(readStoryChunkFile(chronicleName, storyName, startTime, endTime, listOfChunks, file) < 0)
        {
            read_status = CL_ERR_UNKNOWN;
        }
    }
    return read_status;
}

} // namespace chronolog
