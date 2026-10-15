#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <ArchiveManifest.h>
#include <chronolog_errcode.h>

namespace fs = std::filesystem;
namespace chl = chronolog;

namespace
{
class ArchiveManifestTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        root = fs::temp_directory_path() / ("archive_manifest_test_" + std::to_string(::getpid()) + "_" +
                                            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root);
        fs::create_directories(root);
    }

    void TearDown() override { fs::remove_all(root); }

    fs::path root;
};

chl::ArchiveManifestRecord publication(std::string const& chronicle,
                                       std::string const& story,
                                       std::string const& file,
                                       uint64_t start,
                                       uint64_t end)
{
    chl::ArchiveManifestRecord record;
    record.op = chl::ArchiveManifestRecord::Op::PUBLISH;
    record.chronicle = chronicle;
    record.story = story;
    record.file = file;
    record.start = start;
    record.end = end;
    record.events = 3;
    return record;
}

chl::ArchiveManifestRecord storyDeletion(std::string const& chronicle, std::string const& story)
{
    chl::ArchiveManifestRecord record;
    record.op = chl::ArchiveManifestRecord::Op::DELETE;
    record.chronicle = chronicle;
    record.story = story;
    return record;
}

void appendRaw(std::string const& path, std::string const& bytes)
{
    std::ofstream out(path, std::ios::app | std::ios::binary);
    out << bytes;
}
} // namespace

TEST(ArchiveManifestLine, PublicationRoundTripsWithAwkwardNames)
{
    // names are the client's identity and may hold anything a string can
    chl::ArchiveManifestRecord const record = publication("a.b/c\"d}{\\",
                                                          "line\nbreak %2F ünï \\\"}",
                                                          "some/dir/1700000000.vlen.h5",
                                                          1700000000000000000ULL,
                                                          1700000030000000000ULL);
    std::string const line = chl::toManifestLine(record);
    EXPECT_EQ(line.find('\n'), std::string::npos);

    chl::ArchiveManifestRecord parsed;
    ASSERT_TRUE(chl::parseManifestLine(line, parsed));
    EXPECT_EQ(parsed.op, chl::ArchiveManifestRecord::Op::PUBLISH);
    EXPECT_EQ(parsed.chronicle, record.chronicle);
    EXPECT_EQ(parsed.story, record.story);
    EXPECT_EQ(parsed.file, record.file);
    EXPECT_EQ(parsed.start, record.start);
    EXPECT_EQ(parsed.end, record.end);
    EXPECT_EQ(parsed.events, record.events);
}

TEST(ArchiveManifestLine, DeletionOfAStoryAndOfAChronicleRoundTrip)
{
    chl::ArchiveManifestRecord parsed;
    ASSERT_TRUE(chl::parseManifestLine(chl::toManifestLine(storyDeletion("c", "s")), parsed));
    EXPECT_EQ(parsed.op, chl::ArchiveManifestRecord::Op::DELETE);
    EXPECT_EQ(parsed.chronicle, "c");
    EXPECT_FALSE(parsed.whole_chronicle);
    EXPECT_EQ(parsed.story, "s");

    chl::ArchiveManifestRecord chronicle_deletion;
    chronicle_deletion.op = chl::ArchiveManifestRecord::Op::DELETE;
    chronicle_deletion.chronicle = "c";
    chronicle_deletion.whole_chronicle = true;
    ASSERT_TRUE(chl::parseManifestLine(chl::toManifestLine(chronicle_deletion), parsed));
    EXPECT_EQ(parsed.op, chl::ArchiveManifestRecord::Op::DELETE);
    EXPECT_TRUE(parsed.whole_chronicle);
    EXPECT_EQ(parsed.chronicle, "c");
}

TEST(ArchiveManifestLine, TornAndMalformedLinesAreRejected)
{
    std::string const line = chl::toManifestLine(publication("c", "s", "f.h5", 1, 2));
    chl::ArchiveManifestRecord parsed;
    EXPECT_FALSE(chl::parseManifestLine(line.substr(0, line.size() / 2), parsed));
    EXPECT_FALSE(chl::parseManifestLine("", parsed));
    EXPECT_FALSE(chl::parseManifestLine("[1,2]", parsed));
    EXPECT_FALSE(chl::parseManifestLine(R"({"op":"publish","chronicle":"c","story":"s"})", parsed));
    EXPECT_FALSE(chl::parseManifestLine(R"({"op":"rename","chronicle":"c","story":"s"})", parsed));
    EXPECT_FALSE(chl::parseManifestLine(R"({"op":"delete"})", parsed));
}

