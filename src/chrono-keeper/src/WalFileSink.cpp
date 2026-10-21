#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <filesystem>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <abt.h>

#include <WalFileSink.h>

namespace chronolog
{

namespace
{

int writeAllAt(int fd, uint64_t offset, iovec const* iov, int iovcnt)
{
    // pwritev may write less than asked and takes at most IOV_MAX buffers, so
    // walk a copy of the array, trimming what has been written
    std::vector<iovec> rest(iov, iov + iovcnt);
    std::size_t first = 0;
    while(first < rest.size())
    {
        if(rest[first].iov_len == 0)
        {
            ++first;
            continue;
        }
        int count = static_cast<int>(std::min<std::size_t>(rest.size() - first, IOV_MAX));
        ssize_t written = ::pwritev(fd, &rest[first], count, static_cast<off_t>(offset));
        if(written < 0)
        {
            if(errno == EINTR)
            {
                continue;
            }
            return errno;
        }
        if(written == 0)
        {
            return EIO; // no progress on a regular file
        }
        offset += static_cast<uint64_t>(written);
        std::size_t left = static_cast<std::size_t>(written);
        while(left > 0 && first < rest.size())
        {
            if(left >= rest[first].iov_len)
            {
                left -= rest[first].iov_len;
                ++first;
            }
            else
            {
                rest[first].iov_base = static_cast<char*>(rest[first].iov_base) + left;
                rest[first].iov_len -= left;
                left = 0;
            }
        }
    }
    return 0;
}

class PosixWalFileSink: public WalFileSink
{
public:
    ~PosixWalFileSink() override
    {
        if(lockFd >= 0)
        {
            ::close(lockFd); // releases the flock
        }
        if(dirFd >= 0)
        {
            ::close(dirFd);
        }
    }

