#include <iostream>
#include <string>
#include <chrono>
#include <H5Cpp.h>
#include <utility>
#include <vector>
#include <random>
#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <algorithm>
#include <fstream>
#include <json-c/json.h>
#include <map>
#include <sstream>
#include <mutex>
#include <tuple>

/**********************************************************************************************************************
 * Global constants
 *********************************************************************************************************************/
const hsize_t n_dims = 1;
uint64_t n_records = 1000000;
hsize_t mean_length = 100;
int range_start_percentage = -1; // [0..100]
int range_end_percentage = -1;  // [0..100]
int range_step_percentage = 10;
hsize_t total_story_chunk_size = 0;
const std::string story_chunk_dataset_name = "08h00m00_08h10m00";
const std::string group_name = "observations";
const std::string story_file_name = "story_temp_2020-12-22_08h00m00_chicago";
const std::string ref_blob_file_name = "ref_blob.h5";
const std::string ref_json_file_name = "ref_json.h5";
const uint64_t test_story_id = 1234567890;
const uint64_t test_start_timestamp = 0;
uint64_t max_story_chunk2_size = 0;

/**********************************************************************************************************************
 * Data structures
 *********************************************************************************************************************/
struct LogEvent
{
    uint64_t storyId;
    uint64_t eventTime{};
    uint32_t clientId{};
    uint32_t eventIndex{};
    hvl_t logRecord{};

    LogEvent(): storyId(0), eventTime(0), clientId(0), eventIndex(0), logRecord()
    {};

    LogEvent(uint64_t storyId, uint64_t eventTime, uint32_t clientId, uint32_t eventIndex, hvl_t logRecord): storyId(
            storyId), eventTime(eventTime), clientId(clientId), eventIndex(eventIndex), logRecord(logRecord)
    {};

    [[nodiscard]] uint64_t const &time() const
    { return eventTime; }

    [[nodiscard]] uint32_t const &index() const
    { return eventIndex; }

    [[nodiscard]] std::string toString() const
    {
        std::string str =
                "storyId: " + std::to_string(storyId) + "\n" + "eventTime: " + std::to_string(eventTime) + "\n" +
                "clientId: " + std::to_string(clientId) + "\n" + "eventIndex: " + std::to_string(eventIndex) + "\n";
        str += "logRecord: ";
        char*ptr = static_cast<char*>(logRecord.p);
        for(hsize_t i = 0; i < logRecord.len; i++)
        {
            str += *(ptr + i);
            str += " ";
        }
        str += "\n";
//        LOG_DEBUG("logRecord len: {}", logRecord.len);
        return str;
    }

    bool operator==(const LogEvent &other) const
    {
        if(storyId != other.storyId)
        {
            return false;
        }
        if(eventTime != other.eventTime)
        {
            return false;
        }
        if(clientId != other.clientId)
        {
            return false;
        }
        if(eventIndex != other.eventIndex)
        {
            return false;
        }
        for(hsize_t i = 0; i < logRecord.len; i++)
        {
            if(((uint8_t*)logRecord.p)[i] != ((uint8_t*)other.logRecord.p)[i])
            {
                return false;
            }
        }
        return true;
    }

    bool operator!=(const LogEvent &other) const
    {
        return !(*this == other);
    }

    bool operator<(const LogEvent &other) const
    {
        return eventTime < other.eventTime;
    }
};

struct StoryChunk
{
    typedef uint64_t chrono_time;
    typedef uint32_t chrono_index;
    typedef uint64_t StoryId;
    typedef uint64_t ChronicleId;
    typedef uint32_t ClientId;
    typedef std::tuple <chrono_time, ClientId, chrono_index> EventSequence;

    explicit StoryChunk(StoryId const &story_id = 0, uint64_t start_time = 0, uint64_t end_time = 0): storyId(story_id)
                                                                                                      , startTime(
                    start_time), endTime(end_time), revisionTime(start_time)
    {}

    ~StoryChunk() = default;

    [[nodiscard]] StoryId const &getStoryId() const
    { return storyId; }

    [[nodiscard]] uint64_t getStartTime() const
    { return startTime; }

    [[nodiscard]] uint64_t getEndTime() const
    { return endTime; }

    [[nodiscard]] bool empty() const
    { return logEvents.empty(); }

    [[nodiscard]] size_t getNumEvents() const
    { return logEvents.size(); }

    [[nodiscard]] size_t getTotalPayloadSize() const
    {
        size_t total_size = 0;
        for(auto const &event: logEvents)
        { total_size += event.second.logRecord.len; }
        return total_size;
    }

    [[nodiscard]] std::map <EventSequence, LogEvent>::const_iterator begin() const
    { return logEvents.begin(); }

    [[nodiscard]] std::map <EventSequence, LogEvent>::const_iterator end() const
    { return logEvents.end(); }

    [[nodiscard]] std::map <EventSequence, LogEvent>::const_iterator lower_bound(uint64_t chrono_time) const
    { return logEvents.lower_bound(EventSequence{chrono_time, 0, 0}); }

    [[nodiscard]] uint64_t firstEventTime() const
    { return (*logEvents.begin()).second.time(); }

    int insertEvent(LogEvent const &event)
    {
        if((event.time() >= startTime) && (event.time() < endTime))
        {
            logEvents.insert(std::pair <EventSequence, LogEvent>({event.time(), event.clientId, event.index()}, event));
            return 1;
        }
        else
        { return 0; }
    }

    bool operator==(const StoryChunk &other) const
    {
        return ((storyId == other.storyId) && (startTime == other.startTime) && (endTime == other.endTime) &&
                (revisionTime == other.revisionTime) && (logEvents == other.logEvents));
    }

    bool operator!=(const StoryChunk &other) const
    {
        return !(*this == other);
    }

    [[nodiscard]] std::string toString() const
    {
        std::stringstream ss;
        ss << "StoryChunk:{" << storyId << ":" << startTime << ":" << endTime << "} has " << logEvents.size()
           << " events: ";
        for(auto const &event: logEvents)
        {
            ss << "<" << std::get <0>(event.first) << ", " << std::get <1>(event.first) << ", "
               << std::get <2>(event.first) << ">: " << event.second.toString();
        }
//        LOG_DEBUG("string size in StoryChunk::toString(): {}", ss.str().size());
        return ss.str();
    }

