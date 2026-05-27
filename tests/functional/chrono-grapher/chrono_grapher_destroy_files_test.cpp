// Unit test for HDF5FileChunkExtractor::delete_story_files /
// delete_chronicle_files (added for issue #574: destructive APIs).
//
// We don't care that the on-disk files are real HDF5 — the deletion code only
// filters by filename and calls std::filesystem::remove — so we drop empty
// files matching ChronoGrapher's per-chunk naming convention
//   <chronicle>.<story>.<startSec>.vlen.h5
// (and the rotated <...>.<n>.vlen.h5 form) into a temp directory, run the
// deletion helpers, and check that exactly the expected files survive.

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include <HDF5FileChunkExtractor.h>

namespace fs = std::filesystem;

namespace
{
fs::path make_temp_dir()
{
    fs::path dir = fs::temp_directory_path() / ("chronolog_destroy_files_test_" + std::to_string(::getpid()) + "_" +
                                                std::to_string(reinterpret_cast<uintptr_t>(&dir)));
    fs::create_directories(dir);
    return dir;
}

void touch(fs::path const& path) { std::ofstream(path).put('\0'); }

std::set<std::string> dir_contents(fs::path const& dir)
{
    std::set<std::string> out;
    for(auto const& entry: fs::directory_iterator(dir)) { out.insert(entry.path().filename().string()); }
    return out;
}
} // namespace

TEST(HDF5FileChunkExtractor, DeleteStoryFilesRemovesOnlyMatching)
{
    fs::path dir = make_temp_dir();

    // Target story files.
    touch(dir / "myChronicle.myStory.1700000000.vlen.h5");
    touch(dir / "myChronicle.myStory.1700000060.vlen.h5");
    // Rotated variant produced by StoryChunkWriter when the base name collides.
    touch(dir / "myChronicle.myStory.1700000000.1.vlen.h5");

    // Other story in same chronicle — must survive a per-story delete.
    touch(dir / "myChronicle.otherStory.1700000000.vlen.h5");

    // Another chronicle entirely — must survive a per-story delete.
    touch(dir / "otherChronicle.myStory.1700000000.vlen.h5");

    // Non-HDF5 cruft — must survive everything.
    touch(dir / "unrelated.txt");

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    size_t deleted = 0;
    ASSERT_EQ(0, ext.delete_story_files("myChronicle", "myStory", &deleted));
    EXPECT_EQ(3u, deleted);

    auto remaining = dir_contents(dir);
    EXPECT_EQ(remaining.count("myChronicle.otherStory.1700000000.vlen.h5"), 1u);
    EXPECT_EQ(remaining.count("otherChronicle.myStory.1700000000.vlen.h5"), 1u);
    EXPECT_EQ(remaining.count("unrelated.txt"), 1u);
    EXPECT_EQ(remaining.size(), 3u);

    fs::remove_all(dir);
}

TEST(HDF5FileChunkExtractor, DeleteChronicleFilesRemovesAllStoriesInChronicle)
{
    fs::path dir = make_temp_dir();

    touch(dir / "myChronicle.storyA.1700000000.vlen.h5");
    touch(dir / "myChronicle.storyA.1700000060.vlen.h5");
    touch(dir / "myChronicle.storyB.1700000000.vlen.h5");

    // Other chronicle's files must survive.
    touch(dir / "otherChronicle.storyA.1700000000.vlen.h5");
    touch(dir / "anotherChronicle.someStory.1700000000.vlen.h5");

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    size_t deleted = 0;
    ASSERT_EQ(0, ext.delete_chronicle_files("myChronicle", &deleted));
    EXPECT_EQ(3u, deleted);

    auto remaining = dir_contents(dir);
    EXPECT_EQ(remaining.count("otherChronicle.storyA.1700000000.vlen.h5"), 1u);
    EXPECT_EQ(remaining.count("anotherChronicle.someStory.1700000000.vlen.h5"), 1u);
    EXPECT_EQ(remaining.size(), 2u);

    fs::remove_all(dir);
}

TEST(HDF5FileChunkExtractor, DeleteStoryFilesIsNoOpOnUnknownStory)
{
    fs::path dir = make_temp_dir();
    touch(dir / "myChronicle.storyA.1700000000.vlen.h5");

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    size_t deleted = 99;
    ASSERT_EQ(0, ext.delete_story_files("myChronicle", "neverRecorded", &deleted));
    EXPECT_EQ(0u, deleted);
    EXPECT_EQ(dir_contents(dir).size(), 1u);

    fs::remove_all(dir);
}

TEST(HDF5FileChunkExtractor, DeleteStoryFilesEscapesSpecialChars)
{
    // Chronicle/story names containing a dot must be matched literally, not as
    // regex metacharacters. Without escaping, "any.story" would also match
    // "anyXstory" and over-delete.
    fs::path dir = make_temp_dir();
    touch(dir / "ch.icle.st.ory.1700000000.vlen.h5");
    touch(dir / "chXicle.stXory.1700000000.vlen.h5");

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    size_t deleted = 0;
    ASSERT_EQ(0, ext.delete_story_files("ch.icle", "st.ory", &deleted));
    EXPECT_EQ(1u, deleted);
    auto remaining = dir_contents(dir);
    EXPECT_EQ(remaining.count("chXicle.stXory.1700000000.vlen.h5"), 1u);
    EXPECT_EQ(remaining.size(), 1u);

    fs::remove_all(dir);
}

TEST(HDF5FileChunkExtractor, DeleteOnNonExistentArchiveDirIsNoOp)
{
    chronolog::HDF5FileChunkExtractor ext("/tmp/definitely_does_not_exist_chronolog_test");
    size_t deleted = 99;
    ASSERT_EQ(0, ext.delete_story_files("ch", "st", &deleted));
    EXPECT_EQ(0u, deleted);
}

TEST(HDF5FileChunkExtractor, DeleteOnUnreadableArchiveDirReportsError)
{
    // If the archive directory exists but can't be opened (e.g. mode 0 so
    // EACCES on directory_iterator construction), delete_*_files must report
    // CL_ERR_UNKNOWN rather than silently returning CL_SUCCESS with zero
    // deletions. We can't reproduce EACCES when running as root (CI), so we
    // skip the test in that case.
    if(::geteuid() == 0)
    {
        GTEST_SKIP() << "skipping unreadable-dir test as root: chmod 0 doesn't gate root";
    }

    fs::path dir = make_temp_dir();
    touch(dir / "myChronicle.myStory.1700000000.vlen.h5");
    fs::permissions(dir, fs::perms::none);

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    size_t deleted = 99;
    int rc = ext.delete_story_files("myChronicle", "myStory", &deleted);

    // Restore permissions before any assertion so the temp dir is cleanable.
    fs::permissions(dir, fs::perms::owner_all);
    EXPECT_NE(rc, 0);

    fs::remove_all(dir);
}
