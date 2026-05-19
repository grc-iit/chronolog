#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <filesystem>
#include <future>
#include <limits>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <thread>
#include <sys/uio.h>
#include <unistd.h>

#include <StoryChunkWriter.h>
#include <chronolog_profile.h>

namespace fs = std::filesystem;

namespace chronolog
{
namespace
{
std::mutex archive_hdf5_mutex;

double elapsedUs(std::chrono::high_resolution_clock::time_point start,
                 std::chrono::high_resolution_clock::time_point end)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0;
}

bool envEnabled(char const* name)
{
    char const* value = std::getenv(name);
    return value != nullptr && (*value == '1' || std::strcmp(value, "true") == 0 || std::strcmp(value, "on") == 0 ||
                               std::strcmp(value, "yes") == 0);
}

std::size_t envSize(char const* name)
{
    char const* value = std::getenv(name);
    if(value == nullptr || *value == '\0')
    {
        return 0;
    }
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if(end == value)
    {
        return 0;
    }
    return static_cast<std::size_t>(parsed);
}

std::string envString(char const* name, std::string const& fallback)
{
    char const* value = std::getenv(name);
    if(value == nullptr || *value == '\0')
    {
        return fallback;
    }
    return value;
}

std::string jsonEscape(std::string const& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for(char c: value)
    {
        switch(c)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += c;
            break;
        }
    }
    return escaped;
}

struct RawPayloadCloseResult
{
    bool ok{false};
    double closeUs{0.0};
    std::string error;
};

struct RawPayloadWriteResult
{
    bool ok{false};
    StoryChunkWriter::WriteProfile profile;
};

struct ArchiveManifestData
{
    std::string chronicle;
    std::string story;
    uint64_t storyId{0};
    uint64_t startTime{0};
    uint64_t endTime{0};
    uint64_t firstEventTime{0};
    uint64_t lastEventTime{0};
    std::size_t eventCount{0};
    std::vector<uint64_t> eventTimes;
};

RawPayloadCloseResult closeRawPayloadFd(int fd, std::string file_name)
{
    auto const close_start = std::chrono::high_resolution_clock::now();
    int const close_result = ::close(fd);
    auto const close_end = std::chrono::high_resolution_clock::now();

    RawPayloadCloseResult result;
    result.ok = (close_result == 0);
    result.closeUs = elapsedUs(close_start, close_end);
    if(!result.ok)
    {
        result.error = std::strerror(errno);
        LOG_ERROR("[StoryChunkWriter] close failed for raw payload file {}: {}", file_name, result.error);
    }
    return result;
}

ArchiveManifestData buildArchiveManifestData(StoryChunk const& story_chunk)
{
    ArchiveManifestData data;
    data.chronicle = story_chunk.getChronicleName();
    data.story = story_chunk.getStoryName();
    data.storyId = story_chunk.getStoryId();
    data.startTime = story_chunk.getStartTime();
    data.endTime = story_chunk.getEndTime();
    data.firstEventTime = story_chunk.firstEventTime();
    data.lastEventTime = story_chunk.lastEventTime();
    data.eventCount = story_chunk.getEventCount();
    data.eventTimes.reserve(story_chunk.getEventCount());
    for(auto const& event_entry: story_chunk)
    {
        data.eventTimes.push_back(event_entry.second.time());
    }
    return data;
}