    StoryId storyId;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t revisionTime;

    std::map <EventSequence, LogEvent> logEvents;

};

struct StoryChunk2
{
    typedef uint64_t chrono_time;
    typedef uint32_t chrono_index;
    typedef uint64_t StoryId;
    typedef uint64_t ChronicleId;
    typedef uint32_t ClientId;

    typedef std::tuple <chrono_time, ClientId, chrono_index> EventSequence;
    typedef std::tuple <uint64_t, uint64_t> EventOffsetSize;

    explicit StoryChunk2(StoryId const &story_id = 0, uint64_t start_time = 0, uint64_t end_time = 0): storyId(story_id)
                                                                                                       , startTime(
                    start_time), endTime(end_time), revisionTime(start_time)
    {
        eventData = (unsigned char*)malloc(max_story_chunk2_size);
    }

    StoryChunk2(StoryChunk2 &other)
    {
        storyId = other.storyId;
        startTime = other.startTime;
        endTime = other.endTime;
        revisionTime = other.revisionTime;
        offsetSizeMap = other.offsetSizeMap;
        currentOffset = other.currentOffset;
        totalSize = other.totalSize;
        eventData = (unsigned char*)malloc(max_story_chunk2_size);
        memcpy(eventData, other.eventData, totalSize);
    }

    ~StoryChunk2()
    {
        free(eventData);
    }

    int insertEvent(LogEvent const &event)
    {
        if((event.time() >= startTime) && (event.time() < endTime))
        {
            std::lock_guard <std::mutex> lock(offsetMapMutex);
            memcpy((uint8_t*)eventData + currentOffset, event.logRecord.p, event.logRecord.len);
            offsetSizeMap.insert(
                    std::pair <EventSequence, EventOffsetSize>({event.time(), event.clientId, event.index()}, {
                            currentOffset, event.logRecord.len}));
            totalEventCount++;
            currentOffset += event.logRecord.len;
            totalSize += event.logRecord.len;
            return 1;
        }
        else
        { return 0; }
    }

    bool operator==(const StoryChunk2 &other) const
    {
        if((storyId != other.storyId) || (startTime != other.startTime) || (endTime != other.endTime) ||
           (revisionTime != other.revisionTime))
            return false;
        if(totalSize != other.totalSize) return false;
        if(currentOffset != other.currentOffset) return false;
        if(offsetSizeMap.size() != other.offsetSizeMap.size()) return false;
        for(auto const &entry: offsetSizeMap)
        {
            if(other.offsetSizeMap.find(entry.first) == other.offsetSizeMap.end()) return false;
            if(other.offsetSizeMap.at(entry.first) != entry.second) return false;
        }
        for(size_t i = 0; i < currentOffset; i++)
        {
            if(((uint8_t*)eventData)[i] != ((uint8_t*)other.eventData)[i]) return false;
        }
        return true;
    }

    bool operator!=(const StoryChunk2 &other) const
    {
        return !(*this == other);
    }

    [[nodiscard]] std::string toString() const
    {
        std::stringstream ss;
        ss << "StoryChunk2:{" << storyId << ":" << startTime << ":" << endTime << "} has " << offsetSizeMap.size()
           << " events: ";
        for(auto const &offsetSizeEntry: offsetSizeMap)
        {
            ss << "<" << std::get <0>(offsetSizeEntry.first) << ", " << std::get <1>(offsetSizeEntry.first) << ", "
               << std::get <2>(offsetSizeEntry.first) << ">: ";
            for(size_t i = std::get <0>(offsetSizeEntry.second);
                i < std::get <0>(offsetSizeEntry.second) + std::get <1>(offsetSizeEntry.second); i++)
//            ss << "<" << offsetSizeEntry.first.time << ", " << offsetSizeEntry.first.clientId << ", "
//               << offsetSizeEntry.first.index << ">: ";
//            for(size_t i = offsetSizeEntry.second.offset;
//                i < offsetSizeEntry.second.offset + offsetSizeEntry.second.size; i++)
            {
                ss << ((uint8_t*)eventData)[i];
            }
        }
        LOG_DEBUG("string size in StoryChunk2::toString(): {}", ss.str().size());
        return ss.str();
    }

    StoryId storyId;
    uint64_t startTime;
    uint64_t endTime;
    uint64_t revisionTime;

    std::map <EventSequence, EventOffsetSize> offsetSizeMap;
    uint64_t totalEventCount = 0;
    uint64_t currentOffset = 0;
    uint64_t totalSize = 0;
    std::mutex offsetMapMutex;
    unsigned char*eventData{};
};

struct OffsetMapEntry
{
    StoryChunk2::EventSequence eventId;
    StoryChunk2::EventOffsetSize offsetSize;

    OffsetMapEntry()
    {
        eventId = StoryChunk2::EventSequence(0, 0, 0);
        offsetSize = StoryChunk2::EventOffsetSize(0, 0);
    }

    OffsetMapEntry(StoryChunk2::EventSequence eventId, StoryChunk2::EventOffsetSize offsetSize): eventId(
            std::move(eventId)), offsetSize(std::move(offsetSize))
    {}
};

/**********************************************************************************************************************
 * Global variables
 *********************************************************************************************************************/
std::vector <std::vector <uint8_t>> log_record_bytes;
std::vector <LogEvent> events;

void parseCommandLineOptions(int argc, char*argv[])
{
    if(argc > 1) n_records = std::stoi(argv[1]);
    if(argc > 2) mean_length = std::stoi(argv[2]);
    if(argc > 3) range_start_percentage = std::stoi(argv[3]);
    if(argc > 4) range_end_percentage = std::stoi(argv[4]);
    if(argc > 5) range_step_percentage = std::stoi(argv[5]);
    std::cout << "n_records: " << n_records << std::endl;
    std::cout << "mean_length: " << mean_length << std::endl;
    std::cout << "range_start_percentage: " << range_start_percentage << std::endl;
    std::cout << "range_end_percentage: " << range_end_percentage << std::endl;
    std::cout << "range_step_percentage: " << range_step_percentage << std::endl;
}

