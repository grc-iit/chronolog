// A grapher's durability rests on its HDF5 archive: only the HDF5 extractor
// advances the persisted watermark W and settles the receipts a keeper waits
// for. A grapher without it never lets a keeper free a chunk, and every chunk
// is sent to it again each resend interval. So the grapher's extraction chain
// must include hdf5_extractor: a configuration that lists no extractors gets
// it, and one that lists others but not it is rejected. Others may run
// alongside it.

#include <gtest/gtest.h>

#include <string>

#include <json-c/json.h>

#include <chronolog_errcode.h>
#include <ChronoGrapherConfiguration.h>

namespace chl = chronolog;

namespace
{
// parses a chrono_grapher block written as JSON text
int parseGrapher(chl::GrapherConfiguration& conf, std::string const& grapher_json)
{
    json_object* parsed = json_tokener_parse(grapher_json.c_str());
    EXPECT_NE(parsed, nullptr) << grapher_json;
    int const status = conf.parseJsonConf(parsed);
    json_object_put(parsed);
    return status;
}

std::string extractionModule(std::string const& extractors)
{
    return R"({"ExtractionModule": {"extraction_stream_count": 2, "extraction_protocol": "ofi+sockets",
               "extractors": {)" +
           extractors + "}}}";
}

std::string const kHdf5 = R"("hdf5_archive_extractor": {"type": "hdf5_extractor", "hdf5_archive_dir": "/data"})";
std::string const kCsv = R"("csv_archive_extractor": {"type": "csv_extractor", "csv_archive_dir": "/csv"})";
std::string const kLogging = R"("log_extractor": {"type": "logging_extractor"})";

std::string hdf5ArchiveDir(chl::GrapherConfiguration const& conf)
{
    auto const hdf5 = conf.EXTRACTION_MODULE_CONF.extractors.find("hdf5_extractor");
    if(hdf5 == conf.EXTRACTION_MODULE_CONF.extractors.end())
    {
        return "";
    }
    return json_object_get_string(json_object_object_get(hdf5->second, "hdf5_archive_dir"));
}
} // namespace

TEST(GrapherConfiguration, AnHdf5ExtractorIsAccepted)
{
    chl::GrapherConfiguration conf;
    EXPECT_EQ(parseGrapher(conf, extractionModule(kHdf5)), chl::CL_SUCCESS);
    EXPECT_EQ(hdf5ArchiveDir(conf), "/data");
}

TEST(GrapherConfiguration, OtherExtractorsAlongsideHdf5AreAccepted)
{
    chl::GrapherConfiguration conf;
    EXPECT_EQ(parseGrapher(conf, extractionModule(kHdf5 + ", " + kCsv + ", " + kLogging)), chl::CL_SUCCESS);
    EXPECT_EQ(conf.EXTRACTION_MODULE_CONF.extractors.size(), 3u);
}

TEST(GrapherConfiguration, OnlyACsvExtractorIsRejected)
{
    chl::GrapherConfiguration conf;
    EXPECT_EQ(parseGrapher(conf, extractionModule(kCsv)), chl::CL_ERR_INVALID_CONF);
}

TEST(GrapherConfiguration, OnlyALoggingExtractorIsRejected)
{
    chl::GrapherConfiguration conf;
    EXPECT_EQ(parseGrapher(conf, extractionModule(kLogging)), chl::CL_ERR_INVALID_CONF);
}

TEST(GrapherConfiguration, CsvAndLoggingWithoutHdf5AreRejected)
{
    chl::GrapherConfiguration conf;
    EXPECT_EQ(parseGrapher(conf, extractionModule(kCsv + ", " + kLogging)), chl::CL_ERR_INVALID_CONF);
}

TEST(GrapherConfiguration, NoExtractorsListedGetsTheHdf5Extractor)
{
    chl::GrapherConfiguration conf;
    EXPECT_EQ(parseGrapher(conf, R"({"ExtractionModule": {"extraction_stream_count": 2}})"), chl::CL_SUCCESS);
    EXPECT_EQ(hdf5ArchiveDir(conf), "/tmp");
}

TEST(GrapherConfiguration, NoExtractionModuleGetsTheHdf5Extractor)
{
    chl::GrapherConfiguration conf;
    EXPECT_EQ(parseGrapher(conf, "{}"), chl::CL_SUCCESS);
    EXPECT_EQ(hdf5ArchiveDir(conf), "/tmp");
}

TEST(GrapherConfiguration, TheShippedTemplateIsAccepted)
{
    json_object* conf_template = json_object_from_file(CHRONOLOG_CONF_TEMPLATE);
    ASSERT_NE(conf_template, nullptr) << CHRONOLOG_CONF_TEMPLATE;
    chl::GrapherConfiguration conf;
    EXPECT_EQ(conf.parseJsonConf(json_object_object_get(conf_template, "chrono_grapher")), chl::CL_SUCCESS);
    EXPECT_NE(hdf5ArchiveDir(conf), "");
    json_object_put(conf_template);
}
