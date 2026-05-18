#include <cstdio>
#include <gtest/gtest.h>
#include <string>

#include <chronosql.h>

// Smoke tests for the configuration-file factory. These tests do NOT require
// a running ChronoLog deployment: they only exercise the config-loading path
// that runs *before* any RPC connection attempt. A bad path must cause
// Create() to return nullptr (the library boundary catches the underlying
// exception and signals failure to the caller without forcing them to use
// try/catch).

TEST(ChronoSQLConfig, ReturnsNullOnNonexistentConfigFile)
{
    EXPECT_EQ(chronosql::ChronoSQL::Create("/this/path/should/not/exist/chronosql.json"), nullptr);
}

TEST(ChronoSQLConfig, ReturnsNullOnConfigFileMissingChronoClientSection)
{
    const std::string path = "/tmp/chronosql_no_section_config.json";
    {
        std::FILE* f = std::fopen(path.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fputs("{}", f);
        std::fclose(f);
    }
    EXPECT_EQ(chronosql::ChronoSQL::Create(path), nullptr);
    std::remove(path.c_str());
}