H5::CompType createEventCompoundType()
{
    H5::CompType data_type(sizeof(LogEvent));
    data_type.insertMember("storyId", HOFFSET(LogEvent, storyId), H5::PredType::NATIVE_UINT64);
    data_type.insertMember("eventTime", HOFFSET(LogEvent, eventTime), H5::PredType::NATIVE_UINT64);
    data_type.insertMember("clientId", HOFFSET(LogEvent, clientId), H5::PredType::NATIVE_UINT32);
    data_type.insertMember("eventIndex", HOFFSET(LogEvent, eventIndex), H5::PredType::NATIVE_UINT32);
    data_type.insertMember("logRecord", HOFFSET(LogEvent, logRecord), H5::VarLenType(H5::PredType::NATIVE_UINT8));
    return data_type;
}

H5::CompType createStoryChunkCompoundType()
{
    H5::CompType data_type(sizeof(StoryChunk));
    data_type.insertMember("storyId", HOFFSET(StoryChunk, storyId), H5::PredType::NATIVE_UINT64);
    data_type.insertMember("startTime", HOFFSET(StoryChunk, startTime), H5::PredType::NATIVE_UINT64);
    data_type.insertMember("endTime", HOFFSET(StoryChunk, endTime), H5::PredType::NATIVE_UINT64);
    data_type.insertMember("revisionTime", HOFFSET(StoryChunk, revisionTime), H5::PredType::NATIVE_UINT64);
    data_type.insertMember("logEvents", HOFFSET(StoryChunk, logEvents), createEventCompoundType());
    return data_type;
}

/**********************************************************************************************************************
 * Data generation functions
 *********************************************************************************************************************/
std::vector <LogEvent> generateEvents(uint64_t &start_timestamp)
{
    hsize_t total_payload_size = 0;
    log_record_bytes.clear();
    total_story_chunk_size = 0;

    // one vector holding the actual data
    log_record_bytes.reserve(n_records);

    // and one holding the hdf5 description and the "simple" columns
    events.reserve(n_records);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution <double> value_dist(0.0, 25.0);
    std::normal_distribution <double> size_dist((double)mean_length, (double)mean_length * 0.1);
//    std::poisson_distribution<hsize_t> size_dist(mean_length);

//    start_timestamp = (uint64_t)std::abs(value_dist(gen)) / 255 * 1000000000;
    for(uint64_t idx = 0; idx < n_records; idx++)
    {
        size_t size = (size_t)std::min(std::max(size_dist(gen), 1.0), static_cast<double>(mean_length) * 3.0);
        log_record_bytes.emplace_back();

        for(hsize_t i = 0; i < size; i++)
        {
            log_record_bytes.at(idx).emplace_back(idx % 26 + 'a');
        }

        // set len and pointer for the variable length descriptor
        uint64_t event_time = start_timestamp + idx * 100;
        uint32_t client_id = 0;
        uint32_t event_index = 0;
        LogEvent event(test_story_id, event_time, client_id, event_index, hvl_t());
        event.logRecord.len = size;
        event.logRecord.p = (void*)&log_record_bytes.at(idx).front();
        events.emplace_back(event);

        // update total_story_chunk_size
        total_story_chunk_size += sizeof(test_story_id) + sizeof(event_time) + sizeof(client_id) + sizeof(event_index);
        total_story_chunk_size += size;
        total_payload_size += size;
    }

    std::cout << "total_story_chunk_size: " << total_story_chunk_size << ", pay_load_size: " << total_payload_size
              << std::endl;

    return events;
}

StoryChunk generateStoryChunk()
{
    uint64_t start_timestamp = test_start_timestamp;
    StoryChunk story_chunk(test_story_id, start_timestamp, start_timestamp + n_records * 1000 + 1);
    size_t event_idx = 0;
    for(auto const &event: events)
    {
        if(story_chunk.insertEvent(event) == 0)
        { LOG_ERROR("Failed to insert {}-th event", event_idx); }
        event_idx++;
    }

    return story_chunk;
}

StoryChunk2 generateStoryChunk2()
{
    uint64_t start_timestamp = test_start_timestamp;
    StoryChunk2 story_chunk(test_story_id, start_timestamp, start_timestamp + n_records * 1000 + 1);
    size_t event_idx = 0;
    for(auto const &event: events)
    {
        if(story_chunk.insertEvent(event) == 0)
        { LOG_ERROR("Failed to insert {}-th event", event_idx); }
        event_idx++;
    }

    return story_chunk;
}

/**********************************************************************************************************************
 * Read/write Events using variable length datatype in HDF5
 *********************************************************************************************************************/
hsize_t writeVlenBytesEvents(H5::H5File*file, std::vector <LogEvent> &data)
{
    try
    {
        /*
         * Create a group in the file
        */
        auto*group = new H5::Group(file->createGroup(group_name));

        hsize_t dim_size = n_records;
        auto*dataspace = new H5::DataSpace(n_dims, &dim_size);

        // target dtype for the file
        H5::CompType data_type = createEventCompoundType();

        auto*dataset = new H5::DataSet(
                file->createDataSet("/" + group_name + "/" + story_chunk_dataset_name + ".vlen_bytes", data_type
                                    , *dataspace));


        dataset->write(&data.front(), data_type);

        delete dataset;
        delete dataspace;
        delete group;

        return data.size();
    }
        // catch failure caused by the H5File operations
    catch(H5::FileIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }

        // catch failure caused by the DataSet operations
    catch(H5::DataSetIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }

        // catch failure caused by the DataSpace operations
    catch(H5::DataSpaceIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }

        // catch failure caused by the DataType operations
    catch(H5::DataTypeIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }
}

