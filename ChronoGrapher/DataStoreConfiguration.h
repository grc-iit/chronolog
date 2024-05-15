#ifndef DATA_STORE_CONFIGURATION_H
#define DATA_STORE_CONFIGURATION_H

#include <string>
#include <iostream>
#include <json-c/json.h>

namespace chronolog
{

class ConfigurationSegment
{
public:
    ConfigurationSegment()
    { } 
    ConfigurationSegment(json_object &)
    { }

    virtual std::string const & getJsonKey() const = 0;    
    virtual int load(json_object &) = 0;
    virtual std::string & to_string(std::string &) = 0;
    virtual std::ostream & to_stream(std::ostream &) = 0;
};
    
class DataStoreConfiguration: public ConfigurationSegment
{
public:
    DataStoreConfiguration();
    DataStoreConfiguration(json_object &);
    ~DataStoreConfiguration() = default;

    virtual std::string const & getJsonKey() const;  
    virtual int load(json_object &);
    virtual std::string & to_string(std::string &);
    virtual std::ostream & to_stream(std::ostream &);

private:
    uint32_t max_story_chunk_size = 65536;  // 2^16 = 65536 
    uint32_t story_chunk_time_granularity = 30;
    uint32_t story_chunk_access_window = 60;
    uint32_t story_pipeline_exit_window = 300;

    std::string csv_file_extraction_dir = "/tmp/";
};

};// namespace chronolog


#endif
