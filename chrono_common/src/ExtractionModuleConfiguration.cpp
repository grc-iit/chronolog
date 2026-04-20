
#include <json-c/json.h>
#include <iostream>

#include <ExtractionModuleConfiguration.h>


namespace chl = chronolog;

int chronolog::ExtractionModuleConfiguration::parse_json_object(json_object * json_block)
{
   if(json_block == nullptr || !json_object_is_type(json_block, json_type_object))
   {
        return chl::CL_ERR_INVALID_CONF;
   }

    json_object_object_foreach(json_block, key, json_val)
    {
        
       if(strcmp(key, "extraction_stream_count") == 0)
        {
            if(!json_object_is_type(json_val, json_type_int))
            {
                std::cerr << "[ExtractionConfiguration] Invalid 'extraction_stream_count': expected json_object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            int value = json_object_get_int(json_val);
            extraction_stream_count = (value >= 0 ? value : 0);
            if(extraction_stream_count == 0)
            {
                std::cerr << "[ExtractionModuleConfiguration] Invalid 'extraction_stream_count': expected integer " << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
        }
        else if(strcmp(key, "extractors") == 0)
        {
            if(!json_object_is_type(json_val, json_type_object))
            {
                std::cerr << "[ExtractionModuleConfiguration] Invalid 'extractors': expected json_object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            json_object* extractors_conf = json_object_object_get(json_block, "extractors");
            json_object_object_foreach(extractors_conf, key, val)
            {
                if(strcmp(key, "extractor") == 0)
                {
                    if(!json_object_is_type(val, json_type_object))
                    {
                       std::cerr << "[ExtractionConfiguration] Invalid 'extractor': expected json_object" << std::endl;
                       return chl::CL_ERR_INVALID_CONF;
                    }
                    else if( !json_object_is_type(json_object_object_get(val, "type") , json_type_string) )
                    {
                       std::cerr << "[ExtractionConfiguration] Invalid 'extractor_type' " << std::endl;
                    }
                    else
                    {
                       std::string extractor_type = json_object_get_string(json_object_object_get(val, "type"));
                       extractors[extractor_type] = val;
                    }
                }
                else
                {
                    std::cerr << "[ExtractionModuleConfiguration] Unknown 'extractors' configuration: " << key << std::endl;
                return chl::CL_ERR_INVALID_CONF;
                }
            }

        }
        else
        {
            std::cerr << "[ExtractionModuleConfiguration] Unknown configuration block: " << key << std::endl;
            return chl::CL_ERR_INVALID_CONF;
        }
      }
        
   return chronolog::CL_SUCCESS;
}