int readVlenBytesEvents(H5::H5File*file, std::vector <LogEvent> &data)
{
    try
    {
        H5::DataSet dataset = file->openDataSet("/" + group_name + "/" + story_chunk_dataset_name + ".vlen_bytes");
        H5T_class_t type_class = dataset.getTypeClass();
        if(type_class != H5T_COMPOUND)
        {
            std::cout << "Not a compound type" << std::endl;
            return -1;
        }
        H5::DataSpace dataspace = dataset.getSpace();
        int rank = dataspace.getSimpleExtentNdims();
        hsize_t dims_out[2];
        dataspace.getSimpleExtentDims(dims_out, nullptr);
        std::cout << "rank " << rank << ", dimensions " << (unsigned long)(dims_out[0]) << " x "
                  << (unsigned long)(dims_out[1]) << std::endl;
        H5::CompType defined_comp_type = createEventCompoundType();
        H5::CompType probed_data_type = dataset.getCompType();
        if(probed_data_type.getNmembers() != defined_comp_type.getNmembers())
        {
            std::cout << "Not a compound type with the same #members" << std::endl;
            return -1;
        }
        if(probed_data_type != defined_comp_type)
        {
            std::cout << "Compound type mismatch" << std::endl;
            return -1;
        }
        data.resize(dims_out[0]);
        dataset.read(data.data(), defined_comp_type);
    }
        // catch failure caused by the H5File operations
    catch(H5::FileIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }

        // catch failure caused by the DataSet operations
    catch(H5::DataSetIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }

        // catch failure caused by the DataSpace operations
    catch(H5::DataSpaceIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }

        // catch failure caused by the DataType operations
    catch(H5::DataTypeIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }
    return chronolog::CL_SUCCESS;
}

/**********************************************************************************************************************
 * Read/write StoryChunks using variable length datatype in HDF5
 *********************************************************************************************************************/
hsize_t writeStoryChunkUsingVlenBytesEvents(StoryChunk &story_chunk)
{
    std::vector <LogEvent> data;
    data.reserve(story_chunk.getNumEvents());
    for(auto const &event: story_chunk.logEvents)
    {
        data.push_back(event.second);
    }
    auto*file = new H5::H5File(story_file_name + ".vlen.h5", H5F_ACC_TRUNC);
    writeVlenBytesEvents(file, data);
    file->flush(H5F_SCOPE_GLOBAL);
    hsize_t file_size = file->getFileSize();
    LOG_DEBUG("vlen-bytes file size: {}", file_size);
    delete file;
    return file_size;
}

int readStoryChunkUsingVlenBytesEvents(StoryChunk &story_chunk)
{
    std::vector <LogEvent> data;
    data.reserve(story_chunk.getNumEvents());
    auto*file = new H5::H5File(story_file_name + ".vlen.h5", H5F_ACC_RDONLY);
    readVlenBytesEvents(file, data);
    size_t event_idx = 0;
    for(auto const &event: data)
    {
        if(story_chunk.insertEvent(event) == 0)
        { LOG_ERROR("Failed to insert {}-th event", event_idx); }
        event_idx++;
    }
    return chronolog::CL_SUCCESS;
}

/**********************************************************************************************************************
 * Read/write StoryChunks using blob data and key-value pairs in HDF5
 *********************************************************************************************************************/
hsize_t writeBlob(H5::H5File*file, void*data, const H5::DataType &type, hsize_t size)
{
    try
    {
        auto*group = new H5::Group(file->createGroup(group_name));

        auto*dataspace = new H5::DataSpace(n_dims, &size);

        auto*dataset = new H5::DataSet(
                file->createDataSet("/" + group_name + "/" + story_chunk_dataset_name + ".data", type, *dataspace));

        file->getFileSize();

        dataset->write(data, type);

        delete dataset;
        delete dataspace;
        delete group;

        return size;
    }
        // catch failure caused by the H5File operations
    catch(H5::FileIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }

        // catch failure caused by the DataSet operations
    catch(H5::DataSetIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }

        // catch failure caused by the DataSpace operations
    catch(H5::DataSpaceIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }

        // catch failure caused by the DataType operations
    catch(H5::DataTypeIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return -1;
    }
}

int readBlob(H5::H5File*file, void*data, const H5::DataType &type)
{
    try
    {
        H5::DataSet dataset = file->openDataSet("/" + group_name + "/" + story_chunk_dataset_name + ".data");
        H5::DataSpace dataspace = dataset.getSpace();
        hsize_t dims_out[2];
        dataspace.getSimpleExtentDims(dims_out, nullptr);
        dataset.read(data, type);
        return chronolog::CL_SUCCESS;
    }
        // catch failure caused by the H5File operations
    catch(H5::FileIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }

        // catch failure caused by the DataSet operations
    catch(H5::DataSetIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }

        // catch failure caused by the DataSpace operations
    catch(H5::DataSpaceIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }

        // catch failure caused by the DataType operations
    catch(H5::DataTypeIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }
}

H5::CompType createEventSequenceCompoundType()
{
    H5::CompType data_type(sizeof(StoryChunk2::EventSequence));
    /* FIXME: remove hard-coded offsets */
    data_type.insertMember("eventTime", 0, H5::PredType::NATIVE_UINT64);
    data_type.insertMember("clientId", 8, H5::PredType::NATIVE_UINT32);
    data_type.insertMember("eventIndex", 12, H5::PredType::NATIVE_UINT32);
    return data_type;
}

H5::CompType createEventOffsetSizeCompoundType()
{
    H5::CompType data_type(sizeof(StoryChunk2::EventOffsetSize));
    /* FIXME: remove hard-coded offsets */
    data_type.insertMember("offset", 0, H5::PredType::NATIVE_UINT64);
    data_type.insertMember("size", 8, H5::PredType::NATIVE_UINT64);
    return data_type;
}

H5::CompType createOffsetMapEntryCompoundType()
{
    H5::CompType data_type(sizeof(OffsetMapEntry));
    auto event_sequence_type = createEventSequenceCompoundType();
    auto event_offset_size_type = createEventOffsetSizeCompoundType();
    data_type.insertMember("eventId", HOFFSET(OffsetMapEntry, eventId), event_sequence_type);
    data_type.insertMember("offsetSize", HOFFSET(OffsetMapEntry, offsetSize), event_offset_size_type);
    return data_type;
}

