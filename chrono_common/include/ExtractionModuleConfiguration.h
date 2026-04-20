#ifndef CHRONO_EXTRACTION_MODULE_CONFIGURATION
#define CHRONO_EXTRACTION_MODULE_CONFIGURATION


#include <json-c/json.h>
#include <iostream>
#include <map>

#include <ConfigurationBlocks.h>

namespace chronolog
{

struct ExtractionModuleConfiguration
{
    int extraction_stream_count = 1;
    std::map<std::string, json_object*> extractors;

    std::string & to_string( std::string & a_string) const
    {
        a_string += "\n[EXTRACTION_MODULE_CONF: { extraction_stream_count: " + std::to_string(extraction_stream_count)
                    + "} extractors {";
        for( auto iter = extractors.begin(); iter != extractors.end(); ++iter)
        {    a_string += " {"+ (*iter).first +  ":" /*+( (*iter).second == nullptr? "nullptr" : std::to_string((*iter).second)) */+"}"; }

        a_string += "} ]";

    return a_string;
    }

    int parse_json_object(json_object * json_object);
};

}

#endif