    int openDirectory(std::string const& dir) override
    {
        if(dirFd >= 0)
        {
            return EBUSY;
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if(ec)
        {
            return ec.value();
        }
        int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if(fd < 0)
        {
            return errno;
        }
        dirFd = fd;
        return 0;
    }

    int lockDirectory() override
    {
        if(dirFd < 0)
        {
            return EBADF;
        }
        if(lockFd >= 0)
        {
            return 0;
        }
        int fd = ::openat(dirFd, "lock", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if(fd < 0)
        {
            return errno;
        }
        while(::flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            if(errno == EINTR)
            {
                continue;
            }
            int err = errno;
            ::close(fd);
            return err;
        }
        lockFd = fd;
        return 0;
    }

    int createZeroed(std::string const& name, uint64_t bytes) override
    {
        if(dirFd < 0)
        {
            return EBADF;
        }
        int fd = ::openat(dirFd, name.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if(fd < 0)
        {
            return errno;
        }
        static std::string const zeros(1u << 20, '\0');
        uint64_t offset = 0;
        int err = 0;
        while(err == 0 && offset < bytes)
        {
            iovec iov{const_cast<char*>(zeros.data()),
                      static_cast<std::size_t>(std::min<uint64_t>(zeros.size(), bytes - offset))};
            err = writeAllAt(fd, offset, &iov, 1);
            offset += iov.iov_len;
        }
        if(err == 0)
        {
            err = syncData(fd);
        }
        ::close(fd);
        return err;
    }

    int openForWrite(std::string const& name, int& fd) override
    {
        if(dirFd < 0)
        {
            return EBADF;
        }
        int opened = ::openat(dirFd, name.c_str(), O_RDWR | O_CLOEXEC);
        if(opened < 0)
        {
            return errno;
        }
        fd = opened;
        return 0;
    }

    int writeAt(int fd, uint64_t offset, iovec const* iov, int iovcnt) override
    {
        return writeAllAt(fd, offset, iov, iovcnt);
    }

    int syncData(int fd) override
    {
        while(::fdatasync(fd) != 0)
        {
            if(errno != EINTR)
            {
                return errno;
            }
        }
        return 0;
    }

    int rename(std::string const& from, std::string const& to) override
    {
        if(dirFd < 0)
        {
            return EBADF;
        }
        return ::renameat(dirFd, from.c_str(), dirFd, to.c_str()) == 0 ? 0 : errno;
    }

    int remove(std::string const& name) override
    {
        if(dirFd < 0)
        {
            return EBADF;
        }
        return ::unlinkat(dirFd, name.c_str(), 0) == 0 ? 0 : errno;
    }

    int syncDirectory() override
    {
        if(dirFd < 0)
        {
            return EBADF;
        }
        return ::fsync(dirFd) == 0 ? 0 : errno;
    }

    int readFile(std::string const& name, std::string& out) override
    {
        if(dirFd < 0)
        {
            return EBADF;
        }
        int fd = ::openat(dirFd, name.c_str(), O_RDONLY | O_CLOEXEC);
        if(fd < 0)
        {
            return errno;
        }
        struct stat st;
        if(::fstat(fd, &st) != 0)
        {
            int err = errno;
            ::close(fd);
            return err;
        }
        out.assign(static_cast<std::size_t>(st.st_size), '\0');
        std::size_t done = 0;
        while(done < out.size())
        {
            ssize_t got = ::pread(fd, &out[done], out.size() - done, static_cast<off_t>(done));
            if(got < 0)
            {
                if(errno == EINTR)
                {
                    continue;
                }
                int err = errno;
                ::close(fd);
                return err;
            }
            if(got == 0)
            {
                break; // shrank while reading
            }
            done += static_cast<std::size_t>(got);
        }
        out.resize(done);
        ::close(fd);
        return 0;
    }

    int list(std::vector<std::string>& names) override
    {
        if(dirFd < 0)
        {
            return EBADF;
        }
        // fdopendir takes over the descriptor it is given
        int fd = ::dup(dirFd);
        if(fd < 0)
        {
            return errno;
        }
        DIR* dir = ::fdopendir(fd);
        if(dir == nullptr)
        {
            int err = errno;
            ::close(fd);
            return err;
        }
        ::rewinddir(dir);
        names.clear();
        int err = 0;
        while(true)
        {
            errno = 0;
            dirent* entry = ::readdir(dir);
            if(entry == nullptr)
            {
                err = errno;
                break;
            }
            std::string name = entry->d_name;
            if(name != "." && name != "..")
            {
                names.push_back(std::move(name));
            }
        }
        ::closedir(dir);
        std::sort(names.begin(), names.end());
        return err;
    }

    int freeSpace(uint64_t& free_bytes, uint64_t& total_bytes) override
    {
        if(dirFd < 0)
        {
            return EBADF;
        }
        struct statvfs st;
        if(::fstatvfs(dirFd, &st) != 0)
        {
            return errno;
        }
        free_bytes = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
        total_bytes = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
        return 0;
    }

    int closeFile(int fd) override
    {
        // the descriptor is gone even on EINTR, so close is never retried
        return ::close(fd) == 0 ? 0 : errno;
    }

private:
    int dirFd = -1;
    int lockFd = -1;
};

// Lets other Argobots threads on the caller's stream run; outside Argobots
// there is nothing to yield to.
void waitBriefly()
{
    ABT_unit_type type;
    if(ABT_initialized() == ABT_SUCCESS && ABT_self_get_type(&type) == ABT_SUCCESS && type == ABT_UNIT_TYPE_THREAD)
    {
        ABT_self_yield();
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}

} // namespace

std::unique_ptr<WalFileSink> makePosixWalFileSink() { return std::make_unique<PosixWalFileSink>(); }

FaultInjectingWalFileSink::FaultInjectingWalFileSink(std::unique_ptr<WalFileSink> inner)
    : inner(std::move(inner))
{}

void FaultInjectingWalFileSink::failSyncsWith(int err) { syncError = err; }

void FaultInjectingWalFileSink::failWritesWith(int err) { writeError = err; }

void FaultInjectingWalFileSink::setFreeSpace(uint64_t free_bytes, uint64_t total_bytes)
{
    std::lock_guard<std::mutex> lock(freeSpaceMutex);
    freeSpaceSet = true;
    fakeFreeBytes = free_bytes;
    fakeTotalBytes = total_bytes;
}

void FaultInjectingWalFileSink::holdSyncs() { syncsHeld = true; }

void FaultInjectingWalFileSink::releaseSyncs() { syncsHeld = false; }

uint64_t FaultInjectingWalFileSink::syncCount() const { return syncs.load(); }

int FaultInjectingWalFileSink::openDirectory(std::string const& dir) { return inner->openDirectory(dir); }

int FaultInjectingWalFileSink::lockDirectory() { return inner->lockDirectory(); }

int FaultInjectingWalFileSink::createZeroed(std::string const& name, uint64_t bytes)
{
    if(int err = writeError.load())
    {
        return err;
    }
    return inner->createZeroed(name, bytes);
}

int FaultInjectingWalFileSink::openForWrite(std::string const& name, int& fd) { return inner->openForWrite(name, fd); }

int FaultInjectingWalFileSink::writeAt(int fd, uint64_t offset, iovec const* iov, int iovcnt)
{
    if(int err = writeError.load())
    {
        return err;
    }
    return inner->writeAt(fd, offset, iov, iovcnt);
}

int FaultInjectingWalFileSink::syncData(int fd)
{
    ++syncs;
    while(syncsHeld.load()) { waitBriefly(); }
    if(int err = syncError.load())
    {
        return err;
    }
    return inner->syncData(fd);
}

int FaultInjectingWalFileSink::rename(std::string const& from, std::string const& to)
{
    return inner->rename(from, to);
}

int FaultInjectingWalFileSink::remove(std::string const& name) { return inner->remove(name); }

int FaultInjectingWalFileSink::syncDirectory() { return inner->syncDirectory(); }

int FaultInjectingWalFileSink::readFile(std::string const& name, std::string& out)
{
    return inner->readFile(name, out);
}

int FaultInjectingWalFileSink::list(std::vector<std::string>& names) { return inner->list(names); }

int FaultInjectingWalFileSink::freeSpace(uint64_t& free_bytes, uint64_t& total_bytes)
{
    {
        std::lock_guard<std::mutex> lock(freeSpaceMutex);
        if(freeSpaceSet)
        {
            free_bytes = fakeFreeBytes;
            total_bytes = fakeTotalBytes;
            return 0;
        }
    }
    return inner->freeSpace(free_bytes, total_bytes);
}

int FaultInjectingWalFileSink::closeFile(int fd) { return inner->closeFile(fd); }

} // namespace chronolog