int writeMapAsKVPairs(H5::H5File*file, std::map <StoryChunk2::EventSequence, StoryChunk2::EventOffsetSize> &offsetMap)
{
    try
    {
        hsize_t max_dims = offsetMap.size();
        auto*dataspace = new H5::DataSpace(n_dims, &max_dims);
        auto offset_dtype = createOffsetMapEntryCompoundType();
        auto*dataset = new H5::DataSet(
                file->createDataSet("/" + group_name + "/" + story_chunk_dataset_name + ".meta", offset_dtype
                                    , *dataspace));
        std::vector <OffsetMapEntry> offsetMapEntries;
        offsetMapEntries.reserve(offsetMap.size());
        for(auto const &entry: offsetMap)
        {
            offsetMapEntries.emplace_back(entry.first, entry.second);
        }
        dataset->write(&offsetMapEntries.front(), offset_dtype);
        delete dataset;
        delete dataspace;
        return chronolog::CL_SUCCESS;
    }
    catch(H5::Exception &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        return chronolog::CL_ERR_UNKNOWN;
    }
}

int readMapAsKVPairs(std::map <StoryChunk2::EventSequence, StoryChunk2::EventOffsetSize> &offsetMap, H5::H5File*file)
{
    try
    {
        H5::DataSet dataset = file->openDataSet("/" + group_name + "/" + story_chunk_dataset_name + ".meta");
        H5T_class_t type_class = dataset.getTypeClass();
        if(type_class != H5T_COMPOUND)
        {
            std::cout << "Not a compound type" << std::endl;
            return -1;
        }
        H5::DataSpace dataspace = dataset.getSpace();
        hsize_t dims_out[2];
        dataspace.getSimpleExtentDims(dims_out, nullptr);
        H5::CompType probed_data_type = dataset.getCompType();
        if(probed_data_type.getNmembers() != 2)
        {
            std::cout << "Not a compound type with 2 members" << std::endl;
            return -1;
        }
        H5::CompType defined_comp_type = createOffsetMapEntryCompoundType();
        if(probed_data_type != defined_comp_type)
        {
            std::cout << "Compound type mismatch" << std::endl;
            return -1;
        }
        std::vector <OffsetMapEntry> offsetMapEntries;
        offsetMapEntries.resize(dims_out[0]);
        dataset.read(offsetMapEntries.data(), defined_comp_type);
        for(auto const &entry: offsetMapEntries)
        {
            offsetMap.insert({entry.eventId, entry.offsetSize});
        }
        return chronolog::CL_SUCCESS;
    }
    catch(H5::Exception &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        return chronolog::CL_ERR_UNKNOWN;
    }
}

hsize_t writeStoryChunkUsingBlobAndKVPairs(StoryChunk2 &story_chunk)
{
    try
    {
        auto*file = new H5::H5File(story_file_name + ".blob+map.h5", H5F_ACC_TRUNC);
        writeBlob(file, story_chunk.eventData, H5::PredType::NATIVE_UINT8, story_chunk.totalSize);
        writeMapAsKVPairs(file, story_chunk.offsetSizeMap);
        file->flush(H5F_SCOPE_GLOBAL);
        hsize_t file_size = file->getFileSize();
        LOG_DEBUG("blob+map file size: {}", file_size);
        delete file;
        return file_size;
    }
    catch(H5::Exception &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        return chronolog::CL_ERR_UNKNOWN;
    }
}

int readStoryChunkUsingBlobAndKVPairs(StoryChunk2 &story_chunk)
{
    try
    {
        auto*file = new H5::H5File(story_file_name + ".blob+map.h5", H5F_ACC_RDONLY);
        H5::Group group = file->openGroup(group_name);
        readMapAsKVPairs(story_chunk.offsetSizeMap, file);
        readBlob(file, story_chunk.eventData, H5::PredType::NATIVE_UINT8);
        delete file;
        uint64_t total_size = 0, total_events = 0;
        for(auto const &entry: story_chunk.offsetSizeMap)
        {
            total_size += std::get <1>(entry.second);
//            total_size += entry.second.size;
            total_events++;
        }
        story_chunk.totalSize = total_size;
        story_chunk.totalEventCount = total_events;
        story_chunk.currentOffset = total_size;
        return chronolog::CL_SUCCESS;
    }
    catch(H5::Exception &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        return chronolog::CL_ERR_UNKNOWN;
    }
}

