#ifndef CHRONOLOG_STORY_CHUNK_WRITER_H
#define CHRONOLOG_STORY_CHUNK_WRITER_H

#include <string>
#include <memory>
#include <cstdint>
#include <sys/uio.h>
#include <vector>
#include <H5Cpp.h>
#include <chrono_monitor.h>

#include "StoryChunk.h"
#include "LogEventHVL.h"
#include "StoryChunkHVL.h"

namespace chronolog
{
class StoryChunkWriter
{
public:
    struct WriteProfile
    {
        double filenameScanUs{0.0};
        double prepUs{0.0};
        double prepScanUs{0.0};
        double prepBuildUs{0.0};
        double prepPayloadCopyUs{0.0};
        double hdf5LockWaitUs{0.0};
        double openUs{0.0};
        double datasetWriteUs{0.0};
        double datasetGroupCreateUs{0.0};
        double datasetDataspaceCreateUs{0.0};
        double datasetDatatypeCreateUs{0.0};
        double datasetCreateUs{0.0};
        double datasetWriteCallUs{0.0};
        double datasetPayloadWriteCallUs{0.0};
        double datasetMetaWriteCallUs{0.0};
        double rawPayloadOpenUs{0.0};
        double rawPayloadPreallocateUs{0.0};
        double rawPayloadWritevUs{0.0};
        double rawPayloadCloseUs{0.0};
        double rawPayloadCloseWaitUs{0.0};
        double rawPayloadWriteWaitUs{0.0};
        std::size_t rawPayloadBytes{0};
        std::size_t rawPayloadWritevCalls{0};
        std::size_t rawPayloadPartialWrites{0};
        int rawPayloadPreallocateResult{0};
        bool rawPayloadAsyncWrite{false};
        bool rawPayloadAsyncClose{false};
        double datasetObjectCloseUs{0.0};
        double flushUs{0.0};
        double fileSizeUs{0.0};
        double closeUs{0.0};
        double renameUs{0.0};
        double publishRenameUs{0.0};
        double archiveManifestWriteUs{0.0};
        double totalUs{0.0};
        bool atomicRename{false};
        bool rawPayloadAsyncPublish{false};
        std::size_t rawPayloadAsyncPublishThreads{1};
        std::size_t chunkEvents{0};
        std::string archiveLayout{"vlen"};
    };

    struct BlobMapEntry
    {
        uint64_t storyId;
        uint64_t eventTime;
        uint64_t clientId;
        uint32_t eventIndex;
        uint64_t offset;
        uint64_t size;
    };

    struct FixedRecordMetaEntry
    {
        uint64_t storyId;
        uint64_t eventTime;
        uint64_t clientId;
        uint32_t eventIndex;
        uint64_t size;
    };

    struct LogEventHVLView
    {
        uint64_t storyId;
        uint64_t eventTime;
        uint32_t clientId;
        uint32_t eventIndex;
        hvl_t logRecord;
    };

    StoryChunkWriter(std::string const &root_dir, std::string const &group_name, std::string const &dset_name)
            : rootDirectory(root_dir), groupName(group_name), dsetName(dset_name), numDims(1)
    {};

    ~StoryChunkWriter()
    {
        LOG_INFO("[StoryChunkWriter] Destructor called. Cleaning up...");
    }

    hsize_t writeStoryChunk(StoryChunkHVL &story_chunk);

    hsize_t writeStoryChunk(StoryChunk &story_chunk);

    hsize_t writeEvents(std::unique_ptr<H5::H5File> &file, std::vector <LogEventHVL> &data);

    hsize_t writeEventViews(std::unique_ptr<H5::H5File> &file, std::vector<LogEventHVLView> &data);

    hsize_t writeBlobMap(std::unique_ptr<H5::H5File> &file,
                         std::vector<unsigned char> const& payload,
                         std::vector<BlobMapEntry> const& meta);

    hsize_t writeRawBlobMap(std::unique_ptr<H5::H5File> &file,
                            std::vector<BlobMapEntry> const& meta);

    hsize_t writeFixedRecordMap(std::unique_ptr<H5::H5File> &file,
                                unsigned char const* payload,
                                std::vector<FixedRecordMetaEntry> const& meta,
                                std::size_t record_size);

    WriteProfile const& lastWriteProfile() const { return lastProfile; }

    static H5::CompType createEventCompoundType()
    {
        H5::CompType data_type(sizeof(LogEventHVL));
        data_type.insertMember("storyId", HOFFSET(LogEventHVL, storyId), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventTime", HOFFSET(LogEventHVL, eventTime), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("clientId", HOFFSET(LogEventHVL, clientId), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("eventIndex", HOFFSET(LogEventHVL, eventIndex), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("logRecord", HOFFSET(LogEventHVL, logRecord), H5::VarLenType(H5::PredType::NATIVE_UINT8));
        return data_type;
    }

    static H5::CompType createEventViewCompoundType()
    {
        H5::CompType data_type(sizeof(LogEventHVLView));
        data_type.insertMember("storyId", HOFFSET(LogEventHVLView, storyId), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventTime", HOFFSET(LogEventHVLView, eventTime), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("clientId", HOFFSET(LogEventHVLView, clientId), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("eventIndex", HOFFSET(LogEventHVLView, eventIndex), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("logRecord",
                               HOFFSET(LogEventHVLView, logRecord),
                               H5::VarLenType(H5::PredType::NATIVE_UINT8));
        return data_type;
    }

    static H5::CompType createBlobMapMetaCompoundType()
    {
        H5::CompType data_type(sizeof(BlobMapEntry));
        data_type.insertMember("storyId", HOFFSET(BlobMapEntry, storyId), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventTime", HOFFSET(BlobMapEntry, eventTime), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("clientId", HOFFSET(BlobMapEntry, clientId), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventIndex", HOFFSET(BlobMapEntry, eventIndex), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("offset", HOFFSET(BlobMapEntry, offset), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("size", HOFFSET(BlobMapEntry, size), H5::PredType::NATIVE_UINT64);
        return data_type;
    }

    static H5::CompType createFixedRecordMetaCompoundType()
    {
        H5::CompType data_type(sizeof(FixedRecordMetaEntry));
        data_type.insertMember("storyId", HOFFSET(FixedRecordMetaEntry, storyId), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventTime", HOFFSET(FixedRecordMetaEntry, eventTime), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("clientId", HOFFSET(FixedRecordMetaEntry, clientId), H5::PredType::NATIVE_UINT64);
        data_type.insertMember("eventIndex", HOFFSET(FixedRecordMetaEntry, eventIndex), H5::PredType::NATIVE_UINT32);
        data_type.insertMember("size", HOFFSET(FixedRecordMetaEntry, size), H5::PredType::NATIVE_UINT64);
        return data_type;
    }

    // base_file_name should be in the format of chronicleName.storyName.startTime.vlen.h5, not including the path
    static std::string getStoryChunkFileName(std::string const &root_dir, std::string const &base_file_name);

private:
    std::string rootDirectory;
    std::string groupName;
    std::string dsetName;
    int numDims;
    WriteProfile lastProfile;
};
} // chronolog

#endif //CHRONOLOG_STORY_CHUNK_WRITER_H
