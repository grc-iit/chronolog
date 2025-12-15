
#include <string>
#include <vector>
#include <fstream>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <ExtractionModuleConfiguration.h>

namespace chl = chronolog;

chl::ExtractionModuleConfiguration::ExtractionModuleConfiguration(std::string const & conf_file_path)
    : extraction_thread_number(0)
{
    json_object* root = json_object_from_file(conf_file_path.c_str());
    if(root == nullptr)
    {
        LOG_ERROR( "[ExtractionModuleConfiguration] Error while parsing configuration file {}", conf_file_path);
        return;
    }

    json_object* extraction_conf = nullptr;
    json_object_object_foreach(root, key, val)
    {
        if(strcmp(key, "ExtractionModule") == 0)
        {
            json_object* extraction_conf = json_object_object_get(root, "ExtractionModule");
            break;
        }
    }

    parse_json_conf(extraction_conf);
}

////////////////

chl::ExtractionModuleConfiguration::ExtractionModuleConfiguration(json_object* extraction_conf)
    : extraction_thread_number(0)
{
    parse_json_conf(extraction_conf);
}
////////////////

int chl::ExtractionModuleConfiguration::parse_json_conf(json_object* extraction_conf)
{
    if(extraction_conf == nullptr || !json_object_is_type(extraction_conf, json_type_object))
    {
        LOG_ERROR( "[ExtractionModuleConfiguration] Error parsing configuration. No valid ExtractionChain object found.");
        return chl::CL_ERR_INVALID_CONF;
    }
    
    //TODO parsing logic goes here

    return chl::CL_SUCCESS;
}
//////////////////

int chl::ExtractionModuleConfiguration::update_config(std::string const & conf_file_path)
{
    extractors.clear();

   json_object* root = json_object_from_file(conf_file_path.c_str());
    if(root == nullptr)
    {
        LOG_ERROR( "[ExtractionModuleConfiguration] Error while parsing configuration file {}", conf_file_path);
        return chl::CL_ERR_INVALID_CONF;
    }

    json_object* extraction_conf = nullptr;
    json_object_object_foreach(root, key, val)
    {
        if(strcmp(key, "ExtractionModule") == 0)
        {
            json_object* extraction_conf = json_object_object_get(root, "ExtractionModule");
            break;
        }
    }

    return parse_json_conf(extraction_conf);
}
//////////////////

int chl::ExtractionModuleConfiguration::update_config(json_object * extraction_conf)
{
    extractors.clear();
    return parse_json_conf(extraction_conf);
}

//////////////////

bool chl::ExtractionModuleConfiguration::is_valid() const
{
    bool return_value = true;

    if(extraction_thread_number == 0 || extractors.empty())
    { return false; }

    for( auto & extractor_conf : extractors)
    {
        if( !extractor_conf.is_valid())
        {
            return_value = false;
            break;
        }
    }

    return return_value;
}
/////////////////////////

std::string & chl::ExtractionModuleConfiguration::to_string( std::string & a_string) const
{
    a_string += "{ ExtractionModuleConfiguration {extraction_thread_count : " + std::to_string(extraction_thread_number)+ "}{extractors: ";
    
    for ( auto & extractor_conf : extractors )
        a_string += extractor_conf.to_string(a_string);       
 
    a_string += " } }";

    return a_string;
}

