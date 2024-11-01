#include "StoryChunkWriter.h"

namespace chronolog
{
hsize_t StoryChunkWriter::writeStoryChunk(StoryChunkHVL &story_chunk)
{
    std::vector <LogEventHVL> data;
    data.reserve(story_chunk.getEventCount());
    for(const auto &start: story_chunk)
    {
        data.push_back(start.second);
    }
    std::string file_name = rootDirectory + story_chunk.getChronicleName() + "." + story_chunk.getStoryName() + "." +
                            std::to_string(story_chunk.getStartTime() / 1000000000) + ".vlen.h5";
    hsize_t ret = 0;
    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating StoryChunk file: {}", file_name);
        auto *file = new H5::H5File(file_name, H5F_ACC_TRUNC);
        sleep(1);
        LOG_DEBUG("[StoryChunkWriter] Writing StoryChunk to file...");
        ret = writeEvents(file, data);
        if(ret == 0)
        {
            LOG_ERROR("[StoryChunkWriter] Error writing StoryChunk to file.");
            return ret;
        }
        file->flush(H5F_SCOPE_GLOBAL);
        hsize_t file_size = file->getFileSize();
        LOG_DEBUG("[StoryChunkWriter] Closing StoryChunk file, size: {}", file_size);
        delete file;
        LOG_DEBUG("[StoryChunkWriter] Finished writing StoryChunk to file.");
        return file_size;
    }
    catch(H5::FileIException &error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    return ret;
}

hsize_t StoryChunkWriter::writeStoryChunk(StoryChunk &story_chunk)
{
    std::vector <LogEventHVL> data;
    data.reserve(story_chunk.getEventCount());
    for(const auto &event: story_chunk)
    {
        LogEventHVL event_hvl;
        event_hvl.storyId = event.second.getStoryId();
        event_hvl.eventTime = event.second.time();
        event_hvl.clientId = event.second.getClientId();
        event_hvl.eventIndex = event.second.index();
        event_hvl.logRecord.len = event.second.logRecord.size();
        event_hvl.logRecord.p = new uint8_t[event_hvl.logRecord.len];
        std::memcpy(event_hvl.logRecord.p, event.second.logRecord.data(), event_hvl.logRecord.len);
        data.push_back(event_hvl);
    }
    std::string file_name = rootDirectory + "/" + story_chunk.getChronicleName() + "." + story_chunk.getStoryName() + "." +
                            std::to_string(story_chunk.getStartTime() / 1000000000) + ".vlen.h5";
    hsize_t ret = 0;
    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating StoryChunk file: {}", file_name);
        auto *file = new H5::H5File(file_name, H5F_ACC_TRUNC);

        LOG_DEBUG("[StoryChunkWriter] Writing StoryChunk to file...");
        ret = writeEvents(file, data);
        if(ret == 0)
        {
            LOG_ERROR("[StoryChunkWriter] Error writing StoryChunk to file.");
            return ret;
        }

        file->flush(H5F_SCOPE_GLOBAL);
        hsize_t file_size = file->getFileSize();

        LOG_DEBUG("[StoryChunkWriter] Closing StoryChunk file, size: {}", file_size);
        delete file;

        LOG_DEBUG("[StoryChunkWriter] Finished writing StoryChunk to file.");
        ret = file_size;
    }
    catch(H5::FileIException &error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    for(auto &event: data)
    {
        delete[] static_cast<uint8_t *>(event.logRecord.p);
        event.logRecord.p = nullptr;
    }
    return ret;
}

hsize_t StoryChunkWriter::writeEvents(H5::H5File *file, std::vector <LogEventHVL> &data)
{
    int ret = 0;
    try
    {
        /*
         * Create a group in the file
        */
        LOG_DEBUG("[StoryChunkWriter] Creating group: {}", groupName);
        auto *group = new H5::Group(file->createGroup(groupName));

        hsize_t dim_size = data.size();
        LOG_DEBUG("[StoryChunkWriter] Creating dataspace with size: {}", dim_size);
        auto *dataspace = new H5::DataSpace(numDims, &dim_size);

        // target dtype for the file
        LOG_DEBUG("[StoryChunkWriter] Creating data type for events...");
        H5::CompType data_type = createEventCompoundType();

        LOG_DEBUG("[StoryChunkWriter] Creating dataset: {}", dsetName);
        auto *dataset = new H5::DataSet(
                file->createDataSet("/" + groupName + "/" + dsetName + ".vlen_bytes", data_type, *dataspace));

        LOG_DEBUG("[StoryChunkWriter] Writing data to dataset...");
        dataset->write(&data.front(), data_type);

        delete dataset;
        delete dataspace;
        delete group;

        return data.size();
    }
    catch(H5::FileIException &error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    catch(H5::DataSetIException &error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSetIException: {}", error.getCDetailMsg());
        H5::DataSetIException::printErrorStack();
    }
    catch(H5::DataSpaceIException &error)
    {
        LOG_ERROR("[StoryChunkWriter] DataSpaceIException: {}", error.getCDetailMsg());
        H5::DataSpaceIException::printErrorStack();
    }
    catch(H5::DataTypeIException &error)
    {
        LOG_ERROR("[StoryChunkWriter] DataTypeIException: {}", error.getCDetailMsg());
        H5::DataTypeIException::printErrorStack();
    }
    return ret;
}

H5::CompType StoryChunkWriter::createEventCompoundType()
{
    H5::CompType data_type(sizeof(LogEventHVL));
    data_type.insertMember("storyId", HOFFSET(LogEventHVL, storyId), H5::PredType::NATIVE_UINT64);
    data_type.insertMember("eventTime", HOFFSET(LogEventHVL, eventTime), H5::PredType::NATIVE_UINT64);
    data_type.insertMember("clientId", HOFFSET(LogEventHVL, clientId), H5::PredType::NATIVE_UINT32);
    data_type.insertMember("eventIndex", HOFFSET(LogEventHVL, eventIndex), H5::PredType::NATIVE_UINT32);
    data_type.insertMember("logRecord", HOFFSET(LogEventHVL, logRecord), H5::VarLenType(H5::PredType::NATIVE_UINT8));
    return data_type;
}

}