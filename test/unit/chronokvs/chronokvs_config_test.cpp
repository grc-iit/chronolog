#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

#include <chronokvs.h>

// Smoke tests for the configuration-file constructor introduced for issue #470.
//
// These tests do NOT require a running ChronoLog deployment: they only
// exercise the config-loading path that runs *before* any RPC connection
// attempt. A bad path must throw before the client tries to connect.

TEST(ChronoKVSConfig, ThrowsOnNonexistentConfigFile)
{
    EXPECT_THROW(chronokvs::ChronoKVS("/this/path/should/not/exist/chronokvs_test.json"), std::runtime_error);
}

TEST(ChronoKVSConfig, ThrowsOnConfigFileMissingChronoClientSection)
{
    // A syntactically valid JSON file that lacks the required 'chrono_client'
    // section: ClientConfiguration::load_from_file() returns false, so the
    // constructor must throw before any RPC connection is attempted.
    const std::string path = "/tmp/chronokvs_no_section_config.json";
    {
        std::FILE* f = std::fopen(path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("{}", f);
        std::fclose(f);
    }
    EXPECT_THROW(chronokvs::ChronoKVS{path}, std::runtime_error);
    std::remove(path.c_str());
}
