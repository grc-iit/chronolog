#pragma once
#include <cstdint>
namespace chronolog::client
{
using ClientId = uint64_t;
struct ClientIdentity
{
    uint32_t ip{};
    uint16_t port{};
    uint16_t instance{};
    ClientId pack() const { return (ClientId{ip} << 32) | (ClientId{port} << 16) | instance; }
    static ClientIdentity unpack(ClientId id)
    {
        return {static_cast<uint32_t>(id >> 32), static_cast<uint16_t>(id >> 16), static_cast<uint16_t>(id)};
    }
};
} // namespace chronolog::client
