#include <string>
#include <chrono>
#include <thallium.hpp>

#include <StoryChunk.h>
#include <StoryChunkWriter.h>
#include <HDF5FileChunkExtractor.h>
#include <chronolog_profile.h>

namespace tl = thallium;

namespace chl = chronolog;

chronolog::HDF5FileChunkExtractor::HDF5FileChunkExtractor(const std::string& hdf5_files_root_dir)
    : rootDirectory(hdf5_files_root_dir)
{
    LOG_DEBUG("[HDF5FileChunkExtractor] Destructor called. Cleaning up...");
}

chronolog::HDF5FileChunkExtractor::~HDF5FileChunkExtractor()
{
    LOG_DEBUG("[HDF5FileChunkExtractor] Destructor called. Cleaning up...");
}

int chronolog::HDF5FileChunkExtractor::process_chunk(chl::StoryChunk* story_chunk)
{
    CL_PROFILE_REGION("grapher_archive_chunk");
    CL_PROFILE_COUNTER("grapher_archive_chunk_events", story_chunk->getEventCount());

    LOG_INFO("[HDF5FileChunkExtractor] tl::thread_id={} processing chunk StoryId={} {}-{} {}-{} eventCount {}",
             thallium::thread::self_id(),
             story_chunk->getStoryId(),
             story_chunk->getChronicleName(),
             story_chunk->getStoryName(),
             story_chunk->getStartTime(),
             story_chunk->getEndTime(),
             story_chunk->getEventCount());

    hsize_t size = 0;
    double lock_wait_us = 0.0;
    StoryChunkWriter::WriteProfile writer_profile;
    auto const write_start = std::chrono::high_resolution_clock::now();
    {
        CL_PROFILE_REGION("grapher_hdf5_write");
        StoryChunkWriter chunkWriter(rootDirectory, "story_chunks", "data");
        size = chunkWriter.writeStoryChunk(*story_chunk);
        writer_profile = chunkWriter.lastWriteProfile();
        lock_wait_us = writer_profile.hdf5LockWaitUs;
    }
    auto const write_end = std::chrono::high_resolution_clock::now();
    auto const write_us =
            std::chrono::duration_cast<std::chrono::nanoseconds>(write_end - write_start).count() / 1000.0;
    if(size == 0)
    {
        LOG_ERROR("[HDF5FileChunkExtractor] Error writing StoryChunk to file: StoryId={} {}-{} {}-{} eventCount {}",
                  story_chunk->getStoryId(),
                  story_chunk->getChronicleName(),
                  story_chunk->getStoryName(),
                  story_chunk->getStartTime(),
                  story_chunk->getEndTime(),
                  story_chunk->getEventCount());
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
        LOG_INFO("[HDF5FileChunkExtractor] Write profile StoryID={} StartTime={} eventCount={} fileSize={} write_us={}",
                 story_chunk->getStoryId(),
                 story_chunk->getStartTime(),
                 story_chunk->getEventCount(),
                 size,
                 write_us);
        LOG_INFO("[HDF5FileChunkExtractor] Write subphase profile StoryID={} StartTime={} eventCount={} fileSize={} "
                 "layout={} atomic_rename={} chunk_events={} prep_us={} prep_scan_us={} prep_build_us={} "
                 "prep_payload_copy_us={} lock_wait_us={} filename_scan_us={} open_us={} dataset_write_us={} flush_us={} "
                 "dataset_group_create_us={} dataset_dataspace_create_us={} dataset_datatype_create_us={} "
                 "dataset_create_us={} dataset_write_call_us={} dataset_payload_write_call_us={} "
                 "dataset_meta_write_call_us={} dataset_object_close_us={} "
                 "raw_payload_open_us={} raw_payload_preallocate_us={} raw_payload_writev_us={} "
                 "raw_payload_close_us={} raw_payload_close_wait_us={} raw_payload_write_wait_us={} "
                 "raw_payload_bytes={} raw_payload_writev_calls={} "
                 "raw_payload_partial_writes={} raw_payload_preallocate_result={} "
                 "raw_payload_async_write={} raw_payload_async_close={} "
                 "raw_payload_async_publish={} raw_payload_async_publish_threads={} "
                 "file_size_us={} close_us={} rename_us={} publish_rename_us={} archive_manifest_write_us={} "
                 "writer_total_us={} write_us={}",
                 story_chunk->getStoryId(),
                 story_chunk->getStartTime(),
                 story_chunk->getEventCount(),
                 size,
                 writer_profile.archiveLayout,
                 writer_profile.atomicRename ? 1 : 0,
                 writer_profile.chunkEvents,
                 writer_profile.prepUs,
                 writer_profile.prepScanUs,
                 writer_profile.prepBuildUs,
                 writer_profile.prepPayloadCopyUs,
                 lock_wait_us,
                 writer_profile.filenameScanUs,
                 writer_profile.openUs,
                 writer_profile.datasetWriteUs,
                 writer_profile.flushUs,
                 writer_profile.datasetGroupCreateUs,
                 writer_profile.datasetDataspaceCreateUs,
                 writer_profile.datasetDatatypeCreateUs,
                 writer_profile.datasetCreateUs,
                 writer_profile.datasetWriteCallUs,
                 writer_profile.datasetPayloadWriteCallUs,
                 writer_profile.datasetMetaWriteCallUs,
                 writer_profile.datasetObjectCloseUs,
                 writer_profile.rawPayloadOpenUs,
                 writer_profile.rawPayloadPreallocateUs,
                 writer_profile.rawPayloadWritevUs,
                 writer_profile.rawPayloadCloseUs,
                 writer_profile.rawPayloadCloseWaitUs,
                 writer_profile.rawPayloadWriteWaitUs,
                 writer_profile.rawPayloadBytes,
                 writer_profile.rawPayloadWritevCalls,
                 writer_profile.rawPayloadPartialWrites,
                 writer_profile.rawPayloadPreallocateResult,
                 writer_profile.rawPayloadAsyncWrite ? 1 : 0,
                 writer_profile.rawPayloadAsyncClose ? 1 : 0,
                 writer_profile.rawPayloadAsyncPublish ? 1 : 0,
                 writer_profile.rawPayloadAsyncPublishThreads,
                 writer_profile.fileSizeUs,
                 writer_profile.closeUs,
                 writer_profile.renameUs,
                 writer_profile.publishRenameUs,
                 writer_profile.archiveManifestWriteUs,
                 writer_profile.totalUs,
                 write_us);
        return chl::CL_SUCCESS;
    }
}
