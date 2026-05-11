#ifndef CHRONO_GRAPHER_EXTRACTION_CHAIN
#define CHRONO_GRAPHER_EXTRACTION_CHAIN

#include <variant>
#include <vector>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <ChunkLoggingExtractor.h>
#include <ChunkExtractorCSV.h>
#include <HDF5FileChunkExtractor.h>
#include <ExtractionModuleConfiguration.h>

namespace tl = thallium;

namespace chronolog
{
using Extractor = std::variant<LoggingExtractor, StoryChunkExtractorCSV, HDF5FileChunkExtractor>;

class ChronoGrapherExtractionChain
{
    std::vector<Extractor> theExtractors;

public:
    ChronoGrapherExtractionChain() {}

    ~ChronoGrapherExtractionChain() { theExtractors.clear(); }

    void process_chunk(StoryChunk* chunk)
    {
        for(auto& e: theExtractors)
        {
            std::visit([chunk](auto& extractor) { extractor.process_chunk(chunk); }, e);
        }
    }

    bool is_active_chain() const
    {
        if(theExtractors.empty())
        {
            return false;
        }

        for(const auto& e: theExtractors)
        {
            bool active = std::visit([](const auto& extractor) -> bool { return extractor.is_active(); }, e);

            // if any single extractor is NOT active, the whole chain fails
            if(!active)
            {
                return false;
            }
        }

        return true;
    }

    int activate(ServiceId const& service_id, ExtractionModuleConfiguration const& extraction_conf)
    {
        int ret_value = CL_SUCCESS;

        for(auto iter = extraction_conf.extractors.begin(); iter != extraction_conf.extractors.end(); ++iter)
        {

            if((*iter).first == "csv_extractor")
            {
                StoryChunkExtractorCSV csv_extractor(service_id);
                ret_value = csv_extractor.reset((*iter).second);
                if(CL_SUCCESS != ret_value)
                {
                    break;
                }
		theExtractors.push_back(std::move(csv_extractor));
            }
            else if((*iter).first == "hdf5_extractor")
            {
                HDF5FileChunkExtractor hdf5_extractor;
                ret_value = hdf5_extractor.reset((*iter).second);
                if(CL_SUCCESS != ret_value)
                {
                    break;
                }
		theExtractors.push_back(std::move(hdf5_extractor));
            }
            else if((*iter).first == "logging_extractor")
            {
		theExtractors.push_back(std::move(LoggingExtractor()));
            }
        }

        return ret_value;
    }

    void flush_outage_buffers()
    {
        for(auto& e: theExtractors)
        {
           std::visit([](auto& extractor) { extractor.flush_outage_buffer(); }, e);
        }

    }	    
};

} // namespace chronolog

#endif
