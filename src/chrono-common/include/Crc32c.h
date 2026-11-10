#ifndef CHRONOLOG_CRC32C_H
#define CHRONOLOG_CRC32C_H

#include <cstddef>
#include <cstdint>

namespace chronolog
{

// CRC-32C (Castagnoli), the checksum of every keeper WAL record.
//
// Each function takes the CRC of the bytes before data and returns the CRC of
// those bytes followed by data: extend(0, data) is the CRC of data, and
// extend(extend(0, a), b) == extend(0, a + b).

// The SSE4.2 crc32 instruction when the CPU has it, otherwise crc32c_software.
// The CPU is checked once, on the first call.
uint32_t crc32c_extend(uint32_t crc, void const* data, std::size_t len);

// boost::crc_optimal<32, 0x1EDC6F41, 0xFFFFFFFF, 0xFFFFFFFF, true, true>
uint32_t crc32c_software(uint32_t crc, void const* data, std::size_t len);

// whether crc32c_extend uses the SSE4.2 instruction
bool crc32c_hardware_available();

} // namespace chronolog

#endif // CHRONOLOG_CRC32C_H