int rangeQuery(uint64_t start_time, uint64_t end_time, StoryChunk2 &res_story_chunk, void*res_data_blob)
{
    try
    {
        auto*file = new H5::H5File(story_file_name + ".blob+map.h5", H5F_ACC_RDONLY);
        H5::Group group = file->openGroup(group_name);
        H5::DataSet dataset = file->openDataSet("/" + group_name + "/" + story_chunk_dataset_name + ".data");
        H5::DataSpace dataspace = dataset.getSpace();
        std::map <StoryChunk2::EventSequence, StoryChunk2::EventOffsetSize> offsetSizeMap;

        // read out the entire offsetSizeMap
        readMapAsKVPairs(offsetSizeMap, file);

        // get the lower and upper iterators for the range query
        // FIXME: what if the clientId and eventIdx are not 0?
        auto lower_it = offsetSizeMap.lower_bound(StoryChunk2::EventSequence{start_time, 0, 0});
        auto upper_it = offsetSizeMap.upper_bound(StoryChunk2::EventSequence{end_time, 0, 0});
//        auto start_idx = std::distance(offsetSizeMap.begin(), lower_it);
//        auto end_idx = std::distance(offsetSizeMap.begin(), upper_it);
//        auto range_size = std::distance(lower_it, upper_it);

        // calculate the total size of the result event payloads
        uint64_t range_data_size = 0;
        for(auto it = lower_it; it != upper_it; it++)
        {
            auto event_payload_len = std::get <1>(it->second);
//            LOG_DEBUG("event_payload_len of result event {}: {}", range_offset++, event_payload_len);
            range_data_size += event_payload_len;
        }
        LOG_DEBUG("Total size of result events: {}", range_data_size);

        // read out the result event payloads
        res_data_blob = malloc(range_data_size);
        hsize_t start_offset = std::get <0>(lower_it->second);
        hsize_t data_size = range_data_size;
//        LOG_DEBUG("Total storage size of dataset: {}", dataset.getStorageSize());
//        hsize_t ndims = dataspace.getSimpleExtentNdims();
//        LOG_DEBUG("Dataspace ndims: {}", ndims);
//        hsize_t dims[2] = {0, 0};
//        dataspace.getSimpleExtentDims(dims, nullptr);
//        LOG_DEBUG("Dataspace dims: {} x {}", dims[0], dims[1]);

        // select file hyperslab
        LOG_DEBUG("Selecting hyperslab of size {} starting from offset {} ...", data_size, start_offset);
        dataspace.selectHyperslab(H5S_SELECT_SET, &data_size, &start_offset);

        // select memory hyperslab
        hsize_t start[2] = {0, 0}, end[2] = {0, 0};
        dataspace.getSelectBounds(start, end);
        hsize_t hyperslab_dims[2] = {end[0] - start[0] + 1, end[1] - start[1] + 1};
        LOG_DEBUG("Hyperslab dims: {} x {}", hyperslab_dims[0], hyperslab_dims[1]);
        H5::DataSpace memspace(n_dims, hyperslab_dims);

        // read
        dataset.read(res_data_blob, H5::PredType::NATIVE_UINT8, memspace, dataspace);

        // fill in the result event vector
        std::map <StoryChunk2::EventSequence, StoryChunk2::EventOffsetSize> res_offsetSizeMap;
        for(auto it = lower_it; it != upper_it; it++)
        {
            uint64_t offset = std::get <0>(it->second) - start_offset;
            uint64_t size = std::get <1>(it->second);
            LogEvent event;
            event.storyId = test_story_id;
            event.eventTime = std::get <0>(it->first);
            event.clientId = std::get <1>(it->first);
            event.eventIndex = std::get <2>(it->first);
            event.logRecord.p = (void*)((uint8_t*)res_data_blob + offset);
            event.logRecord.len = size;
            res_story_chunk.insertEvent(event);
        }
        return chronolog::CL_SUCCESS;
    }
    catch(H5::Exception &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        return chronolog::CL_ERR_UNKNOWN;
    }
}

/**********************************************************************************************************************
 * Utils
 *********************************************************************************************************************/
void writeEventVectorToFile(std::vector <LogEvent> &data, const std::string &filename)
{
    std::ofstream outputFile(filename); // Open the file for writing

    for(const auto &entry: data) // Iterate over the vector
    {
        outputFile << entry.toString(); // Write each element
        outputFile << "\n";
    }

    outputFile.close(); // Close the file
}

template <typename T>
void writeStoryChunkToFile(T &story_chunk, const std::string &filename)
{
    std::ofstream outputFile(filename); // Open the file for writing

    outputFile << story_chunk.toString();

    outputFile.close(); // Close the file
}

/**********************************************************************************************************************
 * Read/write StoryChunks using serialized JSON strings in HDF5
 *********************************************************************************************************************/
std::string serializeStoryChunk(StoryChunk &story_chunk)
{
    json_object*obj = json_object_new_object();
    std::string story_chunk_name =
            std::to_string(story_chunk.getStartTime()) + "_" + std::to_string(story_chunk.getEndTime());
    std::string story_chunk_str = story_chunk.toString();
    json_object*story_chunk_json_array = json_object_new_array();
    for(unsigned long i = 0; i < story_chunk_str.size() / 4; i++)
    {
        json_object_array_add(story_chunk_json_array, json_object_new_int(story_chunk_str.c_str()[i]));
    }
    json_object_object_add(obj, story_chunk_name.c_str(), story_chunk_json_array);
    std::cout << "string size in serializeStoryChunk(): " << strlen(json_object_to_json_string(obj)) << std::endl;
    return {json_object_to_json_string(obj)};
}

StoryChunk deserializeStoryChunk(char*story_chunk_json_str, uint64_t &story_id, uint64_t start_time, uint64_t end_time)
{
    struct json_object*obj = json_tokener_parse(story_chunk_json_str);
    StoryChunk story_chunk(story_id, start_time, end_time);
    enum json_type type = json_object_get_type(obj);
    if(type == json_type_object)
    {
        json_object_object_foreach(obj, key, val)
        {
            // get event_time, client_id, index from key
            // for example, get event_time = 1, client_id = 2, index = 3 from key = "[ 1, 2, 3 ]"
            std::string key_str = std::string(key);
            // Find the positions of the commas and brackets in the key
            size_t comma_pos1 = key_str.find(',');
            size_t comma_pos2 = key_str.find(',', comma_pos1 + 1);
            size_t closing_bracket_pos = key_str.find(']');

            // Extract the values between the commas and brackets
            uint64_t event_time = std::stoull(key_str.substr(2, comma_pos1 - 2));
            uint32_t client_id = std::stoul(key_str.substr(comma_pos1 + 2, comma_pos2 - comma_pos1 - 2));
            uint32_t index = std::stoul(key_str.substr(comma_pos2 + 2, closing_bracket_pos - comma_pos2 - 2));

//            LOG_DEBUG("val: {}", json_object_get_string(val));
            hvl_t log_record;
            log_record.p = (void*)json_object_get_string(val);
            log_record.len = strlen(json_object_get_string(val));
            LogEvent event(story_id, event_time, client_id, index, log_record);
            if(story_chunk.insertEvent(event) == 0)
            { LOG_ERROR("Failed to insert event"); }
//            LOG_DEBUG("#Events: {}", story_chunk.getNumEvents());
        }
    }
    else
    {
        LOG_ERROR("Failed to parse story_chunk_json_str: {}", story_chunk_json_str);
        exit(chronolog::CL_ERR_UNKNOWN);
    }
//    LOG_DEBUG("#Events: {}", story_chunk.getNumEvents());
    return story_chunk;
}

