#ifndef CHRONOLOG_WAL_FILE_SINK_H
#define CHRONOLOG_WAL_FILE_SINK_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sys/uio.h>

namespace chronolog
{

// The file operations the keeper WAL uses, behind an interface so tests can
// inject failures and stalls. Every call returns 0 or an errno value. File
// names are relative to the directory given to openDirectory.
class WalFileSink
{
public:
    virtual ~WalFileSink() = default;

    // Creates the directory (and its parents) if missing and opens it.
    virtual int openDirectory(std::string const& dir) = 0;
    // flock(<dir>/lock, LOCK_EX | LOCK_NB), held until the sink is destroyed.
    // EWOULDBLOCK when another open file holds it, in this process or another.
    virtual int lockDirectory() = 0;
    // Creates or truncates the file, writes `bytes` zeros (not fallocate, so a
    // later overwrite commits no block allocation), then fdatasyncs it.
    virtual int createZeroed(std::string const& name, uint64_t bytes) = 0;
    // Opens an existing file for reading and writing.
    virtual int openForWrite(std::string const& name, int& fd) = 0;
    // pwritev at `offset`; retries EINTR and short writes until every byte is
    // written, and splits iovec arrays longer than IOV_MAX.
    virtual int writeAt(int fd, uint64_t offset, iovec const* iov, int iovcnt) = 0;
    // fdatasync
    virtual int syncData(int fd) = 0;
    virtual int rename(std::string const& from, std::string const& to) = 0;
    virtual int remove(std::string const& name) = 0;
    // fsync of the directory, so creates, renames and removes survive a crash
    virtual int syncDirectory() = 0;
    virtual int readFile(std::string const& name, std::string& out) = 0;
    // Names of the directory's entries, without "." and "..", sorted.
    virtual int list(std::vector<std::string>& names) = 0;
    // statvfs of the directory: free_bytes from f_bavail, total from f_blocks.
    virtual int freeSpace(uint64_t& free_bytes, uint64_t& total_bytes) = 0;
    virtual int closeFile(int fd) = 0;
};

std::unique_ptr<WalFileSink> makePosixWalFileSink();

// A destroy the WAL could not log as a D record because it had latched failed
// (see KeeperWal). Every event of the story in a segment up to last_segment
// belongs to the destroyed story: a latched WAL writes nothing more, so a
// story created again later has its events only in newer segments.
struct UnloggedDrop
{
    uint64_t story_id = 0;
    uint64_t last_segment = 0;
};

// The file in the WAL directory that holds them, one "<story id> <segment>"
// line each. Recovery applies it and removes entries whose segments are gone.
constexpr char kUnloggedDropsFile[] = "dropped-unlogged";

// A missing file is no drops. EINVAL for a line that does not parse.
int readUnloggedDrops(WalFileSink& sink, std::vector<UnloggedDrop>& drops);
// Replaces the file whole (written to a .partial, synced, renamed, directory
// synced), or removes it when there are no drops.
int writeUnloggedDrops(WalFileSink& sink, std::vector<UnloggedDrop> const& drops);

// Test sink: wraps a real one, injects failures and stalls. The failure and
// hold settings may be changed from any thread while the sink is in use.
// createZeroed fdatasyncs through the inner sink, so the syncData settings and
// syncCount do not apply to that sync; failWritesWith does fail createZeroed.
class FaultInjectingWalFileSink: public WalFileSink
{
public:
    explicit FaultInjectingWalFileSink(std::unique_ptr<WalFileSink> inner);

    // Every later syncData returns err without syncing; 0 turns it off.
    // syncDirectory is not affected.
    void failSyncsWith(int err);
    // The next count syncData calls return err without syncing; the ones after
    // them sync, or follow failSyncsWith. A call is counted when it starts, so
    // a sync already waiting in holdSyncs is not one of them.
    void failNextSyncsWith(int err, uint64_t count = 1);
    // Every later syncDirectory returns err without syncing; 0 turns it off.
    void failDirectorySyncsWith(int err);
    // Every later writeAt and createZeroed returns err without writing (e.g.
    // ENOSPC); 0 turns it off.
    void failWritesWith(int err);
    // The next writeAt that starts at offset writes at most bytes of its
    // buffers, then returns err: a write that fails partway. Writes at other
    // offsets, and those after it, are not affected; failWritesWith comes first.
    void failNextWriteAt(uint64_t offset, uint64_t bytes, int err);
    // Later freeSpace calls report these values instead of the real ones.
    void setFreeSpace(uint64_t free_bytes, uint64_t total_bytes);
    // Later syncData calls wait until releaseSyncs; syncDirectory is not held.
    // Inside an Argobots thread the wait only yields, so other threads on the
    // caller's stream run and the stream spins while nothing else is ready;
    // outside Argobots it sleeps 100 us between checks.
    void holdSyncs();
    void releaseSyncs();
    // Number of syncData calls so far, counted on entry, so a held sync counts.
    uint64_t syncCount() const;
    // Number of syncDirectory calls so far, failed ones included.
    uint64_t directorySyncCount() const;

    int openDirectory(std::string const& dir) override;
    int lockDirectory() override;
    int createZeroed(std::string const& name, uint64_t bytes) override;
    int openForWrite(std::string const& name, int& fd) override;
    int writeAt(int fd, uint64_t offset, iovec const* iov, int iovcnt) override;
    int syncData(int fd) override;
    int rename(std::string const& from, std::string const& to) override;
    int remove(std::string const& name) override;
    int syncDirectory() override;
    int readFile(std::string const& name, std::string& out) override;
    int list(std::vector<std::string>& names) override;
    int freeSpace(uint64_t& free_bytes, uint64_t& total_bytes) override;
    int closeFile(int fd) override;

private:
    std::unique_ptr<WalFileSink> inner;
    std::atomic<int> syncError{0};
    std::atomic<int> directorySyncError{0};
    std::atomic<int> writeError{0};
    std::atomic<bool> syncsHeld{false};
    std::atomic<uint64_t> syncs{0};
    std::atomic<uint64_t> directorySyncs{0};
    // guarded by nextFaultMutex
    std::mutex nextFaultMutex;
    int nextSyncError = 0;
    uint64_t nextSyncFailures = 0;
    bool nextWriteArmed = false;
    uint64_t nextWriteOffset = 0;
    uint64_t nextWriteBytes = 0;
    int nextWriteError = 0;
    std::mutex freeSpaceMutex;
    bool freeSpaceSet = false;
    uint64_t fakeFreeBytes = 0;
    uint64_t fakeTotalBytes = 0;
};

} // namespace chronolog

#endif // CHRONOLOG_WAL_FILE_SINK_H
