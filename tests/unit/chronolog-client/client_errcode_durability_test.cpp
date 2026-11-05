#include <string>

#include <gtest/gtest.h>

#include <chronolog_errcode.h>
#include <client_errcode.h>

TEST(ClientErrcode, DurabilityCodes)
{
    EXPECT_EQ(chronolog::CL_ERR_DURABILITY_UNAVAILABLE, -19);
    EXPECT_EQ(chronolog::CL_ERR_DURABILITY_FULL, -20);
    EXPECT_EQ(chronolog::CL_ERR_DURABILITY_UNKNOWN, -21);

    EXPECT_STREQ(chronolog::to_string(chronolog::CL_ERR_DURABILITY_UNAVAILABLE), "CL_ERR_DURABILITY_UNAVAILABLE");
    EXPECT_STREQ(chronolog::to_string(chronolog::CL_ERR_DURABILITY_FULL), "CL_ERR_DURABILITY_FULL");
    EXPECT_STREQ(chronolog::to_string(chronolog::CL_ERR_DURABILITY_UNKNOWN), "CL_ERR_DURABILITY_UNKNOWN");

    EXPECT_STREQ(chronolog::to_string_client(-19), "CL_ERR_DURABILITY_UNAVAILABLE");
    EXPECT_STREQ(chronolog::to_string_client(-20), "CL_ERR_DURABILITY_FULL");
    EXPECT_STREQ(chronolog::to_string_client(-21), "CL_ERR_DURABILITY_UNKNOWN");
}

TEST(ClientErrcode, ReservedAndExistingCodesKeepTheirMeaning)
{
    EXPECT_EQ(chronolog::CL_ERR_PARTIAL_RESULT, -18);
    EXPECT_STREQ(chronolog::to_string(chronolog::CL_ERR_PARTIAL_RESULT), "CL_ERR_PARTIAL_RESULT");
    // -14..-17 stay reserved for the acceptance-window work.
    for(int code = -17; code <= -14; ++code)
    {
        EXPECT_STREQ(chronolog::to_string_client(code), "UnknownClientErrorCode");
    }
    EXPECT_STREQ(chronolog::to_string_client(-22), "UnknownClientErrorCode");
}
