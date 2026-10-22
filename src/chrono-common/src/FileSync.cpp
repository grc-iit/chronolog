#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <FileSync.h>

namespace chl = chronolog;

namespace
{
std::atomic<chl::FsyncFn> installedFsync{nullptr};
} // namespace

int chl::archiveFsync(int fd)
{
    FsyncFn const fn = installedFsync.load();
    return fn != nullptr ? fn(fd) : ::fsync(fd);
}

void chl::setArchiveFsyncForTest(FsyncFn fn) { installedFsync.store(fn); }

int chl::fsyncPath(std::string const& path)
{
    int const fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0)
    {
        return errno;
    }
    int error = 0;
    errno = 0;
    if(archiveFsync(fd) != 0)
    {
        error = errno != 0 ? errno : EIO;
    }
    if(::close(fd) != 0 && error == 0)
    {
        error = errno;
    }
    return error;
}
