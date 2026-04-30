#include <cstdio>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

#include <chronosql.h>

// Smoke tests for the config-file constructor. These run without a live
// ChronoLog deployment: they only exercise the load path that runs before
// any RPC connection attempt.

TEST(ChronoSQLConfig, ThrowsOnNonexistentConfigFile)
{
    EXPECT_THROW(chronosql::ChronoSQL{"/this/path/should/not/exist/chronosql.json"}, std::runtime_error);
}

TEST(ChronoSQLConfig, ThrowsOnConfigFileMissingChronoClientSection)
{
    const std::string path = "/tmp/chronosql_no_section_config.json";
    {
        std::FILE* f = std::fopen(path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("{}", f);
        std::fclose(f);
    }
    EXPECT_THROW(chronosql::ChronoSQL{path}, std::runtime_error);
    std::remove(path.c_str());
}
