#ifndef CHRONOLOG_WAL_SEGMENT_MANAGER_H
#define CHRONOLOG_WAL_SEGMENT_MANAGER_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <thallium.hpp>

#include <WalFileSink.h>

namespace chronolog
{

// The segment files of one keeper WAL directory: the lock, the cleanup of
// half-prepared segments at startup, the pre-zeroed spare the committer
// switches to on rotation, and the deletion of released segments.
//
// A spare is prepared as <n>.wal.partial: zeros over the whole segment, the H
// record at offset 0, fdatasync, rename to <n>.wal, fsync of the directory.
// Writes into it then only overwrite zeros, so a data sync never has to commit
// a size change. Segment numbers are handed out in the order spares are
// prepared, and spares are taken in that order, so the numbers follow log
// order. A spare that is never taken stays on disk as a header-only segment,
// which recovery may adopt.
//
// The sink must already have its directory open. segments(), takeSpare,
// adoptExisting and deleteSegment are for one thread at a time (recovery, then
// the committer); the preparer only touches the spare, under the manager's
// Argobots mutex.
class WalSegmentManager
{
public:
    // directory is used only to name the directory in messages
    WalSegmentManager(WalFileSink& sink,
                      uint64_t segment_bytes,
                      std::string keeper_endpoint,
                      std::string directory = {});
    ~WalSegmentManager();

    WalSegmentManager(WalSegmentManager const&) = delete;
    WalSegmentManager& operator=(WalSegmentManager const&) = delete;

    // Takes the directory lock, removes every *.partial and lists the segments.
    // Returns 0 or an errno value; EWOULDBLOCK when another keeper holds the
    // lock, and then nothing in the directory is changed. error() names the
    // directory and the reason.
    int open();
    std::string const& error() const { return lastError; }
    // the directory as given to the constructor, for messages
    std::string const& directoryName() const { return directory; }

    // Oldest first: the segments open() found, plus those taken or adopted
    // since, minus those deleted. A spare not yet taken is not listed.
    std::vector<uint64_t> const& segments() const { return segmentList; }

    // A ULT on its own tl::xstream keeps one spare ready. After a failed
    // preparation it waits for the next takeSpare instead of retrying.
    void startPreparer();
    // Joins the preparer. A spare it already prepared stays ready.
    void stopPreparer();

    // Waits (Argobots) for a spare of at least min_bytes, or creates one of
    // that size when the regular spare is smaller: the spare is extended with
    // zeros, so its number keeps its place in the order. Without the preparer
    // running, the spare is prepared on the caller's thread. The H record is
    // already at offset 0 and synced; data_offset is the end of it. size_bytes,
    // when given, receives the segment's size.
    int
    takeSpare(uint64_t min_bytes, uint64_t& segment, int& fd, uint64_t& data_offset, uint64_t* size_bytes = nullptr);
    // Recovery: reuses the newest segment open() found when it holds only its
    // H record. EINVAL when it is not the newest or holds anything else.
    int adoptExisting(uint64_t segment, int& fd, uint64_t& data_offset, uint64_t* size_bytes = nullptr);
    // Removes the segment's file, then fsyncs the directory. ENOENT, with
    // nothing changed, for a segment segments() does not list.
    int deleteSegment(uint64_t segment);

private:
    struct Spare
    {
        uint64_t segment;
        int fd;
        uint64_t data_offset;
        uint64_t size_bytes;
    };

    // Makes <segment>.wal from scratch; on failure removes what it made.
    int prepare(uint64_t segment, uint64_t bytes, Spare& spare);
    int extend(Spare& spare, uint64_t bytes);
    void preparerLoop();

    WalFileSink& sink;
    uint64_t const segmentBytes;
    std::string const keeperEndpoint;
    std::string const directory;
    std::string lastError;
    bool opened = false;
    bool segmentHandedOut = false; // a segment was taken or adopted since open()
    std::vector<uint64_t> segmentList;

    // guarded by mutex
    thallium::mutex mutex;
    thallium::condition_variable changed;
    uint64_t nextSegment = 1;
    std::optional<Spare> spare;
    bool preparing = false;      // a spare is being prepared, by the preparer or a takeSpare
    bool preparerFailed = false; // the last preparation by the preparer failed; cleared by takeSpare
    bool stopping = false;

    bool preparerRunning = false;
    thallium::managed<thallium::xstream> preparerStream;
    thallium::managed<thallium::thread> preparerThread;
};

} // namespace chronolog

#endif // CHRONOLOG_WAL_SEGMENT_MANAGER_H
