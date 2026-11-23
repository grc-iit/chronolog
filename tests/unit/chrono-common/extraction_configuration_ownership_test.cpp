#include <gtest/gtest.h>
#include <ExtractionModuleConfiguration.h>
#include <utility>
namespace
{
json_object* observed(unsigned& destroyed)
{
    auto object = json_object_new_object();
    json_object_set_userdata(object,
                             &destroyed,
                             [](json_object*, void* counter) { ++*static_cast<unsigned*>(counter); });
    return object;
}
TEST(ExtractionConfigurationOwnership, CopyKeepsTheOriginalExtractorAlive)
{
    unsigned destroyed = 0;
    {
        chronolog::ExtractionModuleConfiguration original;
        original.extraction_stream_count = 3;
        original.extraction_protocol = "ofi+tcp";
        original.extractors["csv_extractor"] = observed(destroyed);
        {
            auto retained = original;
            EXPECT_EQ(retained.extraction_stream_count, 3);
            EXPECT_EQ(retained.extraction_protocol, "ofi+tcp");
        }
        EXPECT_EQ(destroyed, 0u);
        // RED fixture cleanup only: avoid a second put through the old dangling
        // pointer after observing that the copy freed the original's object.
        if(destroyed)
            original.extractors.clear();
    }
    EXPECT_EQ(destroyed, 1u);
}
TEST(ExtractionConfigurationOwnership, CopyAssignmentReleasesOldAndRetainsNewOwnership)
{
    unsigned previousDestroyed = 0, sourceDestroyed = 0;
    {
        chronolog::ExtractionModuleConfiguration source;
        source.extractors["csv_extractor"] = observed(sourceDestroyed);
        auto previous = observed(previousDestroyed);
        {
            chronolog::ExtractionModuleConfiguration target;
            target.extractors["old"] = previous;
            target = source;
            EXPECT_EQ(previousDestroyed, 1u);
            if(!previousDestroyed)
                json_object_put(previous); // Dispose only the leaked RED fixture object.
            target = target;
        }
        EXPECT_EQ(sourceDestroyed, 0u);
        if(sourceDestroyed)
            source.extractors.clear();
    }
    EXPECT_EQ(sourceDestroyed, 1u);
}
TEST(ExtractionConfigurationOwnership, CopyPreservesNestedValuesAndOwnsIndependentNodes)
{
    unsigned sourceDestroyed = 0;
    chronolog::ExtractionModuleConfiguration retained;
    {
        chronolog::ExtractionModuleConfiguration source;
        auto value = json_tokener_parse(R"({"nested":{"signed":-9223372036854775808,"unsigned":18446744073709551615,
                                            "double":1.125,"text":"a\u0000b","array":[true,null,3]}})");
        ASSERT_NE(value, nullptr);
        json_object_set_userdata(value,
                                 &sourceDestroyed,
                                 [](json_object*, void* counter) { ++*static_cast<unsigned*>(counter); });
        source.extractors["values"] = value;
        retained = source;
        auto copy = retained.extractors.at("values");
        EXPECT_NE(copy, value);
        EXPECT_TRUE(json_object_equal(value, copy));
        EXPECT_EQ(json_object_get_userdata(copy), nullptr);
        json_object *originalNested = nullptr, *copiedNested = nullptr;
        ASSERT_TRUE(json_object_object_get_ex(value, "nested", &originalNested));
        ASSERT_TRUE(json_object_object_get_ex(copy, "nested", &copiedNested));
        EXPECT_NE(originalNested, copiedNested);
        json_object_object_add(copiedNested, "copy_only", json_object_new_int(9));
        json_object* absent = nullptr;
        EXPECT_FALSE(json_object_object_get_ex(originalNested, "copy_only", &absent));
    }
    EXPECT_EQ(sourceDestroyed, 1u);
    auto copy = retained.extractors.at("values");
    json_object* nested = nullptr;
    ASSERT_TRUE(json_object_object_get_ex(copy, "nested", &nested));
    json_object* number = nullptr;
    ASSERT_TRUE(json_object_object_get_ex(nested, "unsigned", &number));
    EXPECT_EQ(json_object_get_uint64(number), UINT64_MAX);
}
TEST(ExtractionConfigurationOwnership, MoveTransfersAnExtractorWithoutAnExtraRelease)
{
    unsigned destroyed = 0;
    chronolog::ExtractionModuleConfiguration source;
    source.extractors["csv_extractor"] = observed(destroyed);
    {
        auto retained = std::move(source);
        EXPECT_TRUE(source.extractors.empty());
        EXPECT_EQ(destroyed, 0u);
    }
    EXPECT_EQ(destroyed, 1u);
    if(destroyed)
        source.extractors.clear();
}
TEST(ExtractionConfigurationOwnership, MoveAssignmentReleasesOldAndTransfersNewOwnership)
{
    unsigned previousDestroyed = 0, sourceDestroyed = 0;
    chronolog::ExtractionModuleConfiguration source;
    source.extractors["csv_extractor"] = observed(sourceDestroyed);
    auto previous = observed(previousDestroyed);
    {
        chronolog::ExtractionModuleConfiguration target;
        target.extractors["old"] = previous;
        target = std::move(source);
        EXPECT_TRUE(source.extractors.empty());
        EXPECT_EQ(previousDestroyed, 1u);
        if(!previousDestroyed)
            json_object_put(previous);
    }
    EXPECT_EQ(sourceDestroyed, 1u);
    if(sourceDestroyed)
        source.extractors.clear();
}
} // namespace