// Numbers are stored as int64 so that json-c 0.13 can read them; a negative
// one is not a time or a count.
TEST(ArchiveManifestLine, ANegativeNumberIsRejected)
{
    chl::ArchiveManifestRecord parsed;
    EXPECT_FALSE(chl::parseManifestLine(
            R"({"op":"publish","chronicle":"c","story":"s","file":"f.h5","start":-1,"end":10,"events":1})",
            parsed));
    EXPECT_TRUE(chl::parseManifestLine(
            R"({"op":"publish","chronicle":"c","story":"s","file":"f.h5","start":0,"end":10,"events":1})",
            parsed));
}

// Two appends that ran together on one line: neither is taken, rather than
// the first silently standing for both.
TEST(ArchiveManifestLine, TwoRecordsOnOneLineAreRejected)
{
    std::string const first = chl::toManifestLine(publication("c", "s", "a.h5", 1, 2));
    std::string const second = chl::toManifestLine(publication("c", "s", "b.h5", 2, 3));
    chl::ArchiveManifestRecord parsed;
    EXPECT_FALSE(chl::parseManifestLine(first + second, parsed));
    EXPECT_FALSE(chl::parseManifestLine(first + "x", parsed));
    EXPECT_TRUE(chl::parseManifestLine(first, parsed));
}

TEST_F(ArchiveManifestTest, OpeningAWriterCreatesItsLogInTheManifestDirectory)
{
    chl::ArchiveManifestWriter writer(root.string(), "7");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    EXPECT_EQ(fs::path(writer.logPath()), root / chl::kArchiveManifestDirName / "7.log");
    EXPECT_TRUE(fs::is_regular_file(writer.logPath()));
    EXPECT_EQ(chl::listArchiveManifestLogs(root.string()), std::vector<std::string>{writer.logPath()});
}

TEST_F(ArchiveManifestTest, OneLogPerWriterAndOnlyLogsAreListed)
{
    chl::ArchiveManifestWriter first(root.string(), "1");
    chl::ArchiveManifestWriter second(root.string(), "2");
    ASSERT_EQ(first.open(), chl::CL_SUCCESS);
    ASSERT_EQ(second.open(), chl::CL_SUCCESS);
    appendRaw((root / chl::kArchiveManifestDirName / "notes.txt").string(), "x");

    std::vector<std::string> const logs = chl::listArchiveManifestLogs(root.string());
    EXPECT_EQ(logs, (std::vector<std::string>{first.logPath(), second.logPath()}));
}

TEST_F(ArchiveManifestTest, NoManifestDirectoryListsNoLogs)
{
    EXPECT_TRUE(chl::listArchiveManifestLogs(root.string()).empty());
}

TEST_F(ArchiveManifestTest, TailReturnsAppendedRecordsOnce)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    chl::ArchiveManifestTail tail(writer.logPath());

    std::vector<chl::ArchiveManifestRecord> records;
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    EXPECT_TRUE(records.empty());

    ASSERT_EQ(writer.append(publication("c", "s", "a.h5", 0, 10)), chl::CL_SUCCESS);
    ASSERT_EQ(writer.append(storyDeletion("c", "s")), chl::CL_SUCCESS);
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].file, "a.h5");
    EXPECT_EQ(records[1].op, chl::ArchiveManifestRecord::Op::DELETE);

    records.clear();
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    EXPECT_TRUE(records.empty());

    ASSERT_EQ(writer.append(publication("c", "s", "b.h5", 10, 20)), chl::CL_SUCCESS);
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].file, "b.h5");
}

TEST_F(ArchiveManifestTest, TailWaitsForARecordToBeComplete)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    chl::ArchiveManifestTail tail(writer.logPath());

    std::string const line = chl::toManifestLine(publication("c", "s", "a.h5", 0, 10));
    appendRaw(writer.logPath(), line.substr(0, 10));
    std::vector<chl::ArchiveManifestRecord> records;
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    EXPECT_TRUE(records.empty());

    appendRaw(writer.logPath(), line.substr(10) + "\n");
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].file, "a.h5");
}

