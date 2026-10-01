#pragma once

#include "chronolog/types.h"
#include <filesystem>
#include <span>

namespace chronolog
{
class ChunkCodec
{
public:
    virtual ~ChunkCodec() = default;
    virtual absl::Status write(const std::filesystem::path& file, std::span<const Event> events) const = 0;
    virtual absl::StatusOr<std::vector<Event>> read(const std::filesystem::path& file) const = 0;
};

class ProtoChunkCodec final: public ChunkCodec
{
public:
    absl::Status write(const std::filesystem::path& file, std::span<const Event> events) const override;
    absl::StatusOr<std::vector<Event>> read(const std::filesystem::path& file) const override;
};
} // namespace chronolog
