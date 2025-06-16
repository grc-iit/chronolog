#include <filesystem>
#include <regex>
#include "StoryChunkWriter.h"

namespace fs = std::filesystem;

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
    std::unique_ptr<H5::H5File> file;
    try
    {
        LOG_DEBUG("[StoryChunkWriter] Creating StoryChunk file: {}", file_name);
        file = std::make_unique<H5::H5File>(file_name, H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE);

        LOG_DEBUG("[StoryChunkWriter] Writing StoryChunk to file...");
        ret = writeEvents(file, data);
        if(ret == 0)
        {
            LOG_ERROR("[StoryChunkWriter] Error writing StoryChunk to file.");
            return ret;
        }

        file->flush(H5F_SCOPE_GLOBAL);
        hsize_t file_size = file->getFileSize();

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

std::string StoryChunkWriter::getStoryFileName(std::string const &root_dir, std::string const &base_file_name)
{
    const std::string escaped_base_file_name = std::regex_replace(base_file_name, std::regex(R"([.^$|()\\*+?{}\[\]])"), R"(\$&)");
    const std::string rotated_prefix = escaped_base_file_name + ".aux.";

    const std::regex rotated_pattern("^" + rotated_prefix + "([0-9]+)$");

    long long max_n = -1; // Use -1 to indicate no numbered files have been found yet.
    bool base_exists = false;

    std::error_code ec;
    for(const auto &entry: fs::directory_iterator(root_dir, ec))
    {
        if(!entry.is_regular_file(ec))
        {
            continue; // Skip directories, symlinks, etc.
        }

        const std::string filename = entry.path().filename().string();

        // Check for the base file
        if(filename == base_file_name)
        {
            base_exists = true;
            continue;
        }

        // Check for rotated files
        std::smatch match;
        if(std::regex_match(filename, match, rotated_pattern))
        {
            // match[0] is the entire string, match[1] is the first capture group.
            if(match.size() == 2)
            {
                try
                {
                    long long current_n = std::stoll(match[1].str());
                    max_n = std::max(max_n, current_n);
                }
                catch(const std::out_of_range &)
                {
                    // Number was too large to fit in a long long, ignore.
                }
                catch(const std::invalid_argument &)
                {
                    // Should not happen with this regex, but good practice.
                }
            }
        }
    }

    fs::path next_filename;
    if(max_n == -1)
    {
        // No numbered files were found.
        if(!base_exists)
        {
            next_filename = base_file_name;
        }
        else
        {
            next_filename = rotated_prefix + "1";
        }
    }
    else
    {
        // We found numbered files, so the next is max_n + 1.
        next_filename = rotated_prefix + std::to_string(max_n + 1);
    }

    LOG_DEBUG("[StoryChunkWriter] Next unique file name: {}", next_filename.string());
    return root_dir / next_filename;
}

hsize_t StoryChunkWriter::writeStoryChunk(StoryChunk &story_chunk)
{
    std::vector <LogEventHVL> data;
    data.reserve(story_chunk.getEventCount());
    for(const auto &event: story_chunk)
    {
        hvl_t log_record;
        log_record.len = event.second.logRecord.size();
        log_record.p = (void*)event.second.logRecord.data();
        data.emplace_back(event.second.getStoryId(), event.second.time(), event.second.getClientId()
                          ,event.second.index(), log_record);
    }
    std::string file_name = story_chunk.getChronicleName() + "." + story_chunk.getStoryName() + "." +
                            std::to_string(story_chunk.getStartTime() / 1000000000) + ".vlen.h5";
//    file_name = fs::path(rootDirectory) / fs::path(file_name);
    hsize_t ret = 0;
    std::unique_ptr<H5::H5File> file;
    try
    {
        LOG_DEBUG("[StoryChunkWriter] Making sure the StoryChunk file name is unique...");
        file_name = getStoryFileName(rootDirectory, file_name);

        LOG_DEBUG("[StoryChunkWriter] Creating StoryChunk file: {}", file_name);
        file = std::make_unique<H5::H5File>(file_name, H5F_ACC_TRUNC | H5F_ACC_SWMR_WRITE);

        LOG_DEBUG("[StoryChunkWriter] Writing StoryChunk to file...");
        ret = writeEvents(file, data);
        if(ret == 0)
        {
            LOG_ERROR("[StoryChunkWriter] Error writing StoryChunk to file.");
            return ret;
        }

        file->flush(H5F_SCOPE_GLOBAL);
        hsize_t file_size = file->getFileSize();

        LOG_DEBUG("[StoryChunkWriter] Finished writing StoryChunk to file.");
        ret = file_size;
    }
    catch(H5::FileIException &error)
    {
        LOG_ERROR("[StoryChunkWriter] FileIException: {}", error.getCDetailMsg());
        H5::FileIException::printErrorStack();
    }
    return ret;
}

hsize_t StoryChunkWriter::writeEvents(std::unique_ptr<H5::H5File> &file, std::vector <LogEventHVL> &data)
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

}