// A crash in the middle of an append leaves a line without its newline. The
// next append must not be glued onto it, or both records are lost.
TEST_F(ArchiveManifestTest, ATornLineLeftByAnEarlierRunDoesNotSwallowTheNextRecord)
{
    {
        chl::ArchiveManifestWriter writer(root.string(), "1");
        ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
        ASSERT_EQ(writer.append(publication("c", "s", "a.h5", 0, 10)), chl::CL_SUCCESS);
        appendRaw(writer.logPath(), R"({"op":"publish","chron)");
    }
    chl::ArchiveManifestWriter restarted(root.string(), "1");
    ASSERT_EQ(restarted.open(), chl::CL_SUCCESS);
    ASSERT_EQ(restarted.append(publication("c", "s", "b.h5", 10, 20)), chl::CL_SUCCESS);

    chl::ArchiveManifestTail tail(restarted.logPath());
    std::vector<chl::ArchiveManifestRecord> records;
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].file, "a.h5");
    EXPECT_EQ(records[1].file, "b.h5");
}

// A log replaced under the same name (removed and written again) is read from
// its start, not from the old offset.
TEST_F(ArchiveManifestTest, TailRereadsALogReplacedUnderItsName)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    ASSERT_EQ(writer.append(publication("c", "s", "a.h5", 0, 10)), chl::CL_SUCCESS);
    ASSERT_EQ(writer.append(publication("c", "s", "b.h5", 10, 20)), chl::CL_SUCCESS);
    chl::ArchiveManifestTail tail(writer.logPath());
    std::vector<chl::ArchiveManifestRecord> records;
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 2u);

    fs::path const replacement = fs::path(writer.logPath()).replace_extension(".new");
    appendRaw(replacement.string(), chl::toManifestLine(publication("c", "s", "z.h5", 0, 10)) + "\n");
    fs::rename(replacement, writer.logPath());

    records.clear();
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].file, "z.h5");
}

// A player catching up on a long log takes it a piece at a time and still
// gets every record, in order.
TEST_F(ArchiveManifestTest, TailReadsALongLogInPieces)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    for(uint64_t i = 0; i < 10; ++i)
    {
        ASSERT_EQ(writer.append(publication("c", "s", std::to_string(i) + ".h5", i, i + 1)), chl::CL_SUCCESS);
    }
    chl::ArchiveManifestTail tail(writer.logPath());
    std::vector<chl::ArchiveManifestRecord> records;
    ASSERT_EQ(tail.readNew(records, 64), chl::CL_SUCCESS);
    EXPECT_LT(records.size(), 10u);
    int calls = 1;
    for(uint64_t before = 0; before != tail.offset(); ++calls)
    {
        before = tail.offset();
        ASSERT_EQ(tail.readNew(records, 64), chl::CL_SUCCESS);
    }
    EXPECT_GT(calls, 2);
    ASSERT_EQ(records.size(), 10u);
    for(uint64_t i = 0; i < 10; ++i) { EXPECT_EQ(records[i].start, i); }
}

TEST_F(ArchiveManifestTest, ALineLongerThanAPieceIsReadWhole)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    ASSERT_EQ(writer.append(publication(std::string(300, 'c'), "s", "a.h5", 0, 10)), chl::CL_SUCCESS);
    chl::ArchiveManifestTail tail(writer.logPath());
    std::vector<chl::ArchiveManifestRecord> records;
    // each call takes one line, however long: the log's header, then the record
    for(uint64_t before = 1; before != tail.offset();)
    {
        before = tail.offset();
        std::size_t const had = records.size();
        ASSERT_EQ(tail.readNew(records, 16), chl::CL_SUCCESS);
        EXPECT_LE(records.size(), had + 1);
    }
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].chronicle, std::string(300, 'c'));
}

TEST_F(ArchiveManifestTest, TailOfAMissingLogReadsNothing)
{
    chl::ArchiveManifestTail tail((root / "absent.log").string());
    std::vector<chl::ArchiveManifestRecord> records;
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    EXPECT_TRUE(records.empty());
}

TEST_F(ArchiveManifestTest, ConcurrentAppendsEachLandAsAWholeRecord)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::vector<std::thread> threads;
    for(int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
                [&writer, t]()
                {
                    for(int i = 0; i < kPerThread; ++i)
                    {
                        std::string const file = std::to_string(t) + "_" + std::to_string(i) + ".h5";
                        EXPECT_EQ(writer.append(publication("c", "s", file, i, i + 1)), chl::CL_SUCCESS);
                    }
                });
    }
    for(auto& thread: threads) { thread.join(); }

    chl::ArchiveManifestTail tail(writer.logPath());
    std::vector<chl::ArchiveManifestRecord> records;
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    std::set<std::string> files;
    for(auto const& record: records) { files.insert(record.file); }
    EXPECT_EQ(records.size(), static_cast<std::size_t>(kThreads * kPerThread));
    EXPECT_EQ(files.size(), static_cast<std::size_t>(kThreads * kPerThread));
}

