#include <gtest/gtest.h>
#include <ChronoVisorConfiguration.h>
#include <string>

namespace
{
int parse(chronolog::VisorConfiguration& conf, const char* text)
{
    auto* root = json_tokener_parse(text);
    EXPECT_NE(root, nullptr);
    int rc = conf.parseJsonConf(root);
    json_object_put(root);
    return rc;
}
} // namespace

TEST(VisorConfig, CatalogPathParsed)
{
    chronolog::VisorConfiguration conf;
    ASSERT_EQ(parse(conf, R"({"catalog_db_path":"/data/visor/catalog.sqlite"})"), chronolog::CL_SUCCESS);
    EXPECT_EQ(conf.CATALOG_DB_PATH, "/data/visor/catalog.sqlite");
    EXPECT_NE(conf.to_String().find("/data/visor/catalog.sqlite"), std::string::npos);
}

TEST(VisorConfig, CatalogPathDefaultsWhenMissing)
{
    chronolog::VisorConfiguration conf;
    ASSERT_EQ(parse(conf, R"({"rpc_slots":8})"), chronolog::CL_SUCCESS);
    EXPECT_EQ(conf.CATALOG_DB_PATH, "visor/catalog.sqlite");
}

TEST(VisorConfig, EmptyCatalogPathRefused)
{
    for(auto text: {R"({"catalog_db_path":""})", R"({"catalog_db_path":5})", R"({"catalog_db_path":null})"})
    {
        chronolog::VisorConfiguration conf;
        EXPECT_EQ(parse(conf, text), chronolog::CL_ERR_INVALID_CONF) << text;
    }
}