bool writeAllIovecs(std::string const& file_name,
                    std::vector<iovec> const& source_iovecs,
                    StoryChunkWriter::WriteProfile& profile,
                    std::future<RawPayloadCloseResult>* close_future = nullptr)
{
    auto const open_start = std::chrono::high_resolution_clock::now();
    int fd = ::open(file_name.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    auto const open_end = std::chrono::high_resolution_clock::now();
    profile.rawPayloadOpenUs += elapsedUs(open_start, open_end);
    if(fd < 0)
    {
        LOG_ERROR("[StoryChunkWriter] Failed to open raw payload file {}: {}", file_name, std::strerror(errno));
        return false;
    }
    if(envEnabled("CHRONOLOG_RAW_BLOB_PREALLOCATE"))
    {
        off_t payload_size = 0;
        for(auto const& source_iovec: source_iovecs)
        {
            if(source_iovec.iov_len > static_cast<std::size_t>(std::numeric_limits<off_t>::max() - payload_size))
            {
                profile.rawPayloadPreallocateResult = EOVERFLOW;
                break;
            }
            payload_size += static_cast<off_t>(source_iovec.iov_len);
        }
        if(profile.rawPayloadPreallocateResult == 0 && payload_size > 0)
        {
            auto const preallocate_start = std::chrono::high_resolution_clock::now();
            profile.rawPayloadPreallocateResult = ::posix_fallocate(fd, 0, payload_size);
            auto const preallocate_end = std::chrono::high_resolution_clock::now();
            profile.rawPayloadPreallocateUs += elapsedUs(preallocate_start, preallocate_end);
            if(profile.rawPayloadPreallocateResult != 0)
            {
                LOG_WARNING("[StoryChunkWriter] posix_fallocate failed for raw payload file {}: {}",
                            file_name,
                            std::strerror(profile.rawPayloadPreallocateResult));
            }
        }
    }

    long max_iov = ::sysconf(_SC_IOV_MAX);
    if(max_iov <= 0)
    {
        max_iov = 1024;
    }

    std::vector<iovec> pending;
    pending.reserve(std::min<std::size_t>(source_iovecs.size(), static_cast<std::size_t>(max_iov)));
    std::size_t index = 0;
    while(index < source_iovecs.size())
    {
        pending.clear();
        for(; index < source_iovecs.size() && pending.size() < static_cast<std::size_t>(max_iov); ++index)
        {
            if(source_iovecs[index].iov_len > 0)
            {
                pending.push_back(source_iovecs[index]);
            }
        }

        if(!pending.empty())
        {
            std::size_t first_pending = 0;
            while(first_pending < pending.size())
            {
                auto const writev_start = std::chrono::high_resolution_clock::now();
                ssize_t written = ::writev(fd,
                                           pending.data() + first_pending,
                                           static_cast<int>(pending.size() - first_pending));
                auto const writev_end = std::chrono::high_resolution_clock::now();
                profile.rawPayloadWritevUs += elapsedUs(writev_start, writev_end);
                if(written < 0)
                {
                    if(errno == EINTR)
                    {
                        continue;
                    }
                    LOG_ERROR("[StoryChunkWriter] writev failed for raw payload file {}: {}",
                              file_name,
                              std::strerror(errno));
                    RawPayloadCloseResult close_result = closeRawPayloadFd(fd, file_name);
                    profile.rawPayloadCloseUs += close_result.closeUs;
                    return false;
                }
                if(written == 0)
                {
                    LOG_ERROR("[StoryChunkWriter] writev made no progress for raw payload file {}", file_name);
                    RawPayloadCloseResult close_result = closeRawPayloadFd(fd, file_name);
                    profile.rawPayloadCloseUs += close_result.closeUs;
                    return false;
                }

                std::size_t consumed = static_cast<std::size_t>(written);
                profile.rawPayloadWritevCalls += 1;
                profile.rawPayloadBytes += consumed;
                while(first_pending < pending.size() && consumed >= pending[first_pending].iov_len)
                {
                    consumed -= pending[first_pending].iov_len;
                    ++first_pending;
                }
                if(first_pending < pending.size() && consumed > 0)
                {
                    pending[first_pending].iov_base = static_cast<char*>(pending[first_pending].iov_base) + consumed;
                    pending[first_pending].iov_len -= consumed;
                    profile.rawPayloadPartialWrites += 1;
                }
            }
        }
    }

    if(close_future != nullptr)
    {
        profile.rawPayloadAsyncClose = true;
        *close_future = std::async(std::launch::async, closeRawPayloadFd, fd, file_name);
        return true;
    }

    RawPayloadCloseResult close_result = closeRawPayloadFd(fd, file_name);
    profile.rawPayloadCloseUs += close_result.closeUs;
    return close_result.ok;
}

RawPayloadWriteResult writeAllIovecsWithProfile(std::string const& file_name,
                                                std::vector<iovec> const& source_iovecs)
{
    RawPayloadWriteResult result;
    result.ok = writeAllIovecs(file_name, source_iovecs, result.profile);
    return result;
}

void mergeRawPayloadProfile(StoryChunkWriter::WriteProfile& target,
                            StoryChunkWriter::WriteProfile const& source)
{
    target.rawPayloadOpenUs += source.rawPayloadOpenUs;
    target.rawPayloadPreallocateUs += source.rawPayloadPreallocateUs;
    target.rawPayloadWritevUs += source.rawPayloadWritevUs;
    target.rawPayloadCloseUs += source.rawPayloadCloseUs;
    target.rawPayloadBytes += source.rawPayloadBytes;
    target.rawPayloadWritevCalls += source.rawPayloadWritevCalls;
    target.rawPayloadPartialWrites += source.rawPayloadPartialWrites;
    target.rawPayloadPreallocateResult = source.rawPayloadPreallocateResult;
}

bool reserveArchiveFileName(std::string const& file_name, bool log_errors = true)
{
    int fd = ::open(file_name.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    if(fd < 0)
    {
        if(log_errors)
        {
            LOG_ERROR("[StoryChunkWriter] Failed to reserve archive file {}: {}", file_name, std::strerror(errno));
        }
        return false;
    }
    if(::close(fd) != 0)
    {
        LOG_ERROR("[StoryChunkWriter] Failed to close reserved archive file {}: {}", file_name, std::strerror(errno));
        return false;
    }
    return true;
}

bool tryFastRawBlobReservation(std::string const& root_dir,
                               std::string const& base_file_name,
                               std::string& final_file_name,
                               std::string& write_file_name,
                               std::string& final_payload_file_name,
                               std::string& write_payload_file_name)
{
    fs::path final_path = fs::path(root_dir) / fs::path(base_file_name);
    final_file_name = final_path.make_preferred().string();
    write_file_name = final_file_name + ".tmp";
    final_payload_file_name = final_file_name + ".payload";
    write_payload_file_name = final_payload_file_name + ".tmp";

    std::error_code ec;
    if(fs::exists(final_file_name, ec) || fs::exists(write_file_name, ec) ||
       fs::exists(final_payload_file_name, ec) || fs::exists(write_payload_file_name, ec))
    {
        return false;
    }
    return reserveArchiveFileName(write_file_name, false);
}

bool writeArchiveManifestData(std::string const& archive_file_name,
                              ArchiveManifestData const& manifest_data,
                              std::size_t payload_bytes,
                              double* manifest_write_us = nullptr)
{
    auto const manifest_start = std::chrono::high_resolution_clock::now();
    auto record_manifest_elapsed = [&]() {
        if(manifest_write_us != nullptr)
        {
            *manifest_write_us += elapsedUs(manifest_start, std::chrono::high_resolution_clock::now());
        }
    };
    std::string const manifest_file_name = archive_file_name + ".manifest";
    std::string const temp_manifest_file_name = manifest_file_name + ".tmp";
    {
        std::ofstream manifest(temp_manifest_file_name, std::ios::out | std::ios::trunc);
        if(!manifest)
        {
            LOG_ERROR("[StoryChunkWriter] Failed to open archive manifest {}", temp_manifest_file_name);
            record_manifest_elapsed();
            return false;
        }
        manifest << "{\n"
                 << "  \"chronicle\": \"" << jsonEscape(manifest_data.chronicle) << "\",\n"
                 << "  \"story\": \"" << jsonEscape(manifest_data.story) << "\",\n"
                 << "  \"story_id\": " << manifest_data.storyId << ",\n"
                 << "  \"start_time\": " << manifest_data.startTime << ",\n"
                 << "  \"end_time\": " << manifest_data.endTime << ",\n"
                 << "  \"first_event_time\": " << manifest_data.firstEventTime << ",\n"
                 << "  \"last_event_time\": " << manifest_data.lastEventTime << ",\n"
                 << "  \"event_count\": " << manifest_data.eventCount << ",\n"
                 << "  \"payload_bytes\": " << payload_bytes << ",\n"
                 << "  \"event_times\": [";
        bool first = true;
        for(auto const& event_time: manifest_data.eventTimes)
        {
            if(!first)
            {
                manifest << ", ";
            }
            first = false;
            manifest << event_time;
        }
        manifest << "]\n"
                 << "}\n";
        if(!manifest)
        {
            LOG_ERROR("[StoryChunkWriter] Failed to write archive manifest {}", temp_manifest_file_name);
            record_manifest_elapsed();
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp_manifest_file_name, manifest_file_name, ec);
    if(ec)
    {
        LOG_ERROR("[StoryChunkWriter] Failed to publish archive manifest {}: {}",
                  manifest_file_name,
                  ec.message());
        record_manifest_elapsed();
        return false;
    }
    record_manifest_elapsed();
    return true;
}

bool writeArchiveManifest(std::string const& archive_file_name,
                          StoryChunk const& story_chunk,
                          std::size_t payload_bytes,
                          double* manifest_write_us = nullptr)
{
    return writeArchiveManifestData(archive_file_name,
                                    buildArchiveManifestData(story_chunk),
                                    payload_bytes,
                                    manifest_write_us);
}

bool writeRawBlobMetaSidecar(std::string const& file_name, std::vector<StoryChunkWriter::BlobMapEntry> const& meta)
{
    std::ofstream output(file_name, std::ios::binary | std::ios::trunc);
    if(!output)
    {
        LOG_ERROR("[StoryChunkWriter] Failed to open raw_blob metadata sidecar {}", file_name);
        return false;
    }
    std::uint64_t const entry_count = static_cast<std::uint64_t>(meta.size());
    output.write(reinterpret_cast<char const*>(&entry_count), sizeof(entry_count));
    if(!meta.empty())
    {
        output.write(reinterpret_cast<char const*>(meta.data()),
                     static_cast<std::streamsize>(meta.size() * sizeof(meta.front())));
    }
    if(!output)
    {
        LOG_ERROR("[StoryChunkWriter] Failed to write raw_blob metadata sidecar {}", file_name);
        return false;
    }
    return true;
}

struct AsyncArchivePublishTask
{
    uint64_t storyId{0};
    uint64_t startTime{0};
    std::string writeFileName;
    std::string finalFileName;
    std::string writePayloadFileName;
    std::string finalPayloadFileName;
    std::string writeMetaFileName;
    std::string finalMetaFileName;
    bool hasSidecarMeta{false};
    bool publishBeforePayloadClose{false};
    std::size_t payloadBytes{0};
    ArchiveManifestData manifestData;
    std::future<RawPayloadCloseResult> closeFuture;
};

class AsyncArchivePublisher
{
public:
    ~AsyncArchivePublisher()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        for(auto& worker: workers)
        {
            if(worker.joinable())
            {
                worker.join();
            }
        }
    }

    void enqueue(AsyncArchivePublishTask&& task, std::size_t requested_worker_count)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            tasks.push_back(std::move(task));
            std::size_t const worker_count = std::max<std::size_t>(1, requested_worker_count);
            while(workers.size() < worker_count)
            {
                workers.emplace_back(&AsyncArchivePublisher::run, this, workers.size());
            }
        }
        changed.notify_one();
    }

private:
    void run(std::size_t worker_index)
    {
        while(true)
        {
            AsyncArchivePublishTask task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&]() { return stopping || !tasks.empty(); });
                if(tasks.empty())
                {
                    if(stopping)
                    {
                        return;
                    }
                    continue;
                }
                task = std::move(tasks.front());
                tasks.pop_front();
            }

            auto const total_start = std::chrono::high_resolution_clock::now();
            RawPayloadCloseResult close_result{true, 0.0, ""};
            double close_wait_us = 0.0;

            auto wait_for_payload_close = [&]() {
                auto const close_wait_start = std::chrono::high_resolution_clock::now();
                close_result = task.closeFuture.get();
                auto const close_wait_end = std::chrono::high_resolution_clock::now();
                close_wait_us = elapsedUs(close_wait_start, close_wait_end);
                return close_result.ok;
            };

            if(!task.publishBeforePayloadClose && !wait_for_payload_close())
            {
                std::error_code remove_ec;
                std::filesystem::remove(task.writeFileName, remove_ec);
                std::filesystem::remove(task.writePayloadFileName, remove_ec);
                std::filesystem::remove(task.writeMetaFileName, remove_ec);
                LOG_ERROR("[StoryChunkWriter] Async archive publish failed during payload close StoryID={} StartTime={} "
                          "close_wait_us={} close_us={}",
                          task.storyId,
                          task.startTime,
                          close_wait_us,
                          close_result.closeUs);
                continue;
            }

            double publish_rename_us = 0.0;
            double archive_manifest_write_us = 0.0;
            bool ok = true;
            try
            {
                auto const rename_start = std::chrono::high_resolution_clock::now();
                std::filesystem::rename(task.writePayloadFileName, task.finalPayloadFileName);
                if(task.hasSidecarMeta)
                {
                    std::filesystem::rename(task.writeMetaFileName, task.finalMetaFileName);
                }
                std::filesystem::rename(task.writeFileName, task.finalFileName);
                auto const rename_end = std::chrono::high_resolution_clock::now();
                publish_rename_us = elapsedUs(rename_start, rename_end);
                ok = writeArchiveManifestData(task.finalFileName,
                                              task.manifestData,
                                              task.payloadBytes,
                                              &archive_manifest_write_us);
            }
            catch(std::exception const& error)
            {
                ok = false;
                LOG_ERROR("[StoryChunkWriter] Async archive publish exception StoryID={} StartTime={}: {}",
                          task.storyId,
                          task.startTime,
                          error.what());
            }

            if(task.publishBeforePayloadClose && !wait_for_payload_close())
            {
                ok = false;
                LOG_ERROR("[StoryChunkWriter] Async archive publish delayed payload close failed StoryID={} StartTime={} "
                          "close_wait_us={} close_us={}",
                          task.storyId,
                          task.startTime,
                          close_wait_us,
                          close_result.closeUs);
            }

            double const total_us = elapsedUs(total_start, std::chrono::high_resolution_clock::now());
            LOG_INFO("[StoryChunkWriter] Async archive publish completed StoryID={} StartTime={} worker={} ok={} "
                     "publish_before_payload_close={} close_wait_us={} close_us={} publish_rename_us={} "
                     "archive_manifest_write_us={} total_us={}",
                     task.storyId,
                     task.startTime,
                     worker_index,
                     ok ? 1 : 0,
                     task.publishBeforePayloadClose ? 1 : 0,
                     close_wait_us,
                     close_result.closeUs,
                     publish_rename_us,
                     archive_manifest_write_us,
                     total_us);
        }
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<AsyncArchivePublishTask> tasks;
    std::vector<std::thread> workers;
    bool stopping{false};
};