// A log that is there but cannot be read may hold records a replay needs: an
// error, not an empty log.
TEST_F(ArchiveManifestTest, TailOfALogThatCannotBeOpenedIsAnError)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    ASSERT_EQ(writer.append(publication("c", "s", "a.h5", 0, 10)), chl::CL_SUCCESS);
    fs::permissions(writer.logPath(), fs::perms::none);
    if(::access(writer.logPath().c_str(), R_OK) == 0)
    {
        GTEST_SKIP() << "permissions do not apply (running as root)";
    }
    chl::ArchiveManifestTail tail(writer.logPath());
    std::vector<chl::ArchiveManifestRecord> records;
    EXPECT_NE(tail.readNew(records), chl::CL_SUCCESS);
    EXPECT_TRUE(records.empty());

    // and nothing is skipped once it can be read again
    fs::permissions(writer.logPath(), fs::perms::owner_read | fs::perms::owner_write);
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    EXPECT_EQ(records.size(), 1u);
}

TEST_F(ArchiveManifestTest, TailOfALogThatCannotBeReadIsAnError)
{
    fs::create_directories(root / "dir.log"); // opens, but every read fails
    chl::ArchiveManifestTail tail((root / "dir.log").string());
    std::vector<chl::ArchiveManifestRecord> records;
    EXPECT_NE(tail.readNew(records), chl::CL_SUCCESS);
}

// Someone cleaning the archive removed the manifest directory under a running
// grapher. Failing every append from then on would fail every window.
TEST_F(ArchiveManifestTest, AppendCreatesTheManifestDirectoryAgainWhenItIsRemoved)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    fs::remove_all(root / chl::kArchiveManifestDirName);

    ASSERT_EQ(writer.append(publication("c", "s", "a.h5", 0, 10)), chl::CL_SUCCESS);
    chl::ArchiveManifestTail tail(writer.logPath());
    std::vector<chl::ArchiveManifestRecord> records;
    EXPECT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    EXPECT_EQ(records.size(), 1u);
}

// A missing archive root is an unmounted file system or a wrong path: the
// writer must not create it on the local disk under the mount point.
TEST_F(ArchiveManifestTest, AWriterNeverCreatesAMissingArchiveRoot)
{
    chl::ArchiveManifestWriter missing((root / "not-mounted").string(), "1");
    EXPECT_NE(missing.open(), chl::CL_SUCCESS);
    EXPECT_FALSE(fs::exists(root / "not-mounted"));

    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    fs::remove_all(root);
    EXPECT_NE(writer.append(publication("c", "s", "a.h5", 0, 10)), chl::CL_SUCCESS);
    EXPECT_FALSE(fs::exists(root));
}

// A log deleted and created again can get the freed inode number back. If it
// has grown past where the reader stopped, the inode and size checks pass and
// the reader would start in the middle of the new log.
TEST_F(ArchiveManifestTest, TailRereadsALogReplacedOnTheSameInode)
{
    std::string const log = (root / "1.log").string();
    appendRaw(log, chl::toManifestLine(publication("c", "s", "old.h5", 0, 10)) + "\n");
    chl::ArchiveManifestTail tail(log);
    std::vector<chl::ArchiveManifestRecord> records;
    ASSERT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 1u);

    // same inode, new content longer than what was read
    {
        std::ofstream out(log, std::ios::trunc | std::ios::binary);
        out << chl::toManifestLine(publication("c", "s", "new-first.h5", 20, 30)) << "\n"
            << chl::toManifestLine(publication("c", "s", "new-second.h5", 30, 40)) << "\n";
    }
    records.clear();
    ASSERT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].file, "new-first.h5");
    EXPECT_EQ(records[1].file, "new-second.h5");
}

