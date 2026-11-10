// Unit tests for CRC-32C. The keeper WAL checks every record against it, so the
// SSE4.2 path and the boost fallback must agree on every length and alignment:
// a keeper that wrote a segment on one CPU must be able to recover it on
// another.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <Crc32c.h>

namespace chl = chronolog;

namespace
{
std::vector<unsigned char> randomBytes(std::size_t len, uint32_t seed)
{
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int> byte(0, 255);
    std::vector<unsigned char> bytes(len);
    for(auto& b: bytes) { b = static_cast<unsigned char>(byte(gen)); }
    return bytes;
}
} // namespace

TEST(Crc32c, KnownVector)
{
    std::string const check = "123456789";
    EXPECT_EQ(0xE3069283u, chl::crc32c_software(0, check.data(), check.size()));
    EXPECT_EQ(0xE3069283u, chl::crc32c_extend(0, check.data(), check.size()));
    EXPECT_EQ(0u, chl::crc32c_software(0, check.data(), 0));
    EXPECT_EQ(0u, chl::crc32c_extend(0, check.data(), 0));
}

TEST(Crc32c, HardwareMatchesSoftware)
{
    if(!chl::crc32c_hardware_available())
    {
        GTEST_SKIP() << "no SSE4.2 on this CPU: crc32c_extend is the software path";
    }
    std::vector<unsigned char> const bytes = randomBytes(4096 + 8, 1);
    for(std::size_t offset = 0; offset < 8; ++offset)
    {
        for(std::size_t len = 0; len <= 4096; ++len)
        {
            unsigned char const* data = bytes.data() + offset;
            ASSERT_EQ(chl::crc32c_software(0, data, len), chl::crc32c_extend(0, data, len))
                    << "offset " << offset << " length " << len;
        }
    }
}

TEST(Crc32c, ExtendEqualsOneShot)
{
    std::size_t const len = 1u << 20;
    std::vector<unsigned char> const bytes = randomBytes(len, 2);
    uint32_t const whole = chl::crc32c_extend(0, bytes.data(), len);
    EXPECT_EQ(chl::crc32c_software(0, bytes.data(), len), whole);

    std::vector<std::size_t> splits = {0, 1, 7, 8, 9, 63, 64, 4095, 4096, 4097, len / 2, len - 1, len};
    std::mt19937 gen(3);
    std::uniform_int_distribution<std::size_t> anywhere(0, len);
    for(int i = 0; i < 64; ++i) { splits.push_back(anywhere(gen)); }

    for(std::size_t split: splits)
    {
        uint32_t const head = chl::crc32c_extend(0, bytes.data(), split);
        EXPECT_EQ(whole, chl::crc32c_extend(head, bytes.data() + split, len - split)) << "split at " << split;
        uint32_t const soft_head = chl::crc32c_software(0, bytes.data(), split);
        EXPECT_EQ(whole, chl::crc32c_software(soft_head, bytes.data() + split, len - split))
                << "software split at " << split;
    }
}
