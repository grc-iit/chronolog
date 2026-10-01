#pragma once

#include <absl/status/status.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>

namespace chronolog::tier_detail
{
inline absl::Status IoError(std::string_view operation)
{
    const int error = errno;
    const auto message = std::string(operation) + ": " + std::strerror(error);
    return error == ENOSPC || error == EDQUOT ? absl::ResourceExhaustedError(message) : absl::UnavailableError(message);
}

class Fd
{
public:
    explicit Fd(int value)
        : value_(value)
    {}
    ~Fd()
    {
        if(value_ >= 0)
            ::close(value_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }

private:
    int value_;
};

inline absl::Status WriteAll(int fd, std::string_view bytes)
{
    while(!bytes.empty())
    {
        const auto n = ::write(fd, bytes.data(), bytes.size());
        if(n < 0 && errno == EINTR)
            continue;
        if(n <= 0)
            return IoError("write");
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return absl::OkStatus();
}

inline absl::Status SyncDirectory(const std::filesystem::path& directory)
{
    Fd fd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if(fd.get() < 0)
        return IoError("open directory");
    if(::fsync(fd.get()) != 0)
        return IoError("fsync directory");
    return absl::OkStatus();
}
} // namespace chronolog::tier_detail
