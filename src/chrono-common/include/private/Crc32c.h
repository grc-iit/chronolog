#ifndef CHRONOLOG_CRC32C_H
#define CHRONOLOG_CRC32C_H
#include <cstddef>
#include <cstdint>
namespace chronolog::rpc
{
namespace detail
{
// Slicing-by-8 tables for the reflected Castagnoli polynomial 0x82F63B78.
struct Crc32cTables
{
    uint32_t table[8][256];
    Crc32cTables()
    {
        for(uint32_t i = 0; i < 256; ++i)
        {
            uint32_t crc = i;
            for(int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0x82F63B78u : 0u);
            table[0][i] = crc;
        }
        for(uint32_t i = 0; i < 256; ++i)
            for(int k = 1; k < 8; ++k) table[k][i] = (table[k - 1][i] >> 8) ^ table[0][table[k - 1][i] & 0xffu];
    }
};
inline Crc32cTables const& crc32cTables()
{
    static Crc32cTables const tables;
    return tables;
}
} // namespace detail

// CRC-32C (Castagnoli), the checksum the sprint's chunk transfer carries.
// Pass an earlier result as crc to continue it over more bytes.
inline uint32_t crc32c(void const* data, std::size_t size, uint32_t crc = 0) noexcept
{
    auto const& t = detail::crc32cTables().table;
    auto const* p = static_cast<unsigned char const*>(data);
    crc = ~crc;
    while(size >= 8)
    {
        uint32_t const low = crc ^ (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
        uint32_t const high = uint32_t(p[4]) | uint32_t(p[5]) << 8 | uint32_t(p[6]) << 16 | uint32_t(p[7]) << 24;
        crc = t[7][low & 0xffu] ^ t[6][(low >> 8) & 0xffu] ^ t[5][(low >> 16) & 0xffu] ^ t[4][low >> 24] ^
              t[3][high & 0xffu] ^ t[2][(high >> 8) & 0xffu] ^ t[1][(high >> 16) & 0xffu] ^ t[0][high >> 24];
        p += 8;
        size -= 8;
    }
    while(size--) crc = t[0][(crc ^ *p++) & 0xffu] ^ (crc >> 8);
    return ~crc;
}
} // namespace chronolog::rpc
#endif
