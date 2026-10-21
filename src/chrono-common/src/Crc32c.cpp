#include <cstring>

#include <boost/crc.hpp>

#include <Crc32c.h>

#if defined(__x86_64__)
#include <nmmintrin.h>
#endif

namespace chl = chronolog;

namespace
{
using Crc32cSoftware = boost::crc_optimal<32, 0x1EDC6F41, 0xFFFFFFFF, 0xFFFFFFFF, true, true>;

#if defined(__x86_64__)
// Compiled for SSE4.2 on its own, so the rest of the build keeps its flags;
// called only when the CPU reports SSE4.2.
__attribute__((target("sse4.2"))) uint32_t crc32cHardware(uint32_t crc, void const* data, std::size_t len)
{
    unsigned char const* p = static_cast<unsigned char const*>(data);
    uint64_t rem = static_cast<uint32_t>(~crc);
    while(len > 0 && (reinterpret_cast<uintptr_t>(p) & 7) != 0)
    {
        rem = _mm_crc32_u8(static_cast<uint32_t>(rem), *p++);
        --len;
    }
    while(len >= 8)
    {
        uint64_t word;
        std::memcpy(&word, p, sizeof(word));
        rem = _mm_crc32_u64(rem, word);
        p += 8;
        len -= 8;
    }
    while(len > 0)
    {
        rem = _mm_crc32_u8(static_cast<uint32_t>(rem), *p++);
        --len;
    }
    return ~static_cast<uint32_t>(rem);
}

bool detectHardware() { return __builtin_cpu_supports("sse4.2"); }
#else
bool detectHardware() { return false; }
#endif
} // namespace

uint32_t chl::crc32c_software(uint32_t crc, void const* data, std::size_t len)
{
    // boost takes the remainder before the final xor, unreflected; it keeps
    // the remainder reflected and the checksum is that remainder xor 0xFFFFFFFF
    uint32_t rem = crc ^ 0xFFFFFFFF;
    uint32_t unreflected = 0;
    for(int bit = 0; bit < 32; ++bit, rem >>= 1) { unreflected = (unreflected << 1) | (rem & 1); }
    Crc32cSoftware computer(unreflected);
    computer.process_bytes(data, len);
    return computer.checksum();
}

bool chl::crc32c_hardware_available()
{
    static bool const available = detectHardware();
    return available;
}

uint32_t chl::crc32c_extend(uint32_t crc, void const* data, std::size_t len)
{
#if defined(__x86_64__)
    if(crc32c_hardware_available())
    {
        return crc32cHardware(crc, data, len);
    }
#endif
    return crc32c_software(crc, data, len);
}
