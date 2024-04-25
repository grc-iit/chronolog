#ifndef DATA_STORE_CONFIGURATION_H


namespace chronolog
{

class DataStoreConfiguration: public ConfigurationSegment
{
public:
    DataStoreConfiguration();
    DataStoreConfiguration();
    ~DataStoreConfiguration() = default;
    int load(json_object&);
    std::string& to_string(std::string&);
    std::ostream& to_stream(std::ostream&);

    uint32_t story_chunk_time_granularity = 30;
    uint32_t max_story_chunk_size = 1024;
    uint32_t story_chunk_access_window = 60;
};

};// namespace chronolog


#endif
