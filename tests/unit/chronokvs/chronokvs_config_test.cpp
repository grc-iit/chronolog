#include <gtest/gtest.h>
#include <cstdio>
#include <string>

#include <chronokvs.h>

// Smoke tests for the configuration-file factory introduced for issue #470.
//
// These tests do NOT require a running ChronoLog deployment: they only
// exercise the config-loading path that runs *before* any RPC connection
// attempt. A bad path must cause Create() to return nullptr (the library
// boundary catches the underlying exception and signals failure to the
// caller without forcing them to use try/catch).

TEST(ChronoKVSConfig, ReturnsNullOnNonexistentConfigFile)
{
    EXPECT_EQ(chronokvs::ChronoKVS::Create("/this/path/should/not/exist/chronokvs_test.json"), nullptr);
}

TEST(ChronoKVSConfig, ReturnsNullOnConfigFileMissingChronoClientSection)
{
    // A syntactically valid JSON file that lacks the required 'chrono_client'
    // section: ClientConfiguration::load_from_file() returns false, so the
    // factory must return nullptr before any RPC connection is attempted.
    const std::string path = "/tmp/chronokvs_no_section_config.json";
    {
        std::FILE* f = std::fopen(path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("{}", f);
        std::fclose(f);
    }
    EXPECT_EQ(chronokvs::ChronoKVS::Create(path), nullptr);
    std::remove(path.c_str());
}