hsize_t writeStoryChunkInJSON(StoryChunk &story_chunk)
{
    try
    {
        auto*file = new H5::H5File(ref_json_file_name, H5F_ACC_TRUNC);

        auto*group = new H5::Group(file->createGroup(group_name));

        std::string story_chunk_json_str = serializeStoryChunk(story_chunk);
//        LOG_DEBUG("size of story_chunk_json_str: {}", story_chunk_json_str.size());

        hsize_t dims[] = {1};
        auto*dataspace = new H5::DataSpace(n_dims, dims);

        H5::StrType datatype(H5::PredType::C_S1, H5T_VARIABLE);
        H5::DataSet dataset = file->createDataSet("/" + group_name + "/" + story_chunk_dataset_name + ".json", datatype
                                                  , *dataspace);

        dataset.write(story_chunk_json_str, datatype);

        file->flush(H5F_SCOPE_GLOBAL);
        hsize_t file_size = file->getFileSize();

        delete dataspace;
        delete group;
        delete file;

        return file_size;
    }
        // catch failure caused by the H5File operations
    catch(H5::FileIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }

        // catch failure caused by the DataSet operations
    catch(H5::DataSetIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }

        // catch failure caused by the DataSpace operations
    catch(H5::DataSpaceIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }

        // catch failure caused by the DataType operations
    catch(H5::DataTypeIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }
}

int readStoryChunkInJSON(StoryChunk &story_chunk)
{
    try
    {
        H5::H5File file = H5::H5File(ref_json_file_name, H5F_ACC_RDONLY);
        H5::DataSet dataset = file.openDataSet("/" + group_name + "/" + story_chunk_dataset_name + ".json");
        H5::DataSpace dataspace = dataset.getSpace();
        hsize_t dims_out[2];
        dataspace.getSimpleExtentDims(dims_out, nullptr);
        std::string story_chunk_json_str;
        story_chunk_json_str.resize(dims_out[0]);
        H5::StrType datatype(H5::PredType::C_S1, H5T_VARIABLE);
        dataset.read(story_chunk_json_str, datatype);
        uint64_t story_id = test_story_id;
        story_chunk = deserializeStoryChunk((char*)story_chunk_json_str.c_str(), story_id, 0, 0);
        return chronolog::CL_SUCCESS;
    }
        // catch failure caused by the H5File operations
    catch(H5::FileIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }

        // catch failure caused by the DataSet operations
    catch(H5::DataSetIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }

        // catch failure caused by the DataSpace operations
    catch(H5::DataSpaceIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }

        // catch failure caused by the DataType operations
    catch(H5::DataTypeIException &error)
    {
        std::cout << error.getCDetailMsg() << std::endl;
        H5::FileIException::printErrorStack();
        return chronolog::CL_ERR_UNKNOWN;
    }
}

