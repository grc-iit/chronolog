
#include <string>
#include <vector>
#include <fstream>

#include <chrono_monitor.h>
#include <chronolog_errcode.h>
#include <ExtractionChainConfiguration.h>

namespace chl = chronolog;

chl::ExtractionChainConfiguration::ExtractionChainConfiguration(std::string const & conf_file_path)
{
    json_object* root = json_object_from_file(conf_file_path.c_str());
    if(root == nullptr)
    {
        LOG_ERROR( "[ExtractionChainConfiguration] Error while parsing configuration file {}", conf_file_path);
        return;
    }

    json_object* extraction_chain_conf = nullptr;
    json_object_object_foreach(root, key, val)
    {
        if(strcmp(key, "ExtractionChain") == 0)
        {
            json_object* extraction_chain_conf = json_object_object_get(root, "ExtractionChain");
            break;
        }
    }

    parse_json_conf(extraction_chain_conf);
}

////////////////

chl::ExtractionChainConfiguration::ExtractionChainConfiguration(json_object* extraction_chain_conf)
{
    parse_json_conf(extraction_chain_conf);
}
////////////////

int chl::ExtractionChainConfiguration::parse_json_conf(json_object* extraction_chain_conf)
{
    if(extraction_chain_conf != nullptr || json_object_is_type(extraction_chain_conf, json_type_object))
    {
        parse_json_conf(extraction_chain_conf);
    }
    else
    {
        LOG_ERROR( "[ExtractionChainConfiguration] Error parsing configuration. No valid ExtractionChain object found.");
    }
    

    return chl::CL_SUCCESS;
}
//////////////////

bool chl::ExtractionChainConfiguration::is_valid() const
{
    bool return_value = true;

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

