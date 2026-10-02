// DataStoreConf reads a component's DataStoreInternals block. Its knobs are
// signed in the JSON and unsigned where they are used, so a negative value
// wraps into a huge one and quietly turns the mechanism off instead of failing
// the daemon at startup: a negative report interval stops every watermark
// report, a negative resend timeout stops every re-send, and a negative
// retention cap silences the memory warning.

#include <gtest/gtest.h>

#include <json-c/json.h>

#include <chronolog_errcode.h>
#include <ConfigurationBlocks.h>

namespace chl = chronolog;

namespace
{
int parseInto(chl::DataStoreConf& conf, char const* json_text)
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

TEST(DataStoreConf, ReadsTheKnobsItIsGiven)
{
    chl::DataStoreConf conf;
    ASSERT_EQ(parseInto(conf,
                        R"({"watermark_report_interval_secs": 5,
                            "watermark_resend_timeout_secs": 60,
                            "retention_cap_mb": 128})"),
              chl::CL_SUCCESS);
    EXPECT_EQ(conf.watermark_report_interval_secs, 5);
    EXPECT_EQ(conf.watermark_resend_timeout_secs, 60);
    EXPECT_EQ(conf.retention_cap_mb, 128);
}

TEST(DataStoreConf, RejectsANegativeReportInterval)
{
    chl::DataStoreConf conf;
    EXPECT_EQ(parseInto(conf, R"({"watermark_report_interval_secs": -1})"), chl::CL_ERR_INVALID_CONF);
}

TEST(DataStoreConf, RejectsANegativeResendTimeout)
{
    chl::DataStoreConf conf;
    EXPECT_EQ(parseInto(conf, R"({"watermark_resend_timeout_secs": -1})"), chl::CL_ERR_INVALID_CONF);
}

// 0 would send every unconfirmed chunk again on each keeper pass, replacing
// its receipt each time, so no chunk would ever be confirmed
TEST(DataStoreConf, RejectsAZeroResendTimeout)
{
    chl::DataStoreConf conf;
    EXPECT_EQ(parseInto(conf, R"({"watermark_resend_timeout_secs": 0})"), chl::CL_ERR_INVALID_CONF);
}

TEST(DataStoreConf, RejectsANegativeRetentionCap)
{
    chl::DataStoreConf conf;
    EXPECT_EQ(parseInto(conf, R"({"retention_cap_mb": -1})"), chl::CL_ERR_INVALID_CONF);
}

TEST(DataStoreConf, RejectsANegativeTailCapacity)
{
    chl::DataStoreConf conf;
    EXPECT_EQ(parseInto(conf, R"({"tail_capacity": -1})"), chl::CL_ERR_INVALID_CONF);
}
