// The keeper's "WAL" block. A missing block leaves the WAL off with the
// documented defaults; a block the WAL could not run with is refused when the
// configuration is read, so the keeper fails at startup and not on the first
// durable write.

#include <gtest/gtest.h>

#include <json-c/json.h>

#include <cstdint>
#include <string>

#include <ChronoKeeperConfiguration.h>
#include <chronolog_errcode.h>

namespace chl = chronolog;

namespace
{
// Parses a whole keeper section, so the test goes through the same entry point as the daemon.
int parseKeeper(chl::KeeperConfiguration& conf, std::string const& wal_block)
{
    std::string const text = wal_block.empty() ? "{}" : "{\"WAL\": " + wal_block + "}";
    json_object* json = json_tokener_parse(text.c_str());
    EXPECT_NE(json, nullptr) << text;
    if(json == nullptr)
    {
        return -1;
    }
    int const status = conf.parseJsonConf(json);
    json_object_put(json);
    return status;
}

void expectDefaults(chl::WalConf const& wal)
{
    EXPECT_FALSE(wal.enabled);
    EXPECT_EQ(wal.dir, "");
    EXPECT_EQ(wal.segment_bytes, 67108864u);
    EXPECT_EQ(wal.max_bytes, 1073741824u);
    EXPECT_EQ(wal.group_commit_max_bytes, 4194304u);
    EXPECT_EQ(wal.group_commit_window_us, 0u);
    EXPECT_EQ(wal.reserve_min_bytes, 268435456u);
    EXPECT_EQ(wal.reserve_percent, 10u);
    EXPECT_EQ(wal.durable_cap_mb, 4096u);
}
} // namespace

TEST(KeeperWalConf, AMissingBlockGivesTheDefaults)
{
    chl::KeeperConfiguration conf;
    ASSERT_EQ(parseKeeper(conf, ""), chl::CL_SUCCESS);
    expectDefaults(conf.WAL_CONF);
}

TEST(KeeperWalConf, TheTemplatesBlockParsesToTheDefaults)
{
    json_object* parsed = json_object_from_file(CHRONOLOG_CONF_TEMPLATE);
    ASSERT_NE(parsed, nullptr) << CHRONOLOG_CONF_TEMPLATE;
    json_object* keeper_json = json_object_object_get(parsed, "chrono_keeper");
    ASSERT_NE(keeper_json, nullptr);
    ASSERT_NE(json_object_object_get(keeper_json, "WAL"), nullptr);

    chl::KeeperConfiguration conf;
    ASSERT_EQ(conf.parseJsonConf(keeper_json), chl::CL_SUCCESS);
    expectDefaults(conf.WAL_CONF);
    json_object_put(parsed);
}

TEST(KeeperWalConf, ReadsEveryKey)
{
    chl::KeeperConfiguration conf;
    ASSERT_EQ(parseKeeper(conf,
                          R"({"enabled": true, "dir": "/mnt/nvme/wal", "segment_bytes": 2097152,
                              "max_bytes": 5000000, "group_commit_max_bytes": 65536,
                              "group_commit_window_us": 250, "reserve_min_bytes": 1024,
                              "reserve_percent": 20, "durable_cap_mb": 512})"),
              chl::CL_SUCCESS);
    chl::WalConf const& wal = conf.WAL_CONF;
    EXPECT_TRUE(wal.enabled);
    EXPECT_EQ(wal.dir, "/mnt/nvme/wal");
    EXPECT_EQ(wal.segment_bytes, 2097152u);
    EXPECT_EQ(wal.max_bytes, 5000000u);
    EXPECT_EQ(wal.group_commit_max_bytes, 65536u);
    EXPECT_EQ(wal.group_commit_window_us, 250u);
    EXPECT_EQ(wal.reserve_min_bytes, 1024u);
    EXPECT_EQ(wal.reserve_percent, 20u);
    EXPECT_EQ(wal.durable_cap_mb, 512u);
}

TEST(KeeperWalConf, ADisabledWalNeedsNoDirectory)
{
    chl::KeeperConfiguration conf;
    ASSERT_EQ(parseKeeper(conf, R"({"enabled": false, "dir": ""})"), chl::CL_SUCCESS);
    EXPECT_FALSE(conf.WAL_CONF.enabled);
}

TEST(KeeperWalConf, AnEnabledWalNeedsAnAbsoluteDirectory)
{
    chl::KeeperConfiguration relative;
    EXPECT_EQ(parseKeeper(relative, R"({"enabled": true, "dir": "wal/keeper-1"})"), chl::CL_ERR_INVALID_CONF);

    chl::KeeperConfiguration empty;
    EXPECT_EQ(parseKeeper(empty, R"({"enabled": true, "dir": ""})"), chl::CL_ERR_INVALID_CONF);

    chl::KeeperConfiguration missing;
    EXPECT_EQ(parseKeeper(missing, R"({"enabled": true})"), chl::CL_ERR_INVALID_CONF);

    // the order of the keys does not matter
    chl::KeeperConfiguration reordered;
    EXPECT_EQ(parseKeeper(reordered, R"({"dir": "/var/wal", "enabled": true})"), chl::CL_SUCCESS);
}

