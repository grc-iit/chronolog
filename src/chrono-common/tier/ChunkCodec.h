#pragma once

#include "chronolog/types.h"
#include <filesystem>
#include <span>
#include <string_view>
#include <memory>

namespace chronolog
{
struct ChunkBytes
{
    std::unique_ptr<unsigned char[]> data;
    size_t size{};
    std::span<unsigned char> view() const { return {data.get(), size}; }
};
// ENOENT and ESTALE at open, fstat or read mean the file vanished (an unlink, possibly by another NFS client). The
// status stays UNAVAILABLE and carries a payload that ArchiveFileVanished recognises; every other errno does not.
absl::Status ArchiveFileError(std::string_view operation, int error);
bool ArchiveFileVanished(const absl::Status& status);
absl::StatusOr<ChunkBytes> LoadChunkFile(const std::filesystem::path& file);
absl::StatusOr<std::vector<Event>> DecodeChunkFile(const std::filesystem::path& file, ChunkBytes& bytes);

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
    virtual absl::StatusOr<std::vector<Event>> read(const std::filesystem::path& file) const;
    virtual absl::StatusOr<std::vector<Event>> decode(std::span<unsigned char> bytes) const;
};

class ProtoChunkCodec final: public ChunkCodec
{
public:
    absl::Status write(const std::filesystem::path& file, std::span<const Event> events) const override;
    absl::StatusOr<std::vector<Event>> decode(std::span<unsigned char> bytes) const override;
};
class HDF5ChunkCodec final: public ChunkCodec
{
public:
    std::string extension() const override { return ".h5"; }
    absl::Status write(const std::filesystem::path& file, std::span<const Event> events) const override;
    absl::Status writeChunk(const std::filesystem::path& file, const Chunk& chunk) const override;
    absl::StatusOr<std::vector<Event>> decode(std::span<unsigned char> bytes) const override;
};

absl::StatusOr<std::vector<Event>> ReadChunkFile(const std::filesystem::path& file);
} // namespace chronolog