int main(int argc, char*argv[])
{
    int result = chronolog::chrono_monitor::initialize("console", "cmp_vlen_bytes_vs_blob_map.log", spdlog::level::debug
                                                       , "cmp_vlen_bytes_vs_blob_map", 102400, 1, spdlog::level::debug);
    if(result == 1)
    {
        exit(EXIT_FAILURE);
    }

    parseCommandLineOptions(argc, argv);

    std::chrono::time_point <std::chrono::high_resolution_clock> start, end;
    uint64_t file_size;
    long duration_ns;
    double duration_s;
    double storage_efficiency;

    /*
     * Generate events
     */
    uint64_t start_timestamp = test_start_timestamp;
    generateEvents(start_timestamp);

    /*
     * Write/read events using vlen type
     */
    {
        StoryChunk wdata = generateStoryChunk();

        // measure time of writeVlenBytesEvents
        std::cout << "======================================================================================="
                  << std::endl;
        start = std::chrono::high_resolution_clock::now();
        file_size = writeStoryChunkUsingVlenBytesEvents(wdata);
        end = std::chrono::high_resolution_clock::now();
        storage_efficiency = (double)total_story_chunk_size / (double)file_size;
        duration_ns = std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count();
        duration_s = (double)duration_ns / 1e9;
        std::cout << std::endl << "Storage efficiency of events: " << storage_efficiency * 100 << "%" << std::endl;
        std::cout << std::endl << "Time taken to write events: " << duration_s << " seconds" << std::endl
                  << "Write bandwidth: " << ((double)total_story_chunk_size / (1e+6 * duration_s)) << " MB/second"
                  << std::endl;

        StoryChunk rdata(test_story_id, test_start_timestamp, test_start_timestamp + n_records * 1000 + 1);
        // measure time of readVlenBytesEvents
        start = std::chrono::high_resolution_clock::now();
        readStoryChunkUsingVlenBytesEvents(rdata);
        end = std::chrono::high_resolution_clock::now();
        duration_ns = std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count();
        duration_s = (double)duration_ns / 1e9;
        std::cout << std::endl << "Time taken to read events: " << duration_s / 1e9 << " seconds" << std::endl
                  << "Read bandwidth: " << ((double)total_story_chunk_size / (1e+6 * duration_s)) << " MB/second"
                  << std::endl;

        if(wdata != rdata)
        {
            std::cerr << "\n\n" << "Writing events using vlen-bytes datatype: Data mismatch" << "\n\n" << std::endl;
            std::cout << "Data written:" << wdata.toString() << std::endl;
            std::cout << "Data read:" << rdata.toString() << std::endl;
            writeStoryChunkToFile(wdata, "wdata.txt");
            writeStoryChunkToFile(rdata, "rdata.txt");
//        return -1;
        }
    }

    /*
     * Write/read events data as blob and metadata as key-value pairs
     */
    {
        /*
         * Full chunk write and read
         */
        max_story_chunk2_size = n_records * mean_length * 2;
        StoryChunk2 wdata2 = generateStoryChunk2();

        // measure time of write blob+map
        std::cout << "======================================================================================="
                  << std::endl;
        start = std::chrono::high_resolution_clock::now();
        file_size = writeStoryChunkUsingBlobAndKVPairs(wdata2);
        end = std::chrono::high_resolution_clock::now();
        duration_ns = std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count();
        duration_s = (double)duration_ns / 1e9;
        storage_efficiency = (double)total_story_chunk_size / (double)file_size;
        std::cout << std::endl << "Storage efficiency of blob+map: " << storage_efficiency * 100 << "%" << std::endl;
        std::cout << std::endl << "Time taken to write blob+map: " << duration_s / 1e9 << " seconds" << std::endl
                  << "Write bandwidth: " << ((double)total_story_chunk_size / (1e+6 * duration_s)) << " MB/second"
                  << std::endl;

        StoryChunk2 rdata2(test_story_id, test_start_timestamp, test_start_timestamp + n_records * 1000 + 1);
        start = std::chrono::high_resolution_clock::now();
        readStoryChunkUsingBlobAndKVPairs(rdata2);
        end = std::chrono::high_resolution_clock::now();
        duration_ns = std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count();
        duration_s = (double)duration_ns / 1e9;
        std::cout << std::endl << "Time taken to read blob: " << duration_s / 1e9 << " seconds" << std::endl
                  << "Read bandwidth: " << ((double)total_story_chunk_size / (1e+6 * duration_s)) << " MB/second"
                  << std::endl;

        if(wdata2 != rdata2)
        {
            std::cerr << "\n\n" << "Writing events as blob+metadata kv-pairs: Data mismatch" << "\n\n" << std::endl;
//        std::cout << "Data written:" << wdata2.toString() << std::endl;
//        std::cout << "Data read:" << rdata2.toString() << std::endl;
            writeStoryChunkToFile(wdata2, "wdata2.txt");
            writeStoryChunkToFile(rdata2, "rdata2.txt");
//        return -1;
        }

        /*
         * Range query
         */
//        size_t idx = 0;
//        for(auto const &event: events)
//        {
//            LOG_DEBUG("Raw event {}: {}", idx++, event.toString().c_str());
//        }
        for(auto start_percent = range_start_percentage, end_percent = range_start_percentage + range_step_percentage;
            end_percent <= range_end_percentage; end_percent += range_step_percentage)
        {
            std::cout << "======================================================================================="
                      << std::endl;
            StoryChunk2 res_story_chunk(test_story_id, test_start_timestamp,
                    test_start_timestamp + n_records * 1000 + 1);
            auto range_event_time_start_offset =
                    (events.back().eventTime - events.front().eventTime) * start_percent / 100;
            auto range_event_time_end_offset = (events.back().eventTime - events.front().eventTime) * end_percent / 100;
            auto range_start_time = static_cast<uint64_t>(test_start_timestamp + range_event_time_start_offset);
            auto range_end_time = static_cast<uint64_t>(test_start_timestamp + range_event_time_end_offset);
            LOG_INFO("Range query: start_percent: {}%%, end_percent: {}%%", start_percent, end_percent);
            LOG_INFO("Range query: start_time: {}, end_time: {}", range_start_time, range_end_time);

            // calculate the expected result StoryChunk2
            auto start_idx = static_cast<int>(events.size() * start_percent / 100);
            auto end_idx = static_cast<int>(events.size() * end_percent / 100);
            LOG_DEBUG("Expected result events range from {} to {}", start_idx, end_idx);
            StoryChunk2 expected_res_story_chunk(test_story_id, test_start_timestamp,
                    test_start_timestamp + n_records * 1000 + 1);
            uint64_t total_res_payload_size = 0;
            for(auto i = start_idx; i < end_idx; i++)
            {
                expected_res_story_chunk.insertEvent(events[i]);
                total_res_payload_size += events[i].logRecord.len;
            }
            LOG_DEBUG("Total size of expected result event payloads: {}", total_res_payload_size);
            void*res_data_blob = nullptr;

            // test rangeQuery
            start = std::chrono::high_resolution_clock::now();
            rangeQuery(range_start_time, range_end_time, res_story_chunk, res_data_blob);
            end = std::chrono::high_resolution_clock::now();
            duration_ns = std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count();
            duration_s = (double)duration_ns / 1e9;
            std::cout << std::endl << "Time taken to range query: " << duration_s / 1e9 << " seconds" << std::endl
                      << "Read bandwidth: " << ((double)total_res_payload_size / (1e+6 * duration_s)) << " MB/second"
                      << std::endl;
            if(res_story_chunk != expected_res_story_chunk)
            {
                std::cerr << "\n\n" << "Range query: Data mismatch" << "\n\n" << std::endl;
                writeStoryChunkToFile(expected_res_story_chunk, "expected_res_story_chunk.txt");
                writeStoryChunkToFile(res_story_chunk, "res_story_chunk.txt");
                free(res_data_blob);
            }
        }
    }

    /*
     * Write/read StoryChunk in JSON
     */
    /*
    std::cout << "=======================================================================================" << std::endl;
    StoryChunk story_chunk = generateStoryChunk();
    start = std::chrono::high_resolution_clock::now();
    file_size = writeStoryChunkInJSON(story_chunk);
    end = std::chrono::high_resolution_clock::now();
    duration_ns = std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count();
    duration_s = (double)duration_ns / 1e9;
    storage_efficiency = (double)total_story_chunk_size / (double)file_size;
    std::cout << std::endl << "Storage efficiency of JSON: " << storage_efficiency * 100 << "%" << std::endl;
    std::cout << std::endl << "Time taken to write StoryChunk in JSON: " << duration_s / 1e9 << " seconds" << std::endl
              << "Write bandwidth: " << ((double)total_story_chunk_size / (1e+6 * duration_s)) << " MB/second"
              << std::endl;

    StoryChunk story_chunk2;
    start = std::chrono::high_resolution_clock::now();
    readStoryChunkInJSON(story_chunk2);
    end = std::chrono::high_resolution_clock::now();
    duration_ns = std::chrono::duration_cast <std::chrono::nanoseconds>(end - start).count();
    duration_s = (double)duration_ns / 1e9;
    std::cout << std::endl << "Time taken to read StoryChunk in JSON: " << duration_s / 1e9 << " seconds" << std::endl
              << "Read bandwidth: " << ((double)total_story_chunk_size / (1e+6 * duration_s)) << " MB/second"
              << std::endl;
    */

    return 0;
}