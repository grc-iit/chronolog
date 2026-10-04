#include "keeper/wal/FileSink.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace chronolog
{
namespace
{

class PosixFileSink final: public FileSink
{
public:
    explicit PosixFileSink(const std::string& path)
    {
        fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if(fd_ < 0)
            throw std::runtime_error("open WAL: " + std::string(std::strerror(errno)));
    }
    ~PosixFileSink() override { ::close(fd_); }
    absl::Status write(std::string_view bytes) override
    {
        while(!bytes.empty())
        {
            const auto written = ::write(fd_, bytes.data(), bytes.size());
            if(written < 0 && errno == EINTR)
                continue;
            if(written <= 0)
                return absl::UnavailableError("write WAL: " + std::string(std::strerror(errno)));
            bytes.remove_prefix(static_cast<size_t>(written));
        }
        return absl::OkStatus();
    }
    absl::Status sync() override
    {
        int result;
        do {
            result = ::fdatasync(fd_);
        } while(result != 0 && errno == EINTR);
        return result == 0 ? absl::OkStatus()
                           : absl::UnavailableError("fdatasync WAL: " + std::string(std::strerror(errno)));
    }

private:
    int fd_;
};

} // namespace

std::unique_ptr<FileSink> openFileSink(const std::string& path) { return std::make_unique<PosixFileSink>(path); }

} // namespace chronolog
