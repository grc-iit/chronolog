#ifndef CHRONO_GRAPHER_EXTRACTION_CHAIN
#define CHRONO_GRAPHER_EXTRACTION_CHAIN

#include <variant>
#include <vector>
#include <thallium.hpp>

#include <chronolog_errcode.h>
#include <ChunkLoggingExtractor.h>
#include <ChunkExtractorCSV.h>
#include <HDF5FileChunkExtractor.h>
#include <StoryWatermarkRegistry.h>
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

    // CL_SUCCESS only if every active extractor accepted the chunk; the first
    // failure code otherwise.
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

    // Disposal seam invoked by the extraction module after process_chunk. The
    // grapher keeps today's ownership model — the drained chunk is freed
    // regardless of outcome; durability feedback to the keepers flows through
    // the StoryWatermarkRegistry instead.
    void dispose_chunk(StoryChunk* chunk, int /*status*/) { delete chunk; }

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
                 ExtractionModuleConfiguration const& extraction_conf,
                 StoryWatermarkRegistry* watermark_registry = nullptr,
                 ArchiveManifest* archive_manifest = nullptr)
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
                hdf5_extractor.attachWatermarkRegistry(watermark_registry);
                hdf5_extractor.attachArchiveManifest(archive_manifest);
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
        //TODO #635
    }

    // Forward a story-level destroy to every extractor that knows how to
    // delete persisted artifacts. Today only HDF5 persists; CSV/logging are
    // no-ops. Returns the first non-success status.
    int delete_story_files(std::string const& chronicle_name, std::string const& story_name)
    {
        int ret = CL_SUCCESS;
        for(auto& e: theExtractors)
        {
            int rc = std::visit(
                    [&](auto& extractor) -> int
                    {
                        using T = std::decay_t<decltype(extractor)>;
                        if constexpr(std::is_same_v<T, HDF5FileChunkExtractor>)
                        {
                            return extractor.delete_story_files(chronicle_name, story_name);
                        }
                        return CL_SUCCESS;
                    },
                    e);
            if(rc != CL_SUCCESS && ret == CL_SUCCESS)
            {
                ret = rc;
            }
        }
        return ret;
    }

    int delete_chronicle_files(std::string const& chronicle_name)
    {
        int ret = CL_SUCCESS;
        for(auto& e: theExtractors)
        {
            int rc = std::visit(
                    [&](auto& extractor) -> int
                    {
                        using T = std::decay_t<decltype(extractor)>;
                        if constexpr(std::is_same_v<T, HDF5FileChunkExtractor>)
                        {
                            return extractor.delete_chronicle_files(chronicle_name);
                        }
                        return CL_SUCCESS;
                    },
                    e);
            if(rc != CL_SUCCESS && ret == CL_SUCCESS)
            {
                ret = rc;
            }
        }
        return ret;
    }
};

} // namespace chronolog

#endif
