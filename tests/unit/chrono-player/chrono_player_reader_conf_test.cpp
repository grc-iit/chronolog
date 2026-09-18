// The player's ArchiveReaders block: where its archive lives, how often it
// lists that directory for new files, and the grapher's window size, which a
// replay uses to build the names of files the listing has not shown yet.

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

TEST(PlayerReaderConf, ReadsTheArchiveReaderKnobs)
{
    chl::PlayerConfiguration conf;
    ASSERT_EQ(parseInto(conf,
                        R"({"ArchiveReaders": {"story_files_dir": "/tmp/archive",
                                               "archive_scan_interval_secs": 3,
                                               "archive_window_secs": 45}})"),
              chl::CL_SUCCESS);
    EXPECT_EQ(conf.READER_CONF.story_files_dir, "/tmp/archive");
    EXPECT_EQ(conf.READER_CONF.archive_scan_interval_secs, 3);
    EXPECT_EQ(conf.READER_CONF.archive_window_secs, 45);
}

TEST(PlayerReaderConf, RejectsAScanIntervalOfZero)
{
    // the scan thread would spin
    chl::PlayerConfiguration conf;
    EXPECT_EQ(parseInto(conf, R"({"ArchiveReaders": {"archive_scan_interval_secs": 0}})"), chl::CL_ERR_INVALID_CONF);
}

TEST(PlayerReaderConf, RejectsANegativeScanInterval)
{
    chl::PlayerConfiguration conf;
    EXPECT_EQ(parseInto(conf, R"({"ArchiveReaders": {"archive_scan_interval_secs": -5}})"), chl::CL_ERR_INVALID_CONF);
}

TEST(PlayerReaderConf, RejectsANegativeWindow)
{
    chl::PlayerConfiguration conf;
    EXPECT_EQ(parseInto(conf, R"({"ArchiveReaders": {"archive_window_secs": -30}})"), chl::CL_ERR_INVALID_CONF);
}

TEST(PlayerReaderConf, ZeroWindowIsAllowedAndTurnsProbingOff)
{
    chl::PlayerConfiguration conf;
    ASSERT_EQ(parseInto(conf, R"({"ArchiveReaders": {"archive_window_secs": 0}})"), chl::CL_SUCCESS);
    EXPECT_EQ(conf.READER_CONF.archive_window_secs, 0);
}
