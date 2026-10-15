// The player's ArchiveReaders block: where its archive lives.

#include <gtest/gtest.h>

#include <json-c/json.h>

#include <chronolog_errcode.h>
#include <ChronoPlayerConfiguration.h>

namespace chl = chronolog;

namespace
{
int parseInto(chl::PlayerConfiguration& conf, char const* json_text)
{
    json_object* json = json_tokener_parse(json_text);
    EXPECT_NE(json, nullptr) << json_text;
    if(json == nullptr)
    {
        return -1;
    }
    int const status = conf.parseJsonConf(json);
    json_object_put(json);
    return status;
}
} // namespace

TEST(PlayerReaderConf, ReadsTheArchiveDirectory)
{
    chl::PlayerConfiguration conf;
    ASSERT_EQ(parseInto(conf, R"({"ArchiveReaders": {"story_files_dir": "/tmp/archive"}})"), chl::CL_SUCCESS);
    EXPECT_EQ(conf.READER_CONF.story_files_dir, "/tmp/archive");
}

// The knobs of the directory listing and name probing that the manifest
// replaced are ignored, so a conf that still has them keeps working.
TEST(PlayerReaderConf, KnobsOfTheRemovedDirectoryScanAreIgnored)
{
    chl::PlayerConfiguration conf;
    EXPECT_EQ(parseInto(conf,
                        R"({"ArchiveReaders": {"story_files_dir": "/tmp/archive",
                                               "archive_scan_interval_secs": 0,
                                               "archive_window_secs": -30}})"),
              chl::CL_SUCCESS);
}
