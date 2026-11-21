
#include <json-c/json.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

#include <ExtractionModuleConfiguration.h>


namespace chl = chronolog;

chronolog::ExtractionModuleConfiguration::ExtractionModuleConfiguration(ExtractionModuleConfiguration const& source)
    : ExtractionModuleConfiguration()
{
    extraction_stream_count = source.extraction_stream_count;
    extraction_protocol = source.extraction_protocol;
    for(auto const& extractor: source.extractors)
    {
        std::unique_ptr<json_object, decltype(&json_object_put)> owned(nullptr, &json_object_put);
        if(extractor.second)
        {
            // Configurations cross worker lifetimes. Own an independent JSON
            // value, including its children: the installed json-c reference
            // counters are not atomic. Userdata belongs to the source owner.
            json_object* cloned = nullptr;
            auto copyValue = [](json_object* source,
                                json_object* parent,
                                char const* key,
                                size_t index,
                                json_object** destination)
            {
                int status = json_c_shallow_copy_default(source, parent, key, index, destination);
                if(status < 0)
                    return status;
                // Parsed doubles may carry a serializer using source userdata.
                // Reset the clone to value serialization without touching the
                // source cache or copying its ownership/deletion callbacks.
                json_object_set_serializer(*destination, nullptr, nullptr, nullptr);
                return 2; // json-c must not attempt its userdata-copy phase.
            };
            int status = json_object_deep_copy(extractor.second, &cloned, copyValue);
            owned.reset(cloned); // Also owns a partial clone on checked failure.
            if(status != 0 || !owned)
                throw std::runtime_error("Cannot own extractor configuration copy");
        }
        extractors.emplace(extractor.first, owned.get());
        owned.release();
    }
}
chronolog::ExtractionModuleConfiguration&
chronolog::ExtractionModuleConfiguration::operator=(ExtractionModuleConfiguration const& source)
{
    if(this != &source)
    {
        ExtractionModuleConfiguration owned(source);
        swap(owned);
    }
    return *this;
}
chronolog::ExtractionModuleConfiguration::ExtractionModuleConfiguration(ExtractionModuleConfiguration&& source) noexcept
    : extraction_stream_count(source.extraction_stream_count)
    , extraction_protocol(std::move(source.extraction_protocol))
    , extractors(std::move(source.extractors))
{
    source.extractors.clear();
}
chronolog::ExtractionModuleConfiguration&
chronolog::ExtractionModuleConfiguration::operator=(ExtractionModuleConfiguration&& source) noexcept
{
    if(this != &source)
    {
        ExtractionModuleConfiguration owned(std::move(source));
        swap(owned);
    }
    return *this;
}
void chronolog::ExtractionModuleConfiguration::swap(ExtractionModuleConfiguration& other) noexcept
{
    std::swap(extraction_stream_count, other.extraction_stream_count);
    extraction_protocol.swap(other.extraction_protocol);
    extractors.swap(other.extractors);
}

chronolog::ExtractionModuleConfiguration::~ExtractionModuleConfiguration()
{
    //clear old extractors map
    for(auto iter = extractors.begin(); iter != extractors.end(); ++iter) { json_object_put((*iter).second); }

    extractors.clear();
}

///////

int chronolog::ExtractionModuleConfiguration::parse_json_object(json_object* json_block)
{

    if(json_block == nullptr || !json_object_is_type(json_block, json_type_object))
    {
        return chl::CL_ERR_INVALID_CONF;
    }

    //clear old extractors map

    for(auto iter = extractors.begin(); iter != extractors.end(); ++iter) { json_object_put((*iter).second); }

    extractors.clear();

    json_object_object_foreach(json_block, key, json_val)
    {

        if(strcmp(key, "extraction_stream_count") == 0)
        {
            if(!json_object_is_type(json_val, json_type_int))
            {
                std::cerr << "[ExtractionConfiguration] Invalid 'extraction_stream_count': expected json_object"
                          << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            int value = json_object_get_int(json_val);
            extraction_stream_count = (value >= 0 ? value : 0);
            if(extraction_stream_count == 0)
            {
                std::cerr << "[ExtractionModuleConfiguration] Invalid 'extraction_stream_count': expected integer "
                          << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
        }
        else if(strcmp(key, "extraction_protocol") == 0)
        {
            if(!json_object_is_type(json_val, json_type_string))
            {
                std::cerr << "[ExtractionConfiguration] Invalid 'extraction_protocol': expected json_object"
                          << std::endl;
                return chl::CL_ERR_INVALID_CONF;
            }
            extraction_protocol = json_object_get_string(json_val);
        }
        else if(strcmp(key, "extractors") == 0)
        {
            if((json_val == nullptr) || !json_object_is_type(json_val, json_type_object))
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
                else if(!json_object_is_type(json_object_object_get(val, "type"), json_type_string))
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
