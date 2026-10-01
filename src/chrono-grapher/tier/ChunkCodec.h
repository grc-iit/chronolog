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
    virtual std::string extension() const { return ".pb"; }
    virtual absl::Status writeChunk(const std::filesystem::path& file, const Chunk& chunk) const
    {
        return write(file, chunk.events);
    }
    virtual absl::Status write(const std::filesystem::path& file, std::span<const Event> events) const = 0;
    virtual absl::StatusOr<std::vector<Event>> read(const std::filesystem::path& file) const = 0;
};

class ProtoChunkCodec final: public ChunkCodec
{
public:
    absl::Status write(const std::filesystem::path& file, std::span<const Event> events) const override;
    absl::StatusOr<std::vector<Event>> read(const std::filesystem::path& file) const override;
};
class HDF5ChunkCodec final: public ChunkCodec
{
public:
    std::string extension() const override { return ".h5"; }
    absl::Status write(const std::filesystem::path& file, std::span<const Event> events) const override;
    absl::Status writeChunk(const std::filesystem::path& file, const Chunk& chunk) const override;
    absl::StatusOr<std::vector<Event>> read(const std::filesystem::path& file) const override;
};

absl::StatusOr<std::vector<Event>> ReadChunkFile(const std::filesystem::path& file);
} // namespace chronolog
