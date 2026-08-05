#ifndef CHRONOLOG_ARCHIVE_MANIFEST_H
#define CHRONOLOG_ARCHIVE_MANIFEST_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <chronolog_types.h>

namespace chronolog
{

// The archive's own record of what has been published to it.
//
// Two things in ChronoLog depend on knowing the archive's contents, and today
// both recover that knowledge by walking the directory tree: the Player scans
// recursively to map start times to files, and a restarted Grapher has no way at
// all to recover each story's persisted watermark W. The manifest replaces the
// scan with a read, and makes W durable.
//
// On disk, in the archive root:
//
//   archive_manifest.log    append-only, one JSON object per line
//   archive_manifest.json   compacted snapshot, replaced by atomic rename
//
// Ordering rule: a record is appended only AFTER the rename that publishes its
// file, so the manifest can never name a partially written file. The cost of
// that ordering is that a crash between the two loses the trailing append --
// which is deliberately the safe direction. W then under-reports, keepers retain
// longer and re-send, and E <= W still holds. Over-reporting is impossible,
// which is why appends do not need an fsync each.
struct ArchiveManifestRecord
{
    static constexpr uint32_t CURRENT_VERSION = 1;

    // published: a real HDF5 file exists and its events are durable.
    // empty:     the chunk window held no events, so no file was written. It is
    //            still recorded, because it was persisted vacuously and dropping
    //            it would leave a hole that truncates the contiguous run and make
    //            restored W regress below its pre-restart value.
    // deleted:   the file was removed (destroy story/chronicle). Because the log
    //            is append-only, this arrives as a LATER record naming the same
    //            file, and it supersedes that file's publication rather than
    //            merely being skipped -- otherwise a file that is gone from disk
    //            would still be claimed as persisted.
    // failed:    the HDF5 write failed, so nothing was published. Recorded because
    //            an operator wants to see it, but it must never advance W: unlike
    //            "empty", its window was NOT persisted.
    enum class State : uint8_t
    {
        PUBLISHED = 0,
        EMPTY = 1,
        DELETED = 2,
        FAILED = 3
    };

    uint32_t version = CURRENT_VERSION;
    ChronicleName chronicle;
    StoryName story;
    // Carried explicitly rather than re-derived from the names. The Grapher has
    // the exact StoryId in hand when it publishes, and the Visor derives ids as
    // CityHash64(chronicle_name + story_name) -- a concatenation, so ("a","bc")
    // and ("ab","c") collide. Restoring a watermark onto the wrong story is
    // silent and unrecoverable, so the id is stored, not inferred.
    StoryId story_id = 0;
    // Empty for EMPTY records: there is no file.
    std::string file;
    // The chunk window [start, end). Events are inside it by construction, so
    // this doubles as the event-time bound the Player currently lacks.
    uint64_t start = 0;
    uint64_t end = 0;
    // Rotation index: 0 for the base name, n for "...vlen.<n>.h5".
    uint32_t seq = 0;
    uint64_t events = 0;
    State state = State::PUBLISHED;
    // Mirrors StoryChunk::isWatermarkExempt(). A salvage chunk is a real file the
    // reader must see, but its events were never merged into the contiguous
    // timeline, so it must never advance W.
    bool exempt = false;
};

using ManifestState = ArchiveManifestRecord::State;

// One record <-> one line of the log. Kept as free functions so the encoding can
// be tested without touching the filesystem.
std::string toManifestLine(ArchiveManifestRecord const& record);

// Returns false for anything that is not a complete, well-formed record --
// including the torn final line a crash mid-append leaves behind. Callers treat
// a rejected line as absent rather than trying to salvage fields from it.
bool parseManifestLine(std::string const& line, ArchiveManifestRecord& out);

class ArchiveManifest
{
public:
    explicit ArchiveManifest(std::string const& archive_root);

    // Appends one record to the log and keeps it in memory. Returns CL_SUCCESS,
    // or CL_ERR_UNKNOWN if the log could not be written.
    //
    // Thread-safe: the grapher's extraction module publishes from several streams
    // into one manifest, so appends interleave. Without serialization the writes
    // tear each other's lines on disk and race on the in-memory vector.
    int append(ArchiveManifestRecord const& record);

    // Replaces the in-memory state with the snapshot followed by the log. A
    // missing manifest is the normal first-start case and returns CL_SUCCESS
    // with no records.
    int load();

    // A snapshot by value: returning a reference would hand out a container that
    // a concurrent append can reallocate underneath the caller.
    std::vector<ArchiveManifestRecord> records() const;

    // Compacts: writes every in-memory record to <snapshot>.tmp, renames it over
    // the snapshot, then truncates the log. The rename is what makes the swap
    // atomic -- a crash leaves either the old snapshot plus the full log, or the
    // new snapshot plus a log that is still a suffix of it.
    int snapshot();

    // Per-story persisted windows, sorted by start with duplicates collapsed.
    //
    // This is the record-filtering rule in one place: exempt and failed records
    // never count, and a file that a later record deleted stops counting even
    // though its publication is still in the append-only log. Every exclusion is
    // in the under-reporting direction, which E <= W absorbs.
    std::map<StoryId, std::map<uint64_t, uint64_t>> persistedIntervals() const;

    // Per-story W: the end of the longest contiguous run of persisted windows
    // starting at that story's earliest recorded window.
    //
    // Only PUBLISHED and EMPTY records that are not exempt contribute. Every
    // exclusion is in the under-reporting direction, which E <= W absorbs.
    std::map<StoryId, uint64_t> deriveWatermarks() const;

    std::string const& logPath() const { return theLogPath; }

    std::string const& snapshotPath() const { return theSnapshotPath; }

private:
    mutable std::mutex theMutex;
    std::string theArchiveRoot;
    std::string theLogPath;
    std::string theSnapshotPath;
    std::vector<ArchiveManifestRecord> theRecords;
};

} // namespace chronolog

#endif // CHRONOLOG_ARCHIVE_MANIFEST_H