TEST_F(ArchiveManifestTest, TailRereadsALogReplacedOnTheSameInodeAtTheSameSize)
{
    std::string const log = (root / "1.log").string();
    std::string const old_log = "{\"op\":\"open\",\"writer\":\"1\",\"time\":1791320000123456789}\n" +
                                chl::toManifestLine(publication("c", "s", "old.h5", 20, 30)) + "\n";
    std::string const new_log = "{\"op\":\"open\",\"writer\":\"1\",\"time\":1791320000123456790}\n" +
                                chl::toManifestLine(publication("c", "s", "new.h5", 20, 30)) + "\n";
    ASSERT_EQ(old_log.size(), new_log.size());
    appendRaw(log, old_log);
    chl::ArchiveManifestTail tail(log);
    std::vector<chl::ArchiveManifestRecord> records;
    ASSERT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].file, "old.h5");

    // Truncate in place to reproduce reuse of the inode without depending on
    // the file system's inode allocator. The reader sees no intermediate size.
    {
        std::ofstream out(log, std::ios::trunc | std::ios::binary);
        out << new_log;
    }
    records.clear();
    ASSERT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].file, "new.h5");

    records.clear();
    ASSERT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    EXPECT_TRUE(records.empty());
}

// A record no reader could take back is refused, so the window counts as failed
// rather than written and unreachable.
TEST_F(ArchiveManifestTest, ARecordWhoseNumbersDoNotFitIsRefused)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    EXPECT_NE(writer.append(publication("c", "s", "a.h5", 1ULL << 63, (1ULL << 63) + 10)), chl::CL_SUCCESS);
    ASSERT_EQ(writer.append(publication("c", "s", "b.h5", (1ULL << 63) - 10, (1ULL << 63) - 1)), chl::CL_SUCCESS);
    chl::ArchiveManifestTail tail(writer.logPath());
    std::vector<chl::ArchiveManifestRecord> records;
    ASSERT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].file, "b.h5");
}

// A log the writer starts begins with a header naming the writer and the time,
// so a log created again on the same inode number is told apart from the old
// one even when its first record repeats the old first record. The tail never
// returns the header.
TEST_F(ArchiveManifestTest, ALogStartedAgainWithTheSameFirstRecordIsReadFromItsStart)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    ASSERT_EQ(writer.append(storyDeletion("c", "s")), chl::CL_SUCCESS);
    chl::ArchiveManifestTail tail(writer.logPath());
    std::vector<chl::ArchiveManifestRecord> records;
    ASSERT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].op, chl::ArchiveManifestRecord::Op::DELETE);

    // emptied in place: the same inode, started again by the next append
    fs::resize_file(writer.logPath(), 0);
    ASSERT_EQ(writer.append(storyDeletion("c", "s")), chl::CL_SUCCESS);
    ASSERT_EQ(writer.append(publication("c", "s", "new.h5", 20, 30)), chl::CL_SUCCESS);
    records.clear();
    ASSERT_EQ(tail.readNew(records), chl::CL_SUCCESS);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].op, chl::ArchiveManifestRecord::Op::DELETE);
    EXPECT_EQ(records[1].file, "new.h5");
}

TEST(ArchiveManifestLine, ADeletionWithItsBoundRoundTrips)
{
    chl::ArchiveManifestRecord record = storyDeletion("c", "s");
    record.writer_start = 1791320000123456789ULL;
    record.incarnation_bound = 42;
    chl::ArchiveManifestRecord parsed;
    ASSERT_TRUE(chl::parseManifestLine(chl::toManifestLine(record), parsed));
    EXPECT_EQ(parsed.writer_start, record.writer_start);
    EXPECT_EQ(parsed.incarnation_bound, 42u);

    // without a bound: every earlier file of the log
    ASSERT_TRUE(chl::parseManifestLine(chl::toManifestLine(storyDeletion("c", "s")), parsed));
    EXPECT_EQ(parsed.incarnation_bound, UINT64_MAX);
}

TEST(ArchiveManifestLine, TheLogHeaderRoundTrips)
{
    chl::ArchiveManifestRecord header;
    header.op = chl::ArchiveManifestRecord::Op::OPEN;
    header.writer = "3";
    header.start = 1791320000123456789ULL;
    chl::ArchiveManifestRecord parsed;
    ASSERT_TRUE(chl::parseManifestLine(chl::toManifestLine(header), parsed));
    EXPECT_EQ(parsed.op, chl::ArchiveManifestRecord::Op::OPEN);
    EXPECT_EQ(parsed.writer, "3");
    EXPECT_EQ(parsed.start, header.start);
}

TEST_F(ArchiveManifestTest, AppendFailsWhenTheLogCannotBeWritten)
{
    chl::ArchiveManifestWriter writer(root.string(), "1");
    ASSERT_EQ(writer.open(), chl::CL_SUCCESS);
    fs::remove(writer.logPath());
    fs::create_directory(writer.logPath()); // a directory where the log should be
    EXPECT_NE(writer.append(publication("c", "s", "a.h5", 0, 10)), chl::CL_SUCCESS);
}
