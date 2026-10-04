#include "tier/ChunkCodec.h"
#include "tier/FileIO.h"
#include <absl/strings/cord.h>
#include <cstring>
#include <sys/stat.h>

namespace chronolog
{
namespace
{
constexpr std::string_view kVanished = "type.chronolog.io/archive-file-vanished";

absl::Status LoadError(std::string_view operation) { return ArchiveFileError(operation, errno); }

absl::StatusOr<ChunkBytes> LoadBytes(const std::filesystem::path& file, bool hdf5)
{
    const size_t limit = (hdf5 ? 512u : 256u) * 1024 * 1024;
    tier_detail::Fd fd(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
    if(fd.get() < 0)
        return LoadError("open archived chunk");
    struct stat info
    {
    };
    if(::fstat(fd.get(), &info) != 0)
        return LoadError("stat archived chunk");
    if(info.st_size < 0 || static_cast<uint64_t>(info.st_size) > limit || (hdf5 && info.st_size == 0))
        return absl::UnavailableError("invalid archived chunk file size");
    if(!S_ISREG(info.st_mode))
    {
        // Retain the streaming Proto reader's EOF semantics for nonregular inputs.
        std::vector<unsigned char> bytes;
        unsigned char part[65536];
        for(;;)
        {
            const auto count = ::read(fd.get(), part, sizeof(part));
            if(count < 0 && errno == EINTR)
                continue;
            if(count < 0)
                return LoadError("read archived chunk");
            if(count == 0)
                break;
            if(static_cast<size_t>(count) > limit - bytes.size())
                return absl::UnavailableError("chunk byte limit exceeded");
            bytes.insert(bytes.end(), part, part + count);
        }
        ChunkBytes result{std::make_unique_for_overwrite<unsigned char[]>(bytes.size()), bytes.size()};
        if(!bytes.empty())
            std::memcpy(result.data.get(), bytes.data(), bytes.size());
        return result;
    }
    const auto size = static_cast<size_t>(info.st_size);
    ChunkBytes result{std::make_unique_for_overwrite<unsigned char[]>(size), size};
    size_t offset = 0;
    while(offset < size)
    {
        const auto count = ::read(fd.get(), result.data.get() + offset, size - offset);
        if(count < 0 && errno == EINTR)
            continue;
        if(count < 0)
            return LoadError("read archived chunk");
        if(count == 0)
            return absl::UnavailableError("truncated archived chunk file");
        offset += static_cast<size_t>(count);
    }
    return result;
}

} // namespace

absl::Status ArchiveFileError(std::string_view operation, int error)
{
    auto status = absl::UnavailableError(std::string(operation) + ": " + std::strerror(error));
    if(error == ENOENT || error == ESTALE)
        status.SetPayload(kVanished, absl::Cord("1"));
    return status;
}

bool ArchiveFileVanished(const absl::Status& status) { return status.GetPayload(kVanished).has_value(); }

absl::StatusOr<ChunkBytes> LoadChunkFile(const std::filesystem::path& file)
{
    if(file.extension() != ".h5" && file.extension() != ".pb")
        return absl::UnavailableError("unknown archive codec extension");
    return LoadBytes(file, file.extension() == ".h5");
}

absl::StatusOr<std::vector<Event>> ChunkCodec::decode(std::span<unsigned char>) const
{
    return absl::UnimplementedError("codec does not support memory decoding");
}

absl::StatusOr<std::vector<Event>> ChunkCodec::read(const std::filesystem::path& file) const
{
    auto bytes = LoadBytes(file, extension() == ".h5");
    if(!bytes.ok())
        return bytes.status();
    return decode(bytes->view());
}

absl::StatusOr<std::vector<Event>> DecodeChunkFile(const std::filesystem::path& file, ChunkBytes& bytes)
{
    if(file.extension() == ".pb")
        return ProtoChunkCodec().decode(bytes.view());
    if(file.extension() == ".h5")
        return HDF5ChunkCodec().decode(bytes.view());
    return absl::UnavailableError("unknown archive codec extension");
}

absl::StatusOr<std::vector<Event>> ReadChunkFile(const std::filesystem::path& file)
{
    auto bytes = LoadChunkFile(file);
    if(!bytes.ok())
        return bytes.status();
    return DecodeChunkFile(file, *bytes);
}
} // namespace chronolog
