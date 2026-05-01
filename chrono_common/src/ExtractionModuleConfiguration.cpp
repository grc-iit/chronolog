
#include <json-c/json.h>
#include <iostream>

#include <ExtractionModuleConfiguration.h>


namespace chl = chronolog;

chronolog::ExtractionModuleConfiguration::~ExtractionModuleConfiguration()
{
  //clear old extractors map
   for(auto iter = extractors.begin(); iter!= extractors.end();++iter)
   {
       json_object_put((*iter).second);
   }

   extractors.clear();
}

///////

int chronolog::ExtractionModuleConfiguration::parse_json_object(json_object * json_block)
{

   if(json_block == nullptr || !json_object_is_type(json_block, json_type_object))
   {
        return chl::CL_ERR_INVALID_CONF;
   }

  //clear old extractors map

  for(auto iter = extractors.begin(); iter!= extractors.end();++iter)
  {
   json_object_put((*iter).second);
  }

  extractors.clear();

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
        else if(strcmp(key, "extraction_protocol") == 0)
        {
            if(!json_object_is_type(json_val, json_type_string))
            {
                std::cerr << "[ExtractionConfiguration] Invalid 'extraction_protocol': expected json_object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            extraction_protocol = json_object_get_string(json_val);
        }
        else if(strcmp(key, "extractors") == 0)
        {
            if( (json_val == nullptr)
            || !json_object_is_type(json_val, json_type_object))
            {
                std::cerr << "[ExtractionModuleConfiguration] Invalid 'extractors': expected json_object" << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }

            json_object_object_foreach(json_val, key, val)
            {
                    if(!json_object_is_type(val, json_type_object))
                    {
                       std::cerr << "[ExtractionConfiguration] Invalid 'extractor': expected json_object" << std::endl;
                       return chl::CL_ERR_INVALID_CONF;
                    }
                    else if( !json_object_is_type(json_object_object_get(val, "type") , json_type_string) )
                    {
                       std::cerr << "[ExtractionConfiguration] Invalid 'extractor_type' " << std::endl;
                       return chl::CL_ERR_INVALID_CONF;
                    }
                    else
                    {
                       std::string extractor_type = json_object_get_string(json_object_object_get(val, "type"));
                       //retrieve_extractor json_object and increment its reference count
                       json_object* extractor_json_object = json_object_object_get(json_val, key);
                       extractors[extractor_type] = json_object_get(extractor_json_object);
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