TEST(KeeperWalConf, TheCommitWindowIsCappedAtTenMilliseconds)
{
    chl::KeeperConfiguration at_cap;
    ASSERT_EQ(parseKeeper(at_cap, R"({"group_commit_window_us": 10000})"), chl::CL_SUCCESS);
    EXPECT_EQ(at_cap.WAL_CONF.group_commit_window_us, 10000u);

    chl::KeeperConfiguration over;
    EXPECT_EQ(parseKeeper(over, R"({"group_commit_window_us": 10001})"), chl::CL_ERR_INVALID_CONF);

    chl::KeeperConfiguration negative;
    EXPECT_EQ(parseKeeper(negative, R"({"group_commit_window_us": -1})"), chl::CL_ERR_INVALID_CONF);
}

TEST(KeeperWalConf, ASegmentBelowOneMebibyteIsRefused)
{
    chl::KeeperConfiguration at_floor;
    ASSERT_EQ(parseKeeper(at_floor, R"({"segment_bytes": 1048576})"), chl::CL_SUCCESS);

    chl::KeeperConfiguration below;
    EXPECT_EQ(parseKeeper(below, R"({"segment_bytes": 1048575})"), chl::CL_ERR_INVALID_CONF);
}

TEST(KeeperWalConf, ASizeOfZeroOrBelowIsRefused)
{
    for(char const* key:
        {"segment_bytes", "max_bytes", "group_commit_max_bytes", "reserve_min_bytes", "durable_cap_mb"})
    {
        for(char const* value: {"0", "-1"})
        {
            chl::KeeperConfiguration conf;
            EXPECT_EQ(parseKeeper(conf, std::string("{\"") + key + "\": " + value + "}"), chl::CL_ERR_INVALID_CONF)
                    << key << " = " << value;
        }
    }
}

TEST(KeeperWalConf, ADurableCapThatOverflowsInBytesIsRefused)
{
    // INT64_MAX >> 20 MiB is the largest cap whose size in bytes fits in int64_t.
    chl::KeeperConfiguration largest;
    ASSERT_EQ(parseKeeper(largest, R"({"durable_cap_mb": 8796093022207})"), chl::CL_SUCCESS);
    EXPECT_EQ(largest.WAL_CONF.durable_cap_mb, 8796093022207u);
    EXPECT_EQ(largest.WAL_CONF.durableCapBytes(), 8796093022207u << 20);

    chl::KeeperConfiguration above;
    EXPECT_EQ(parseKeeper(above, R"({"durable_cap_mb": 8796093022208})"), chl::CL_ERR_INVALID_CONF);

    // 2^44 MiB is 2^64 bytes, which a plain multiply wraps to a cap of 0.
    chl::KeeperConfiguration wraps;
    EXPECT_EQ(parseKeeper(wraps, R"({"durable_cap_mb": 17592186044416})"), chl::CL_ERR_INVALID_CONF);

    chl::WalConf set_directly;
    set_directly.durable_cap_mb = 17592186044416u;
    EXPECT_EQ(set_directly.durableCapBytes(), chl::WalConf::MAX_DURABLE_CAP_MB << 20);
}

TEST(KeeperWalConf, TheReserveRoundsThePercentOfTheWholeTotal)
{
    // 10% of 199 bytes is 19.9: the reserve is 19, not the 10 of 199 / 100 * 10.
    EXPECT_EQ(chl::walReserveBytes(199, 1, 10), 19u);
    EXPECT_EQ(chl::walReserveBytes(199, 50, 10), 50u);
    EXPECT_EQ(chl::walReserveBytes(UINT64_MAX, 1, 100), UINT64_MAX);
    EXPECT_EQ(chl::walReserveBytes(1000, 1, 0), 1u);
}

TEST(KeeperWalConf, ThePercentIsBetweenZeroAndOneHundred)
{
    chl::KeeperConfiguration full;
    ASSERT_EQ(parseKeeper(full, R"({"reserve_percent": 100})"), chl::CL_SUCCESS);

    chl::KeeperConfiguration over;
    EXPECT_EQ(parseKeeper(over, R"({"reserve_percent": 101})"), chl::CL_ERR_INVALID_CONF);

    chl::KeeperConfiguration negative;
    EXPECT_EQ(parseKeeper(negative, R"({"reserve_percent": -5})"), chl::CL_ERR_INVALID_CONF);
}

TEST(KeeperWalConf, AWrongTypeIsRefused)
{
    chl::KeeperConfiguration enabled_string;
    EXPECT_EQ(parseKeeper(enabled_string, R"({"enabled": "yes"})"), chl::CL_ERR_INVALID_CONF);

    chl::KeeperConfiguration dir_number;
    EXPECT_EQ(parseKeeper(dir_number, R"({"dir": 5})"), chl::CL_ERR_INVALID_CONF);

    chl::KeeperConfiguration size_string;
    EXPECT_EQ(parseKeeper(size_string, R"({"max_bytes": "big"})"), chl::CL_ERR_INVALID_CONF);

    chl::KeeperConfiguration not_an_object;
    EXPECT_EQ(parseKeeper(not_an_object, "true"), chl::CL_ERR_INVALID_CONF);
}