AsyncArchivePublisher& asyncArchivePublisher()
{
    static AsyncArchivePublisher publisher;
    return publisher;
}
}

hsize_t StoryChunkWriter::writeStoryChunk(StoryChunkHVL& story_chunk)
{
    CL_PROFILE_REGION("storage_write");
    CL_PROFILE_COUNTER("append_bytes", story_chunk.getEventCount());
    CL_PROFILE_COUNTER("storage_write_events", story_chunk.getEventCount());

    std::vector<LogEventHVL> data;
    data.reserve(story_chunk.getEventCount());
    for(const auto& start: story_chunk) { data.push_back(start.second); }
    std::string file_name = rootDirectory + story_chunk.getChronicleName() + "." + story_chunk.getStoryName() + "." +
                            std::to_string(story_chunk.getStartTime() / 1000000000) + ".vlen.h5";
    hsize_t ret = 0;
    std::unique_ptr<H5::H5File> file;
    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating StoryChunk file: {}", file_name);
        {
            CL_PROFILE_REGION("grapher_hdf5_open");
            file = std::make_unique<H5::H5File>(file_name, H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE);
        }

        LOG_DEBUG("[StoryChunkWriter] Writing StoryChunk to file...");
        ret = writeEvents(file, data);
        if(ret == 0)
        {
            LOG_ERROR("[StoryChunkWriter] Error writing StoryChunk to file.");
            return ret;
        }

        {
            CL_PROFILE_REGION("grapher_hdf5_flush");
            file->flush(H5F_SCOPE_GLOBAL);
        }
        hsize_t file_size = file->getFileSize();

        LOG_DEBUG("[StoryChunkWriter] Finished writing StoryChunk to file.");
        return file_size;
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    return ret;
}

std::string StoryChunkWriter::getStoryChunkFileName(std::string const& root_dir, std::string const& base_file_name)
{
    const std::string base_file_name_no_ext = fs::path(base_file_name).stem().make_preferred().string();
    const std::string escaped_base_file_name_no_ext =
            std::regex_replace(base_file_name_no_ext, std::regex(R"([.^$|()\\*+?{}\[\]])"), R"(\$&)");
    const std::string rotated_prefix = escaped_base_file_name_no_ext + "\\.";
    const std::string ext = fs::path(base_file_name).extension().string();

    const std::regex rotated_pattern("^" + rotated_prefix + "([0-9]+)" + ext + "$");

    long long max_n = 0; // Means no numbered files have been found yet.
    bool base_exists = false;

    std::error_code ec;
    for(const auto& entry: fs::directory_iterator(root_dir, ec))
    {
        if(!entry.is_regular_file(ec))
        {
            continue; // Skip directories, symlinks, etc.
        }

        const std::string filename = entry.path().filename().string();

        std::string candidate_filename = filename;
        if(candidate_filename.size() > 4 &&
           candidate_filename.compare(candidate_filename.size() - 4, 4, ".tmp") == 0)
        {
            candidate_filename.resize(candidate_filename.size() - 4);
        }

        // Check for the base file, including an in-progress atomic-rename reservation.
        if(candidate_filename == base_file_name)
        {
            base_exists = true;
            continue;
        }

        // Check for rotated files
        std::smatch match;
        if(std::regex_match(candidate_filename, match, rotated_pattern))
        {
            // match[0] is the entire string, match[1] is the first capture group.
            if(match.size() == 2)
            {
                try
                {
                    long long current_n = std::stoll(match[1].str());
                    max_n = std::max(max_n, current_n);
                }
                catch(const std::out_of_range&)
                {
                    LOG_ERROR("[StoryChunkWriter] Number in file name '{}' is out of range.", match[1].str());
                }
            }
        }
    }

    fs::path next_filename_no_ext;
    if(!base_exists)
    {
        next_filename_no_ext = base_file_name_no_ext;
    }
    else
    {
        // We found numbered files, so the next is max_n + 1.
        next_filename_no_ext = base_file_name_no_ext + "." + std::to_string(max_n + 1);
    }

    next_filename_no_ext += ext;
    LOG_DEBUG("[StoryChunkWriter] Next unique file name: {}", next_filename_no_ext.string());
    return (root_dir / next_filename_no_ext).make_preferred();
}

