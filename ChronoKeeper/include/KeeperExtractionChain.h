#ifndef CHRONO_KEEPER_EXTRACTION_CHAIN
#define CHRONO_KEEPER_EXTRACTION_CHAIN

#include <variant>
#include <vector>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <ChunkLoggingExtractor.h>
#include <ChunkExtractorCSV.h>
#include <ChunkExtractorRDMA.h>
#include <DualEndpointChunkExtractorRDMA.h>
#include <ExtractionModuleConfiguration.h>

namespace tl = thallium;

namespace chronolog
{
using Extractor =
        std::variant<LoggingExtractor, StoryChunkExtractorCSV, StoryChunkExtractorRDMA, DualEndpointChunkExtractorRDMA>;

class ChronoKeeperExtractionChain
{
    std::vector<Extractor> theExtractors;

public:
    ChronoKeeperExtractionChain() {}

    ~ChronoKeeperExtractionChain() { theExtractors.clear(); }

    void add_extractor(Extractor e) { theExtractors.push_back(std::move(e)); }

    int process_chunk(StoryChunk* chunk)
    {
        int chain_result = CL_SUCCESS;

        // If extractor fails, mark the chain result as a failure,
        // but keep going for the others.
        for(auto& e: theExtractors)
        {
            int extractor_result =
                    std::visit([chunk](auto& extractor) -> int { return extractor.process_chunk(chunk); }, e);

            if(CL_SUCCESS != extractor_result)
            {
                chain_result = extractor_result;
            }
        }
        return chain_result;
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

    int activate(tl::engine& extraction_engine,
                 ExtractionModuleConfiguration const& extraction_conf,
                 ServiceId const& service_id)
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
                add_extractor(csv_extractor);
            }
            else if((*iter).first == "single_endpoint_rdma_extractor")
            {
                StoryChunkExtractorRDMA single_endpoint_rdma_extractor(extraction_engine);
                ret_value = single_endpoint_rdma_extractor.reset((*iter).second);
                if(CL_SUCCESS != ret_value)
                {
                    break;
                }
                add_extractor(single_endpoint_rdma_extractor);
            }
            else if((*iter).first == "dual_endpoint_rdma_extractor")
            {
                DualEndpointChunkExtractorRDMA dual_endpoint_rdma_extractor(extraction_engine);
                ret_value = dual_endpoint_rdma_extractor.reset((*iter).second);
                if(CL_SUCCESS != ret_value)
                {
                    break;
                }
                add_extractor(dual_endpoint_rdma_extractor);
            }
            else if((*iter).first == "logging_extractor")
            {
                add_extractor(LoggingExtractor());
            }
        }

        return ret_value;
    }
};

} // namespace chronolog

#endif
