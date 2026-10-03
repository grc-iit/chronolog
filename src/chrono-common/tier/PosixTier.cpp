#include "tier/PosixTier.h"
#include <absl/log/log.h>
#include <dirent.h>
#include <nlohmann/json.hpp>
#include <sys/vfs.h>

namespace chronolog
{
PosixTier::PosixTier(TierConfig tier, std::string deployment, size_t threads, std::chrono::milliseconds timeout)
    : config(std::move(tier))
    , deployment_(std::move(deployment))
    , threads_(threads)
    , timeout_(timeout)
{}

absl::StatusOr<std::string> PosixTier::read(int root, const std::string& file, bool direct)
{
    tier_detail::Fd fd(::openat(root, file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if(fd.get() < 0)
        return tier_detail::IoError("open tier file");
    if(direct)
    {
        // Unaligned chunk tails require buffered I/O; invalidate the client's cached bytes before verification.
        const auto error = ::posix_fadvise(fd.get(), 0, 0, POSIX_FADV_DONTNEED);
        if(error)
            return absl::UnavailableError("cannot invalidate tier verification cache");
        VLOG(1) << "tier verification uses POSIX_FADV_DONTNEED";
    }
    std::string bytes;
    char buffer[65536];
    for(;;)
    {
        auto n = ::read(fd.get(), buffer, sizeof(buffer));
        if(n < 0 && errno == EINTR)
            continue;
        if(n < 0)
            return tier_detail::IoError("read tier file");
        if(!n)
            return bytes;
        bytes.append(buffer, static_cast<size_t>(n));
    }
}

absl::Status PosixTier::verify(const TierDirectory& directory) const
{
    tier_detail::Fd present(::open(config.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat original
    {
    }, current{};
    if(present.get() < 0 || ::fstat(directory.fd.get(), &original) != 0 || ::fstat(present.get(), &current) != 0 ||
       original.st_dev != current.st_dev || original.st_ino != current.st_ino)
        return absl::UnavailableError("tier root replaced after probe");
    auto bytes = read(directory.fd.get(), ".chronolog-tier.json");
    if(!bytes.ok())
        return bytes.status();
    try
    {
        auto marker = nlohmann::json::parse(*bytes);
        struct statfs info
        {
        };
        if(::fstatfs(directory.fd.get(), &info) != 0)
            return tier_detail::IoError("stat tier descriptor");
        if(marker.at("deployment_id").get<std::string>() != deployment_ ||
           marker.at("name").get<std::string>() != config.name || marker.at("rank").get<uint32_t>() != config.rank ||
           marker.at("kind").get<std::string>() != config.kind ||
           marker.at("tier_uuid").get<std::string>() != config.tier_uuid ||
           marker.at("f_type").get<int64_t>() != info.f_type)
            return absl::UnavailableError("tier marker identity mismatch");
        return absl::OkStatus();
    }
    catch(const std::exception& error)
    {
        return absl::UnavailableError(error.what());
    }
}

absl::Status PosixTier::probe()
{
    if(probing_.exchange(true))
        return absl::UnavailableError("tier probe already outstanding");
    auto self = shared_from_this();
    uint64_t epoch;
    {
        std::lock_guard lock(mutex_);
        epoch = ++epoch_;
        directory_.reset();
    }
    return run(
            [self, epoch]() -> absl::Status
            {
                struct Done
                {
                    std::atomic<bool>& flag;
                    ~Done() { flag = false; }
                } done{self->probing_};
                auto directory = std::make_shared<TierDirectory>(
                        ::open(self->config.root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW),
                        epoch);
                if(directory->fd.get() < 0)
                    return tier_detail::IoError("open tier root");
                auto status = self->verify(*directory);
                if(!status.ok())
                    return status;
                std::lock_guard lock(self->mutex_);
                if(self->epoch_ != epoch)
                    return absl::UnavailableError("abandoned tier probe");
                self->directory_ = std::move(directory);
                return absl::OkStatus();
            });
}
std::shared_ptr<TierDirectory> PosixTier::directory() const
{
    std::lock_guard lock(mutex_);
    return directory_;
}
bool PosixTier::current(const std::shared_ptr<TierDirectory>& directory) const
{
    std::lock_guard lock(mutex_);
    return directory && directory_ == directory;
}
void PosixTier::unavailable()
{
    std::lock_guard lock(mutex_);
    ++epoch_;
    directory_.reset();
}

absl::Status PosixTier::erase(int root, const std::string& file)
{
    if(::unlinkat(root, file.c_str(), 0) != 0 && errno != ENOENT)
        return tier_detail::IoError("unlink tier file");
    const auto parent = std::filesystem::path(file).parent_path().string();
    tier_detail::Fd directory(::openat(root, parent.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if(directory.get() < 0)
        return errno == ENOENT ? absl::OkStatus() : tier_detail::IoError("open tier story");
    return ::fsync(directory.get()) == 0 ? absl::OkStatus() : tier_detail::IoError("sync tier story");
}
absl::StatusOr<std::vector<std::string>> PosixTier::list(int root, const std::string& path)
{
    int fd = ::openat(root, path.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if(fd < 0)
        return errno == ENOENT ? absl::StatusOr<std::vector<std::string>>(std::vector<std::string>{})
                               : tier_detail::IoError("open tier sweep directory");
    DIR* directory = ::fdopendir(fd);
    if(!directory)
    {
        ::close(fd);
        return tier_detail::IoError("list tier directory");
    }
    std::vector<std::string> names;
    errno = 0;
    while(auto* entry = ::readdir(directory))
    {
        const std::string name(entry->d_name);
        if(name != "." && name != "..")
            names.push_back(path == "." ? name : path + "/" + name);
        errno = 0;
    }
    const auto status = errno ? tier_detail::IoError("read tier directory") : absl::OkStatus();
    ::closedir(directory);
    if(!status.ok())
        return status;
    return names;
}
} // namespace chronolog