hsize_t StoryChunkWriter::writeStoryChunk(StoryChunk& story_chunk)
{
    CL_PROFILE_REGION("storage_write");
    CL_PROFILE_COUNTER("append_bytes", story_chunk.getEventCount());
    CL_PROFILE_COUNTER("storage_write_events", story_chunk.getEventCount());
    lastProfile = WriteProfile{};
    lastProfile.atomicRename = envEnabled("CHRONOLOG_HDF5_ARCHIVE_ATOMIC_RENAME");
    lastProfile.chunkEvents = envSize("CHRONOLOG_HDF5_ARCHIVE_CHUNK_EVENTS");
    lastProfile.archiveLayout = envString("CHRONOLOG_HDF5_ARCHIVE_LAYOUT", "vlen");
    lastProfile.rawPayloadAsyncWrite = envEnabled("CHRONOLOG_RAW_BLOB_ASYNC_WRITE");
    lastProfile.rawPayloadAsyncClose = envEnabled("CHRONOLOG_RAW_BLOB_ASYNC_CLOSE");
    lastProfile.rawPayloadAsyncPublish = envEnabled("CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH");
    bool const fastRawBlobReserve = envEnabled("CHRONOLOG_RAW_BLOB_FAST_RESERVE");
    bool const rawBlobSidecarMeta = envEnabled("CHRONOLOG_RAW_BLOB_SIDECAR_META");
    bool const rawBlobPublishBeforePayloadClose = envEnabled("CHRONOLOG_RAW_BLOB_PUBLISH_BEFORE_CLOSE");
    lastProfile.rawPayloadAsyncPublishThreads =
            std::max<std::size_t>(1, envSize("CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH_THREADS"));
    if(lastProfile.rawPayloadAsyncPublish && envEnabled("CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN"))
    {
        LOG_WARNING("[StoryChunkWriter] CHRONOLOG_RAW_BLOB_ASYNC_PUBLISH is disabled because "
                    "CHRONOLOG_GRAPHER_STOP_STORY_ARCHIVE_DRAIN is enabled.");
        lastProfile.rawPayloadAsyncPublish = false;
    }
    if(lastProfile.archiveLayout != "vlen" && lastProfile.archiveLayout != "blob_map" &&
       lastProfile.archiveLayout != "raw_blob" &&
       lastProfile.archiveLayout != "fixed_record")
    {
        LOG_WARNING("[StoryChunkWriter] Unsupported CHRONOLOG_HDF5_ARCHIVE_LAYOUT='{}'; falling back to vlen",
                    lastProfile.archiveLayout);
        lastProfile.archiveLayout = "vlen";
    }
    auto const total_start = std::chrono::high_resolution_clock::now();

    std::vector<LogEventHVLView> data;
    std::vector<unsigned char> blob_payload;
    std::vector<BlobMapEntry> blob_meta;
    std::vector<iovec> raw_blob_iovecs;
    std::vector<BlobMapEntry> raw_blob_meta;
    std::size_t raw_blob_payload_size = 0;
    std::unique_ptr<unsigned char[]> fixed_record_payload;
    std::vector<FixedRecordMetaEntry> fixed_record_meta;
    std::size_t fixed_record_size = 0;
    auto const prep_start = std::chrono::high_resolution_clock::now();
    if(lastProfile.archiveLayout == "blob_map")
    {
        std::size_t payload_size = 0;
        auto const prep_scan_start = std::chrono::high_resolution_clock::now();
        for(const auto& event: story_chunk) { payload_size += event.second.logRecord.size(); }
        auto const prep_scan_end = std::chrono::high_resolution_clock::now();
        lastProfile.prepScanUs = elapsedUs(prep_scan_start, prep_scan_end);
        blob_payload.reserve(payload_size);
        blob_meta.reserve(story_chunk.getEventCount());
        auto const prep_build_start = std::chrono::high_resolution_clock::now();
        double payload_copy_us = 0.0;
        for(const auto& event: story_chunk)
        {
            auto const& record = event.second.logRecord;
            BlobMapEntry entry{};
            entry.storyId = event.second.getStoryId();
            entry.eventTime = event.second.time();
            entry.clientId = event.second.getClientId();
            entry.eventIndex = event.second.index();
            entry.offset = blob_payload.size();
            entry.size = record.size();
            blob_meta.push_back(entry);
            auto const payload_copy_start = std::chrono::high_resolution_clock::now();
            blob_payload.insert(blob_payload.end(), record.begin(), record.end());
            auto const payload_copy_end = std::chrono::high_resolution_clock::now();
            payload_copy_us += elapsedUs(payload_copy_start, payload_copy_end);
        }
        auto const prep_build_end = std::chrono::high_resolution_clock::now();
        lastProfile.prepBuildUs = elapsedUs(prep_build_start, prep_build_end);
        lastProfile.prepPayloadCopyUs = payload_copy_us;
    }
    else if(lastProfile.archiveLayout == "raw_blob")
    {
        raw_blob_iovecs.reserve(story_chunk.getEventCount());
        raw_blob_meta.reserve(story_chunk.getEventCount());
        auto const prep_build_start = std::chrono::high_resolution_clock::now();
        for(const auto& event: story_chunk)
        {
            auto const& record = event.second.logRecord;
            BlobMapEntry entry{};
            entry.storyId = event.second.getStoryId();
            entry.eventTime = event.second.time();
            entry.clientId = event.second.getClientId();
            entry.eventIndex = event.second.index();
            entry.offset = raw_blob_payload_size;
            entry.size = record.size();
            raw_blob_meta.push_back(entry);
            raw_blob_payload_size += record.size();
            raw_blob_iovecs.push_back(iovec{const_cast<char*>(record.data()), record.size()});
        }
        auto const prep_build_end = std::chrono::high_resolution_clock::now();
        lastProfile.prepBuildUs = elapsedUs(prep_build_start, prep_build_end);
    }
    else if(lastProfile.archiveLayout == "fixed_record")
    {
        auto const prep_scan_start = std::chrono::high_resolution_clock::now();
        for(const auto& event: story_chunk)
        {
            std::size_t const record_size = event.second.logRecord.size();
            fixed_record_size = std::max(fixed_record_size, record_size);
        }
        auto const prep_scan_end = std::chrono::high_resolution_clock::now();
        lastProfile.prepScanUs = elapsedUs(prep_scan_start, prep_scan_end);
        fixed_record_meta.reserve(story_chunk.getEventCount());
        std::size_t const fixed_payload_size = story_chunk.getEventCount() * fixed_record_size;
        if(fixed_payload_size > 0)
        {
            fixed_record_payload.reset(new unsigned char[fixed_payload_size]);
        }
        auto const prep_build_start = std::chrono::high_resolution_clock::now();
        double payload_copy_us = 0.0;
        double payload_padding_zero_us = 0.0;
        std::size_t record_index = 0;
        for(const auto& event: story_chunk)
        {
            std::size_t const record_size = event.second.logRecord.size();
            fixed_record_meta.push_back(FixedRecordMetaEntry{event.second.getStoryId(),
                                                             event.second.time(),
                                                             event.second.getClientId(),
                                                             event.second.index(),
                                                             record_size});
            if(fixed_record_size > 0)
            {
                auto const payload_copy_start = std::chrono::high_resolution_clock::now();
                unsigned char* record_destination = fixed_record_payload.get() + (record_index * fixed_record_size);
                if(record_size > 0)
                {
                    std::memcpy(record_destination,
                                event.second.logRecord.data(),
                                record_size);
                }
                auto const payload_copy_end = std::chrono::high_resolution_clock::now();
                payload_copy_us += elapsedUs(payload_copy_start, payload_copy_end);
                if(record_size < fixed_record_size)
                {
                    auto const padding_zero_start = std::chrono::high_resolution_clock::now();
                    std::memset(record_destination + record_size, 0, fixed_record_size - record_size);
                    auto const padding_zero_end = std::chrono::high_resolution_clock::now();
                    payload_padding_zero_us += elapsedUs(padding_zero_start, padding_zero_end);
                }
            }
            ++record_index;
        }
        auto const prep_build_end = std::chrono::high_resolution_clock::now();
        lastProfile.prepBuildUs = elapsedUs(prep_build_start, prep_build_end);
        lastProfile.prepPayloadCopyUs = payload_copy_us + payload_padding_zero_us;
    }
    if(lastProfile.archiveLayout == "vlen")
    {
        data.reserve(story_chunk.getEventCount());
        auto const prep_build_start = std::chrono::high_resolution_clock::now();
        for(const auto& event: story_chunk)
        {
            hvl_t log_record;
            log_record.len = event.second.logRecord.size();
            log_record.p = (void*)event.second.logRecord.data();
            data.push_back(LogEventHVLView{event.second.getStoryId(),
                                           event.second.time(),
                                           static_cast<uint32_t>(event.second.getClientId()),
                                           event.second.index(),
                                           log_record});
        }
        auto const prep_build_end = std::chrono::high_resolution_clock::now();
        lastProfile.prepBuildUs = elapsedUs(prep_build_start, prep_build_end);
    }
    auto const prep_end = std::chrono::high_resolution_clock::now();
    lastProfile.prepUs = elapsedUs(prep_start, prep_end);
    std::string file_name = story_chunk.getChronicleName() + "." + story_chunk.getStoryName() + "." +
                            std::to_string(story_chunk.getStartTime() / 1000000000) + "." +
                            lastProfile.archiveLayout + ".h5";
    //    file_name = fs::path(rootDirectory) / fs::path(file_name);
    hsize_t ret = 0;
    std::unique_ptr<H5::H5File> file;
    try
    {
        std::string final_file_name;
        std::string write_file_name;
        std::string final_payload_file_name;
        std::string write_payload_file_name;
        std::string final_meta_file_name;
        std::string write_meta_file_name;
        std::future<RawPayloadWriteResult> raw_payload_write_future;
        std::future<RawPayloadCloseResult> raw_payload_close_future;
        bool raw_payload_write_pending = false;
        bool raw_payload_close_pending = false;
        bool raw_blob_deferred_publish = false;
        bool raw_blob_sidecar_meta_written = false;
        auto finish_raw_payload_write = [&]() -> bool {
            if(!raw_payload_write_pending)
            {
                return true;
            }
            auto const write_wait_start = std::chrono::high_resolution_clock::now();
            RawPayloadWriteResult write_result = raw_payload_write_future.get();
            auto const write_wait_end = std::chrono::high_resolution_clock::now();
            raw_payload_write_pending = false;
            lastProfile.rawPayloadWriteWaitUs += elapsedUs(write_wait_start, write_wait_end);
            mergeRawPayloadProfile(lastProfile, write_result.profile);
            return write_result.ok;
        };
        auto finish_raw_payload_close = [&]() -> bool {
            if(!raw_payload_close_pending)
            {
                return true;
            }
            auto const close_wait_start = std::chrono::high_resolution_clock::now();
            RawPayloadCloseResult close_result = raw_payload_close_future.get();
            auto const close_wait_end = std::chrono::high_resolution_clock::now();
            raw_payload_close_pending = false;
            lastProfile.rawPayloadCloseWaitUs += elapsedUs(close_wait_start, close_wait_end);
            lastProfile.rawPayloadCloseUs += close_result.closeUs;
            return close_result.ok;
        };
        if(lastProfile.archiveLayout == "raw_blob" && lastProfile.atomicRename)
        {
            auto const lock_wait_start = std::chrono::high_resolution_clock::now();
            archive_hdf5_mutex.lock();
            auto const lock_acquired = std::chrono::high_resolution_clock::now();
            lastProfile.hdf5LockWaitUs += elapsedUs(lock_wait_start, lock_acquired);
            try
            {
                LOG_DEBUG("[StoryChunkWriter] Making sure the StoryChunk file name is unique...");
                auto const filename_start = std::chrono::high_resolution_clock::now();
                bool reserved_fast_path = false;
                if(fastRawBlobReserve)
                {
                    reserved_fast_path = tryFastRawBlobReservation(rootDirectory,
                                                                   file_name,
                                                                   final_file_name,
                                                                   write_file_name,
                                                                   final_payload_file_name,
                                                                   write_payload_file_name);
                }
                if(!reserved_fast_path)
                {
                    file_name = getStoryChunkFileName(rootDirectory, file_name);
                    final_file_name = file_name;
                    write_file_name = lastProfile.atomicRename ? final_file_name + ".tmp" : final_file_name;
                    final_payload_file_name = final_file_name + ".payload";
                    write_payload_file_name = lastProfile.atomicRename ? final_payload_file_name + ".tmp"
                                                                       : final_payload_file_name;
                    if(!reserveArchiveFileName(write_file_name))
                    {
                        archive_hdf5_mutex.unlock();
                        return 0;
                    }
                }
                final_meta_file_name = final_file_name + ".rawmeta";
                write_meta_file_name = lastProfile.atomicRename ? final_meta_file_name + ".tmp"
                                                                : final_meta_file_name;
                auto const filename_end = std::chrono::high_resolution_clock::now();
                lastProfile.filenameScanUs = elapsedUs(filename_start, filename_end);
            }
            catch(...)
            {
                archive_hdf5_mutex.unlock();
                throw;
            }
            archive_hdf5_mutex.unlock();

            auto const raw_write_start = std::chrono::high_resolution_clock::now();
            if(lastProfile.rawPayloadAsyncWrite)
            {
                raw_payload_write_future = std::async(std::launch::async,
                                                      writeAllIovecsWithProfile,
                                                      write_payload_file_name,
                                                      raw_blob_iovecs);
                raw_payload_write_pending = true;
                raw_blob_deferred_publish = true;
            }
            else
            {
                std::future<RawPayloadCloseResult>* close_future =
                        lastProfile.rawPayloadAsyncClose ? &raw_payload_close_future : nullptr;
                if(!writeAllIovecs(write_payload_file_name, raw_blob_iovecs, lastProfile, close_future))
                {
                    if(lastProfile.atomicRename)
                    {
                        std::error_code remove_ec;
                        std::filesystem::remove(write_file_name, remove_ec);
                        std::filesystem::remove(write_payload_file_name, remove_ec);
                        std::filesystem::remove(write_meta_file_name, remove_ec);
                    }
                    return 0;
                }
                raw_payload_close_pending = lastProfile.rawPayloadAsyncClose;
                raw_blob_deferred_publish = raw_payload_close_pending;
            }
            auto const raw_write_end = std::chrono::high_resolution_clock::now();
            lastProfile.datasetWriteCallUs += elapsedUs(raw_write_start, raw_write_end);
            lastProfile.datasetPayloadWriteCallUs += elapsedUs(raw_write_start, raw_write_end);
            if(rawBlobSidecarMeta)
            {
                auto const meta_write_start = std::chrono::high_resolution_clock::now();
                if(!writeRawBlobMetaSidecar(write_meta_file_name, raw_blob_meta))
                {
                    std::error_code remove_ec;
                    std::filesystem::remove(write_file_name, remove_ec);
                    std::filesystem::remove(write_payload_file_name, remove_ec);
                    std::filesystem::remove(write_meta_file_name, remove_ec);
                    return 0;
                }
                auto const meta_write_end = std::chrono::high_resolution_clock::now();
                double const meta_write_us = elapsedUs(meta_write_start, meta_write_end);
                lastProfile.datasetWriteCallUs += meta_write_us;
                lastProfile.datasetMetaWriteCallUs += meta_write_us;
                raw_blob_sidecar_meta_written = true;
            }
        }

        {
            auto const lock_wait_start = std::chrono::high_resolution_clock::now();
            std::lock_guard<std::mutex> lock(archive_hdf5_mutex);
            auto const lock_acquired = std::chrono::high_resolution_clock::now();
            lastProfile.hdf5LockWaitUs += elapsedUs(lock_wait_start, lock_acquired);

            if(!(lastProfile.archiveLayout == "raw_blob" && lastProfile.atomicRename))
            {
                LOG_DEBUG("[StoryChunkWriter] Making sure the StoryChunk file name is unique...");
                auto const filename_start = std::chrono::high_resolution_clock::now();
                file_name = getStoryChunkFileName(rootDirectory, file_name);
                auto const filename_end = std::chrono::high_resolution_clock::now();
                lastProfile.filenameScanUs = elapsedUs(filename_start, filename_end);

                final_file_name = file_name;
                write_file_name = lastProfile.atomicRename ? final_file_name + ".tmp" : final_file_name;
                final_payload_file_name = final_file_name + ".payload";
                write_payload_file_name =
                        lastProfile.atomicRename ? final_payload_file_name + ".tmp" : final_payload_file_name;
                final_meta_file_name = final_file_name + ".rawmeta";
                write_meta_file_name = lastProfile.atomicRename ? final_meta_file_name + ".tmp"
                                                                : final_meta_file_name;
            }

            LOG_DEBUG("[StoryChunkWriter] Creating StoryChunk file: {}", write_file_name);
            {
                CL_PROFILE_REGION("grapher_hdf5_open");
                auto const open_start = std::chrono::high_resolution_clock::now();
                unsigned int const hdf5_flags =
                        lastProfile.atomicRename ? H5F_ACC_TRUNC : (H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE);
                file = std::make_unique<H5::H5File>(write_file_name, hdf5_flags);
                auto const open_end = std::chrono::high_resolution_clock::now();
                lastProfile.openUs = elapsedUs(open_start, open_end);
            }

            LOG_DEBUG("[StoryChunkWriter] Writing StoryChunk to file...");
            auto const dataset_start = std::chrono::high_resolution_clock::now();
            if(lastProfile.archiveLayout == "blob_map")
            {
                ret = writeBlobMap(file, blob_payload, blob_meta);
            }
            else if(lastProfile.archiveLayout == "raw_blob")
            {
                if(!lastProfile.atomicRename)
                {
                    auto const raw_write_start = std::chrono::high_resolution_clock::now();
                    if(!writeAllIovecs(write_payload_file_name, raw_blob_iovecs, lastProfile))
                    {
                        return 0;
                    }
                    auto const raw_write_end = std::chrono::high_resolution_clock::now();
                    lastProfile.datasetWriteCallUs += elapsedUs(raw_write_start, raw_write_end);
                    lastProfile.datasetPayloadWriteCallUs += elapsedUs(raw_write_start, raw_write_end);
                }
                if(rawBlobSidecarMeta)
                {
                    if(!raw_blob_sidecar_meta_written && !writeRawBlobMetaSidecar(write_meta_file_name, raw_blob_meta))
                    {
                        return 0;
                    }
                    ret = raw_blob_meta.size();
                }
                else
                {
                    ret = writeRawBlobMap(file, raw_blob_meta);
                }
            }
            else if(lastProfile.archiveLayout == "fixed_record")
            {
                ret = writeFixedRecordMap(file, fixed_record_payload.get(), fixed_record_meta, fixed_record_size);
            }
            else
            {
                ret = writeEventViews(file, data);
            }
            auto const dataset_end = std::chrono::high_resolution_clock::now();
            lastProfile.datasetWriteUs = elapsedUs(dataset_start, dataset_end);
            if(ret == 0)
            {
                LOG_ERROR("[StoryChunkWriter] Error writing StoryChunk to file.");
                return ret;
            }

            {
                CL_PROFILE_REGION("grapher_hdf5_flush");
                auto const flush_start = std::chrono::high_resolution_clock::now();
                file->flush(H5F_SCOPE_GLOBAL);
                auto const flush_end = std::chrono::high_resolution_clock::now();
                lastProfile.flushUs = elapsedUs(flush_start, flush_end);
            }
            auto const filesize_start = std::chrono::high_resolution_clock::now();
            hsize_t file_size = file->getFileSize();
            auto const filesize_end = std::chrono::high_resolution_clock::now();
            lastProfile.fileSizeUs = elapsedUs(filesize_start, filesize_end);
            auto const close_start = std::chrono::high_resolution_clock::now();
            file->close();
            auto const close_end = std::chrono::high_resolution_clock::now();
            lastProfile.closeUs = elapsedUs(close_start, close_end);
            if(lastProfile.atomicRename && !raw_blob_deferred_publish)
            {
                auto const rename_start = std::chrono::high_resolution_clock::now();
                if(lastProfile.archiveLayout == "raw_blob")
                {
                    std::filesystem::rename(write_payload_file_name, final_payload_file_name);
                    if(rawBlobSidecarMeta)
                    {
                        std::filesystem::rename(write_meta_file_name, final_meta_file_name);
                    }
                }
                std::filesystem::rename(write_file_name, final_file_name);
                auto const rename_end = std::chrono::high_resolution_clock::now();
                lastProfile.renameUs = elapsedUs(rename_start, rename_end);
                lastProfile.publishRenameUs += lastProfile.renameUs;
            }
            if(!raw_blob_deferred_publish &&
               !writeArchiveManifest(final_file_name, story_chunk, raw_blob_payload_size, &lastProfile.archiveManifestWriteUs))
            {
                return 0;
            }

            LOG_DEBUG("[StoryChunkWriter] Finished writing StoryChunk to file.");
            lastProfile.totalUs = elapsedUs(total_start, std::chrono::high_resolution_clock::now());
            ret = file_size + raw_blob_payload_size;
        }
        if(raw_blob_deferred_publish)
        {
            if(raw_payload_write_pending && !finish_raw_payload_write())
            {
                std::error_code remove_ec;
                std::filesystem::remove(write_file_name, remove_ec);
                std::filesystem::remove(write_payload_file_name, remove_ec);
                return 0;
            }
            if(lastProfile.rawPayloadAsyncPublish)
            {
                AsyncArchivePublishTask task;
                task.storyId = story_chunk.getStoryId();
                task.startTime = story_chunk.getStartTime();
                task.writeFileName = write_file_name;
                task.finalFileName = final_file_name;
                task.writePayloadFileName = write_payload_file_name;
                task.finalPayloadFileName = final_payload_file_name;
                task.writeMetaFileName = write_meta_file_name;
                task.finalMetaFileName = final_meta_file_name;
                task.hasSidecarMeta = rawBlobSidecarMeta;
                task.publishBeforePayloadClose = rawBlobPublishBeforePayloadClose;
                task.payloadBytes = raw_blob_payload_size;
                task.manifestData = buildArchiveManifestData(story_chunk);
                if(raw_payload_close_pending)
                {
                    task.closeFuture = std::move(raw_payload_close_future);
                    raw_payload_close_pending = false;
                }
                else
                {
                    std::promise<RawPayloadCloseResult> close_promise;
                    close_promise.set_value(RawPayloadCloseResult{true, 0.0, ""});
                    task.closeFuture = close_promise.get_future();
                }
                asyncArchivePublisher().enqueue(std::move(task), lastProfile.rawPayloadAsyncPublishThreads);
                lastProfile.totalUs = elapsedUs(total_start, std::chrono::high_resolution_clock::now());
                LOG_INFO("[StoryChunkWriter] Async archive publish enqueued StoryID={} StartTime={} "
                         "payload_bytes={} publisher_threads={} writer_total_us={}",
                         story_chunk.getStoryId(),
                         story_chunk.getStartTime(),
                         raw_blob_payload_size,
                         lastProfile.rawPayloadAsyncPublishThreads,
                         lastProfile.totalUs);
                return ret;
            }
            if(!finish_raw_payload_close())
            {
                std::error_code remove_ec;
                std::filesystem::remove(write_file_name, remove_ec);
                std::filesystem::remove(write_payload_file_name, remove_ec);
                std::filesystem::remove(write_meta_file_name, remove_ec);
                return 0;
            }

            auto const rename_start = std::chrono::high_resolution_clock::now();
            std::filesystem::rename(write_payload_file_name, final_payload_file_name);
            if(rawBlobSidecarMeta)
            {
                std::filesystem::rename(write_meta_file_name, final_meta_file_name);
            }
            std::filesystem::rename(write_file_name, final_file_name);
            auto const rename_end = std::chrono::high_resolution_clock::now();
            double const publish_rename_us = elapsedUs(rename_start, rename_end);
            lastProfile.renameUs += publish_rename_us;
            lastProfile.publishRenameUs += publish_rename_us;
            if(!writeArchiveManifest(final_file_name, story_chunk, raw_blob_payload_size, &lastProfile.archiveManifestWriteUs))
            {
                return 0;
            }
            lastProfile.totalUs = elapsedUs(total_start, std::chrono::high_resolution_clock::now());
        }
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    return ret;
}

hsize_t StoryChunkWriter::writeEventViews(std::unique_ptr<H5::H5File>& file, std::vector<LogEventHVLView>& data)
{
    CL_PROFILE_REGION("storage_write");
    CL_PROFILE_REGION("grapher_hdf5_write_dataset");
    CL_PROFILE_COUNTER("storage_write_events", data.size());

    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating group: {}", groupName);
        auto const group_start = std::chrono::high_resolution_clock::now();
        auto* group = new H5::Group(file->createGroup(groupName));
        auto const group_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetGroupCreateUs += elapsedUs(group_start, group_end);

        hsize_t dim_size = data.size();
        LOG_DEBUG("[StoryChunkWriter] Creating dataspace with size: {}", dim_size);
        auto const dataspace_start = std::chrono::high_resolution_clock::now();
        auto* dataspace = new H5::DataSpace(numDims, &dim_size);
        auto const dataspace_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetDataspaceCreateUs += elapsedUs(dataspace_start, dataspace_end);

        LOG_DEBUG("[StoryChunkWriter] Creating data type for non-owning event views...");
        auto const datatype_start = std::chrono::high_resolution_clock::now();
        H5::CompType data_type = createEventViewCompoundType();
        auto const datatype_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetDatatypeCreateUs += elapsedUs(datatype_start, datatype_end);

        LOG_DEBUG("[StoryChunkWriter] Creating dataset: {}", dsetName);
        std::unique_ptr<H5::DSetCreatPropList> create_properties;
        if(lastProfile.chunkEvents > 0 && dim_size > 0)
        {
            hsize_t chunk_dim = std::min<hsize_t>(static_cast<hsize_t>(lastProfile.chunkEvents), dim_size);
            create_properties = std::make_unique<H5::DSetCreatPropList>();
            create_properties->setChunk(numDims, &chunk_dim);
        }
        auto const dataset_create_start = std::chrono::high_resolution_clock::now();
        auto* dataset = create_properties
                                ? new H5::DataSet(file->createDataSet("/" + groupName + "/" + dsetName + ".vlen_bytes",
                                                                      data_type,
                                                                      *dataspace,
                                                                      *create_properties))
                                : new H5::DataSet(file->createDataSet("/" + groupName + "/" + dsetName + ".vlen_bytes",
                                                                      data_type,
                                                                      *dataspace));
        auto const dataset_create_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetCreateUs += elapsedUs(dataset_create_start, dataset_create_end);

        LOG_DEBUG("[StoryChunkWriter] Writing non-owning event views to dataset...");
        auto const write_call_start = std::chrono::high_resolution_clock::now();
        if(!data.empty())
        {
            dataset->write(data.data(), data_type);
        }
        auto const write_call_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetWriteCallUs += elapsedUs(write_call_start, write_call_end);
        lastProfile.datasetPayloadWriteCallUs += elapsedUs(write_call_start, write_call_end);

        auto const object_close_start = std::chrono::high_resolution_clock::now();
        delete dataset;
        delete dataspace;
        delete group;
        auto const object_close_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetObjectCloseUs += elapsedUs(object_close_start, object_close_end);

        return data.size();
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    catch(H5::DataSetIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSetIException: {}", error.getCDetailMsg());
        H5::DataSetIException::printErrorStack();
    }
    catch(H5::DataSpaceIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSpaceIException: {}", error.getCDetailMsg());
        H5::DataSpaceIException::printErrorStack();
    }
    return 0;
}

hsize_t StoryChunkWriter::writeEvents(std::unique_ptr<H5::H5File>& file, std::vector<LogEventHVL>& data)
{
    CL_PROFILE_REGION("storage_write");
    CL_PROFILE_REGION("grapher_hdf5_write_dataset");
    CL_PROFILE_COUNTER("storage_write_events", data.size());

    int ret = 0;
    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating group: {}", groupName);
        auto const group_start = std::chrono::high_resolution_clock::now();
        auto* group = new H5::Group(file->createGroup(groupName));
        auto const group_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetGroupCreateUs += elapsedUs(group_start, group_end);

        hsize_t dim_size = data.size();
        LOG_DEBUG("[StoryChunkWriter] Creating dataspace with size: {}", dim_size);
        auto const dataspace_start = std::chrono::high_resolution_clock::now();
        auto* dataspace = new H5::DataSpace(numDims, &dim_size);
        auto const dataspace_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetDataspaceCreateUs += elapsedUs(dataspace_start, dataspace_end);

        // target dtype for the file
        LOG_DEBUG("[StoryChunkWriter] Creating data type for events...");
        auto const datatype_start = std::chrono::high_resolution_clock::now();
        H5::CompType data_type = createEventCompoundType();
        auto const datatype_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetDatatypeCreateUs += elapsedUs(datatype_start, datatype_end);

        LOG_DEBUG("[StoryChunkWriter] Creating dataset: {}", dsetName);
        std::unique_ptr<H5::DSetCreatPropList> create_properties;
        if(lastProfile.chunkEvents > 0 && dim_size > 0)
        {
            hsize_t chunk_dim = std::min<hsize_t>(static_cast<hsize_t>(lastProfile.chunkEvents), dim_size);
            create_properties = std::make_unique<H5::DSetCreatPropList>();
            create_properties->setChunk(numDims, &chunk_dim);
        }
        auto const dataset_create_start = std::chrono::high_resolution_clock::now();
        auto* dataset = create_properties
                                ? new H5::DataSet(file->createDataSet("/" + groupName + "/" + dsetName + ".vlen_bytes",
                                                                      data_type,
                                                                      *dataspace,
                                                                      *create_properties))
                                : new H5::DataSet(file->createDataSet("/" + groupName + "/" + dsetName + ".vlen_bytes",
                                                                      data_type,
                                                                      *dataspace));
        auto const dataset_create_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetCreateUs += elapsedUs(dataset_create_start, dataset_create_end);

        LOG_DEBUG("[StoryChunkWriter] Writing data to dataset...");
        auto const write_call_start = std::chrono::high_resolution_clock::now();
        dataset->write(&data.front(), data_type);
        auto const write_call_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetWriteCallUs += elapsedUs(write_call_start, write_call_end);
        lastProfile.datasetPayloadWriteCallUs += elapsedUs(write_call_start, write_call_end);

        auto const object_close_start = std::chrono::high_resolution_clock::now();
        delete dataset;
        delete dataspace;
        delete group;
        auto const object_close_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetObjectCloseUs += elapsedUs(object_close_start, object_close_end);

        return data.size();
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    catch(H5::DataSetIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSetIException: {}", error.getCDetailMsg());
        H5::DataSetIException::printErrorStack();
    }
    catch(H5::DataSpaceIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSpaceIException: {}", error.getCDetailMsg());
        H5::DataSpaceIException::printErrorStack();
    }
    catch(H5::DataTypeIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataTypeIException: {}", error.getCDetailMsg());
        H5::DataTypeIException::printErrorStack();
    }
    return ret;
}

hsize_t StoryChunkWriter::writeBlobMap(std::unique_ptr<H5::H5File>& file,
                                       std::vector<unsigned char> const& payload,
                                       std::vector<BlobMapEntry> const& meta)
{
    CL_PROFILE_REGION("storage_write");
    CL_PROFILE_REGION("grapher_hdf5_write_dataset");
    CL_PROFILE_COUNTER("storage_write_events", meta.size());

    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating group: {}", groupName);
        H5::Group group = file->createGroup(groupName);

        hsize_t payload_dim = payload.size();
        H5::DataSpace payload_space(numDims, &payload_dim);
        H5::DataSet payload_dataset = file->createDataSet("/" + groupName + "/" + dsetName + ".blob",
                                                          H5::PredType::NATIVE_UINT8,
                                                          payload_space);
        if(!payload.empty())
        {
            auto const payload_write_start = std::chrono::high_resolution_clock::now();
            payload_dataset.write(payload.data(), H5::PredType::NATIVE_UINT8);
            auto const payload_write_end = std::chrono::high_resolution_clock::now();
            lastProfile.datasetWriteCallUs += elapsedUs(payload_write_start, payload_write_end);
            lastProfile.datasetPayloadWriteCallUs += elapsedUs(payload_write_start, payload_write_end);
        }

        hsize_t meta_dim = meta.size();
        H5::DataSpace meta_space(numDims, &meta_dim);
        H5::CompType meta_type = createBlobMapMetaCompoundType();
        std::unique_ptr<H5::DSetCreatPropList> create_properties;
        if(lastProfile.chunkEvents > 0 && meta_dim > 0)
        {
            hsize_t chunk_dim = std::min<hsize_t>(static_cast<hsize_t>(lastProfile.chunkEvents), meta_dim);
            create_properties = std::make_unique<H5::DSetCreatPropList>();
            create_properties->setChunk(numDims, &chunk_dim);
        }
        H5::DataSet meta_dataset =
                create_properties
                ? file->createDataSet("/" + groupName + "/" + dsetName + ".meta",
                                      meta_type,
                                      meta_space,
                                      *create_properties)
                : file->createDataSet("/" + groupName + "/" + dsetName + ".meta", meta_type, meta_space);
        if(!meta.empty())
        {
            auto const meta_write_start = std::chrono::high_resolution_clock::now();
            meta_dataset.write(meta.data(), meta_type);
            auto const meta_write_end = std::chrono::high_resolution_clock::now();
            lastProfile.datasetWriteCallUs += elapsedUs(meta_write_start, meta_write_end);
            lastProfile.datasetMetaWriteCallUs += elapsedUs(meta_write_start, meta_write_end);
        }

        return meta.size();
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    catch(H5::DataSetIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSetIException: {}", error.getCDetailMsg());
        H5::DataSetIException::printErrorStack();
    }
    catch(H5::DataSpaceIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSpaceIException: {}", error.getCDetailMsg());
        H5::DataSpaceIException::printErrorStack();
    }
    catch(H5::DataTypeIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataTypeIException: {}", error.getCDetailMsg());
        H5::DataTypeIException::printErrorStack();
    }
    return 0;
}

hsize_t StoryChunkWriter::writeRawBlobMap(std::unique_ptr<H5::H5File>& file,
                                          std::vector<BlobMapEntry> const& meta)
{
    CL_PROFILE_REGION("storage_write");
    CL_PROFILE_REGION("grapher_hdf5_write_dataset");
    CL_PROFILE_COUNTER("storage_write_events", meta.size());

    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating group: {}", groupName);
        auto const group_start = std::chrono::high_resolution_clock::now();
        H5::Group group = file->createGroup(groupName);
        auto const group_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetGroupCreateUs += elapsedUs(group_start, group_end);

        hsize_t meta_dim = meta.size();
        auto const meta_space_start = std::chrono::high_resolution_clock::now();
        H5::DataSpace meta_space(numDims, &meta_dim);
        auto const meta_space_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetDataspaceCreateUs += elapsedUs(meta_space_start, meta_space_end);

        auto const meta_type_start = std::chrono::high_resolution_clock::now();
        H5::CompType meta_type = createBlobMapMetaCompoundType();
        auto const meta_type_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetDatatypeCreateUs += elapsedUs(meta_type_start, meta_type_end);

        auto const meta_dataset_create_start = std::chrono::high_resolution_clock::now();
        H5::DataSet meta_dataset =
                file->createDataSet("/" + groupName + "/" + dsetName + ".raw_meta", meta_type, meta_space);
        auto const meta_dataset_create_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetCreateUs += elapsedUs(meta_dataset_create_start, meta_dataset_create_end);

        if(!meta.empty())
        {
            auto const meta_write_start = std::chrono::high_resolution_clock::now();
            meta_dataset.write(meta.data(), meta_type);
            auto const meta_write_end = std::chrono::high_resolution_clock::now();
            lastProfile.datasetWriteCallUs += elapsedUs(meta_write_start, meta_write_end);
            lastProfile.datasetMetaWriteCallUs += elapsedUs(meta_write_start, meta_write_end);
        }

        return meta.size();
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    catch(H5::DataSetIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSetIException: {}", error.getCDetailMsg());
        H5::DataSetIException::printErrorStack();
    }
    catch(H5::DataSpaceIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSpaceIException: {}", error.getCDetailMsg());
        H5::DataSpaceIException::printErrorStack();
    }
    catch(H5::DataTypeIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataTypeIException: {}", error.getCDetailMsg());
        H5::DataTypeIException::printErrorStack();
    }
    return 0;
}

hsize_t StoryChunkWriter::writeFixedRecordMap(std::unique_ptr<H5::H5File>& file,
                                              unsigned char const* payload,
                                              std::vector<FixedRecordMetaEntry> const& meta,
                                              std::size_t record_size)
{
    CL_PROFILE_REGION("storage_write");
    CL_PROFILE_REGION("grapher_hdf5_write_dataset");
    CL_PROFILE_COUNTER("storage_write_events", meta.size());

    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating group: {}", groupName);
        auto const group_start = std::chrono::high_resolution_clock::now();
        H5::Group group = file->createGroup(groupName);
        auto const group_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetGroupCreateUs += elapsedUs(group_start, group_end);

        hsize_t meta_dim = meta.size();
        auto const meta_space_start = std::chrono::high_resolution_clock::now();
        H5::DataSpace meta_space(numDims, &meta_dim);
        auto const meta_space_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetDataspaceCreateUs += elapsedUs(meta_space_start, meta_space_end);

        auto const meta_type_start = std::chrono::high_resolution_clock::now();
        H5::CompType meta_type = createFixedRecordMetaCompoundType();
        auto const meta_type_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetDatatypeCreateUs += elapsedUs(meta_type_start, meta_type_end);

        auto const meta_dataset_create_start = std::chrono::high_resolution_clock::now();
        H5::DataSet meta_dataset =
                file->createDataSet("/" + groupName + "/" + dsetName + ".fixed_meta", meta_type, meta_space);
        auto const meta_dataset_create_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetCreateUs += elapsedUs(meta_dataset_create_start, meta_dataset_create_end);

        if(!meta.empty())
        {
            auto const meta_write_start = std::chrono::high_resolution_clock::now();
            meta_dataset.write(meta.data(), meta_type);
            auto const meta_write_end = std::chrono::high_resolution_clock::now();
            lastProfile.datasetWriteCallUs += elapsedUs(meta_write_start, meta_write_end);
            lastProfile.datasetMetaWriteCallUs += elapsedUs(meta_write_start, meta_write_end);
        }

        hsize_t payload_dims[2] = {meta.size(), record_size};
        auto const payload_space_start = std::chrono::high_resolution_clock::now();
        H5::DataSpace payload_space(2, payload_dims);
        auto const payload_space_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetDataspaceCreateUs += elapsedUs(payload_space_start, payload_space_end);

        auto const payload_dataset_create_start = std::chrono::high_resolution_clock::now();
        H5::DataSet payload_dataset = file->createDataSet("/" + groupName + "/" + dsetName + ".fixed_records",
                                                          H5::PredType::NATIVE_UINT8,
                                                          payload_space);
        auto const payload_dataset_create_end = std::chrono::high_resolution_clock::now();
        lastProfile.datasetCreateUs += elapsedUs(payload_dataset_create_start, payload_dataset_create_end);

        if(payload != nullptr && !meta.empty() && record_size > 0)
        {
            auto const payload_write_start = std::chrono::high_resolution_clock::now();
            payload_dataset.write(payload, H5::PredType::NATIVE_UINT8);
            auto const payload_write_end = std::chrono::high_resolution_clock::now();
            lastProfile.datasetWriteCallUs += elapsedUs(payload_write_start, payload_write_end);
            lastProfile.datasetPayloadWriteCallUs += elapsedUs(payload_write_start, payload_write_end);
        }

        return meta.size();
    }
    catch(H5::FileIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    catch(H5::DataSetIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSetIException: {}", error.getCDetailMsg());
        H5::DataSetIException::printErrorStack();
    }
    catch(H5::DataSpaceIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSpaceIException: {}", error.getCDetailMsg());
        H5::DataSpaceIException::printErrorStack();
    }
    catch(H5::DataTypeIException& error)
    {
        LOG_ERROR("[StoryChunkWriter] DataTypeIException: {}", error.getCDetailMsg());
        H5::DataTypeIException::printErrorStack();
    }
    return 0;
}

} // namespace chronolog
