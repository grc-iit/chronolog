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
#include <KeeperChunkRetentionStore.h>

namespace tl = thallium;

namespace chronolog
{
using Extractor =
        std::variant<LoggingExtractor, StoryChunkExtractorCSV, StoryChunkExtractorRDMA, DualEndpointChunkExtractorRDMA>;

class ChronoKeeperExtractionChain
{
    std::vector<Extractor> theExtractors;
    KeeperChunkRetentionStore* theRetentionStore = nullptr;

public:
    ChronoKeeperExtractionChain() {}

    ~ChronoKeeperExtractionChain() { theExtractors.clear(); }

    // CL_SUCCESS only if every active extractor accepted the chunk; the first
    // failure code otherwise. The extraction module feeds this status back to
    // dispose_chunk so a failed grapher transfer no longer destroys the chunk.
    int process_chunk(StoryChunk* chunk)
    {
        int ret_value = CL_SUCCESS;
        for(auto& e: theExtractors)
        {
            int rc = std::visit([chunk](auto& extractor) -> int { return extractor.process_chunk(chunk); }, e);
            if(rc != CL_SUCCESS && ret_value == CL_SUCCESS)
            {
                ret_value = rc;
            }
        }
        return ret_value;
    }

    // The retention store owns every sealed chunk; the chain only reports
    // drain outcomes to it. Called from ChronoKeeperInstance after activation.
    void attachRetentionStore(KeeperChunkRetentionStore* store) { theRetentionStore = store; }

    // Whether this chain will ever receive grapher watermark reports. Task 2
    // shim: always false, so a successful send also counts as persisted and
    // behavior matches the historic free-on-ack. Task 3 implements it for
    // real (true iff a grapher-bound RDMA extractor is in the chain), which
    // also permanently covers CSV-only/logging-only keeper configs that will
    // never receive reports.
    bool expects_watermarks() const { return false; }

    // Disposal seam invoked by the extraction module after process_chunk.
    // Never deletes a tracked chunk: ownership stays with the retention store.
    void dispose_chunk(StoryChunk* chunk, int status)
    {
        if(theRetentionStore == nullptr)
        {
            // defensive: no store attached (bare-chain tests) — free the chunk
            delete chunk;
            return;
        }
        // copy identity before the callbacks: confirmPersisted may free the
        // chunk, and a reference into it must not outlive that
        StoryId const story_id = chunk->getStoryId();
        uint64_t const end_time = chunk->getEndTime();
        if(status == CL_SUCCESS)
        {
            theRetentionStore->markShipped(chunk);
            if(!expects_watermarks())
            {
                theRetentionStore->confirmPersisted(story_id, end_time);
            }
        }
        else
        {
            theRetentionStore->markSendFailed(chunk);
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

    int activate(ServiceId const& service_id,
                 tl::engine* extraction_engine,
                 ExtractionModuleConfiguration const& extraction_conf)
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
            else if((*iter).first == "single_endpoint_rdma_extractor")
            {
                StoryChunkExtractorRDMA single_endpoint_rdma_extractor(extraction_engine);
                ret_value = single_endpoint_rdma_extractor.reset((*iter).second);
                if(CL_SUCCESS != ret_value)
                {
                    break;
                }
                theExtractors.push_back(std::move(single_endpoint_rdma_extractor));
            }
            else if((*iter).first == "dual_endpoint_rdma_extractor")
            {
                DualEndpointChunkExtractorRDMA dual_endpoint_rdma_extractor(extraction_engine);
                ret_value = dual_endpoint_rdma_extractor.reset((*iter).second);
                if(CL_SUCCESS != ret_value)
                {
                    break;
                }

                theExtractors.push_back(std::move(dual_endpoint_rdma_extractor));
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
        //TODO #635
    }
};

} // namespace chronolog

#endif
