#ifndef CHRONOLOG_ARCHIVE_MANIFEST_H
#define CHRONOLOG_ARCHIVE_MANIFEST_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <chronolog_types.h>

namespace chronolog
{

// The archive manifest: what the graphers have published to the HDF5 archive.
//
// Each grapher appends to its own log, <archive>/%manifest/<writer>.log, one
// JSON record per line, and players read every log to find the files of a
// story instead of listing the archive directories. A record is appended only
// after its file is in place under its final name, so a reader never learns of
// a file that is not there yet; the grapher reports a window written only after
// its record is appended, so a keeper never frees a chunk before a player can
// find its file.
//
// One log per writer, because NFS has no append: each client computes the end
// of the file from its own cached size, and two clients appending to one file
// overwrite each other's records.
//
// The directory name starts with '%', which the archive's name encoding never
// produces on its own (a '%' in a chronicle name is written as %25), so no
// chronicle directory can be named like it.
inline constexpr char kArchiveManifestDirName[] = "%manifest";

struct ArchiveManifestRecord
{
    enum class Op
    {
        // a file was published: file, start, end and events are set
        PUBLISH,
        // the story's files were deleted, or every story of the chronicle
        // when whole_chronicle is set
        DELETE,
        // the first line of a log: writer and start (the time the log was
        // created, ns) are set. It makes the first line unique, so a reader can
        // tell a log deleted and created again on the same inode number from
        // the one it was reading. ArchiveManifestTail does not return it.
        OPEN
    };

    Op op = Op::PUBLISH;
    ChronicleName chronicle;
    StoryName story;
    bool whole_chronicle = false;
    // the file's path relative to the archive root
    std::string file;
    // the writer id of an OPEN record
    std::string writer;
    // A DELETE can cover only some of its log's earlier files: those of an
    // earlier process of the writer, and those of the process that started at
    // writer_start with an incarnation up to incarnation_bound (see
    // ArchiveLayout.h and HDF5FileChunkExtractor's DestroyScope). 0 and
    // UINT64_MAX: every file the log recorded before it.
    uint64_t writer_start = 0;
    uint64_t incarnation_bound = UINT64_MAX;
    // the time range [start, end) the file covers, in nanoseconds
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t events = 0;
};

// One record as one line, without the newline. Names are JSON strings, so any
// character in them is escaped and a record never spans lines.
std::string toManifestLine(ArchiveManifestRecord const& record);

// false for anything that is not a complete, well-formed record, including a
// line an interrupted append cut short
bool parseManifestLine(std::string const& line, ArchiveManifestRecord& record);

// Every log in <archive_root>/%manifest, sorted. Empty when the directory does
// not exist. listing_failed, when given, is set when the directory exists but
// could not be listed in full.
std::vector<std::string> listArchiveManifestLogs(std::string const& archive_root, bool* listing_failed = nullptr);

// A grapher's log. Thread-safe: every extraction stream of a grapher publishes
// through one writer.
class ArchiveManifestWriter
{
public:
    // writer_id names the log. It must stay the same across restarts of the
    // same grapher (the recording group id does).
    ArchiveManifestWriter(std::string const& archive_root, std::string const& writer_id);

    // Creates the manifest directory and the log if they do not exist yet. An
    // archive root that does not exist is an error; it is never created.
    int open();

    // Appends one record. Each append opens and closes the log, so a reader on
    // another NFS client that opens it afterwards sees the record.
    int append(ArchiveManifestRecord const& record);

    std::string const& logPath() const { return theLogPath; }

private:
    std::mutex theMutex;
    // Creates the manifest directory if the archive root exists. Caller holds
    // theMutex.
    bool createManifestDirectory();

    // the OPEN record that starts a log this writer creates
    std::string headerLine() const;

    std::string theWriterId;
    std::string theArchiveRoot;
    std::string theManifestDir;
    std::string theLogPath;
    // the log may end in a partial line (an append was cut short, in this
    // run or an earlier one): start the next record on a line of its own
    bool theLogNeedsNewline = false;
};

// Follows one log: each call returns the records appended since the last.
// Not thread-safe.
class ArchiveManifestTail
{
public:
    explicit ArchiveManifestTail(std::string const& log_path);

    // Appends to records the complete records added since the last call. A
    // missing log reads as empty; a log that cannot be opened or read is an
    // error. A log replaced under the same name is read from its start.
    //
    // max_bytes bounds how much of the log one call reads (a line longer than
    // that is still read whole), so a reader catching up on a long log can
    // take it in pieces: call again until offset() stops moving.
    int readNew(std::vector<ArchiveManifestRecord>& records, std::size_t max_bytes = SIZE_MAX);

    std::string const& logPath() const { return theLogPath; }

    // how far into the log the records read so far reach
    uint64_t offset() const { return theOffset; }

private:
    // false when the file under the name no longer starts with the first line
    // read before: a log deleted and created again on the same inode number
    bool startsAsBefore(int fd) const;

    std::string theLogPath;
    uint64_t theOffset = 0;
    uint64_t theInode = 0;
    // the log's first line, up to 64 KiB
    std::string theHead;
};

} // namespace chronolog

#endif // CHRONOLOG_ARCHIVE_MANIFEST_H
