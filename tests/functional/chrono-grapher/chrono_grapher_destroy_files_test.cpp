// HDF5FileChunkExtractor::delete_story_files / delete_chronicle_files (added for
// issue #574: destructive APIs).
//
// A story's files are in <archive>/<chronicle>/<story>/ (see ArchiveLayout.h):
// destroying a story deletes the archive files in its directory, and destroying
// a chronicle those of all its stories; the directories stay.
//
// The deletion code does not read the files, so empty ones stand in for HDF5.

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include <ArchiveLayout.h>
#include <HDF5FileChunkExtractor.h>

namespace fs = std::filesystem;

namespace
{
fs::path make_temp_dir()
{
    fs::path dir = fs::temp_directory_path() / ("chronolog_destroy_files_test_" + std::to_string(::getpid()) + "_" +
                                                ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void touch(fs::path const& path)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path).put('\0');
}

// a window file of the story, where the grapher writes it
void touchWindow(fs::path const& dir, std::string const& chronicle, std::string const& story, std::string const& name)
{
    touch(chronolog::storyArchiveDirectory(dir, chronicle, story) / name);
}

// every file left under dir, by its path relative to dir
std::set<std::string> dir_contents(fs::path const& dir)
{
    std::set<std::string> out;
    for(auto const& entry: fs::recursive_directory_iterator(dir))
    {
        if(entry.is_regular_file())
        {
            out.insert(entry.path().lexically_relative(dir).string());
        }
    }
    return out;
}
} // namespace

TEST(HDF5FileChunkExtractor, DeleteStoryFilesRemovesOnlyThatStory)
{
    fs::path dir = make_temp_dir();
    touchWindow(dir, "myChronicle", "myStory", "1700000000.vlen.h5");
    touchWindow(dir, "myChronicle", "myStory", "1700000060.vlen.h5");
    touchWindow(dir, "myChronicle", "myStory", "1700000000.vlen.1.h5");
    touchWindow(dir, "myChronicle", "otherStory", "1700000000.vlen.h5");
    touchWindow(dir, "otherChronicle", "myStory", "1700000000.vlen.h5");
    touch(dir / "unrelated.txt");

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    size_t deleted = 0;
    ASSERT_EQ(0, ext.delete_story_files("myChronicle", "myStory", &deleted));
    EXPECT_EQ(3u, deleted);
    EXPECT_EQ(dir_contents(dir),
              (std::set<std::string>{"myChronicle/otherStory/1700000000.vlen.h5",
                                     "otherChronicle/myStory/1700000000.vlen.h5",
                                     "unrelated.txt"}));
    // the directory stays: a path that came back after it was removed could be
    // hidden by an NFS client's cached "not found" (see ArchiveLayout.h)
    EXPECT_TRUE(fs::is_empty(dir / "myChronicle" / "myStory"));

    fs::remove_all(dir);
}

TEST(HDF5FileChunkExtractor, DeleteChronicleFilesRemovesAllStoriesInChronicle)
{
    fs::path dir = make_temp_dir();
    touchWindow(dir, "myChronicle", "storyA", "1700000000.vlen.h5");
    touchWindow(dir, "myChronicle", "storyA", "1700000060.vlen.h5");
    touchWindow(dir, "myChronicle", "story.B", "1700000000.vlen.h5");
    touchWindow(dir, "myChronicle", "story.B", "1700000000.vlen.1.h5");
    touchWindow(dir, "otherChronicle", "storyA", "1700000000.vlen.h5");
    touchWindow(dir, "myChronicle.x", "storyA", "1700000000.vlen.h5");

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    size_t deleted = 0;
    ASSERT_EQ(0, ext.delete_chronicle_files("myChronicle", &deleted));
    EXPECT_EQ(4u, deleted);
    EXPECT_EQ(dir_contents(dir),
              (std::set<std::string>{"myChronicle.x/storyA/1700000000.vlen.h5",
                                     "otherChronicle/storyA/1700000000.vlen.h5"}));
    EXPECT_TRUE(fs::is_empty(dir / "myChronicle" / "storyA"));
    EXPECT_TRUE(fs::is_empty(dir / "myChronicle" / "story.B"));

    fs::remove_all(dir);
}

// ("a.b", "c") and ("a", "b.c") shared files when names were joined with dots.
TEST(HDF5FileChunkExtractor, StoriesWhoseNamesJoinAlikeAreDeletedSeparately)
{
    fs::path dir = make_temp_dir();
    touchWindow(dir, "a.b", "c", "1700000000.vlen.h5");
    touchWindow(dir, "a", "b.c", "1700000000.vlen.h5");

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    ASSERT_EQ(0, ext.delete_story_files("a.b", "c"));
    EXPECT_EQ(dir_contents(dir), (std::set<std::string>{"a/b.c/1700000000.vlen.h5"}));

    fs::remove_all(dir);
}

TEST(HDF5FileChunkExtractor, NamesWithSlashesAndDotsAreDeleted)
{
    fs::path dir = make_temp_dir();
    touchWindow(dir, "x/y", "..", "1700000000.vlen.h5");
    touchWindow(dir, "x", "y", "1700000000.vlen.h5");

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    ASSERT_EQ(0, ext.delete_story_files("x/y", ".."));
    EXPECT_EQ(dir_contents(dir), (std::set<std::string>{"x/y/1700000000.vlen.h5"}));
    ASSERT_EQ(0, ext.delete_chronicle_files("x/y"));
    EXPECT_EQ(dir_contents(dir), (std::set<std::string>{"x/y/1700000000.vlen.h5"}));
    EXPECT_TRUE(fs::is_directory(dir / "x%2Fy"));

    fs::remove_all(dir);
}

TEST(HDF5FileChunkExtractor, DeleteStoryFilesIsNoOpOnUnknownStory)
{
    fs::path dir = make_temp_dir();
    touchWindow(dir, "myChronicle", "storyA", "1700000000.vlen.h5");

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    size_t deleted = 99;
    ASSERT_EQ(0, ext.delete_story_files("myChronicle", "neverRecorded", &deleted));
    EXPECT_EQ(0u, deleted);
    EXPECT_EQ(dir_contents(dir).size(), 1u);

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
    // An archive directory that cannot be opened (mode 0: EACCES) must not
    // read as "nothing to delete": the story's files would stay on disk. We
    // can't reproduce EACCES as root (CI), so the test is skipped there.
    if(::geteuid() == 0)
    {
        GTEST_SKIP() << "skipping unreadable-dir test as root: chmod 0 doesn't gate root";
    }

    fs::path dir = make_temp_dir();
    touchWindow(dir, "myChronicle", "myStory", "1700000000.vlen.h5");
    fs::permissions(dir, fs::perms::none);

    chronolog::HDF5FileChunkExtractor ext(dir.string());
    size_t deleted = 99;
    int const story_rc = ext.delete_story_files("myChronicle", "myStory", &deleted);
    int const chronicle_rc = ext.delete_chronicle_files("myChronicle");

    // Restore permissions before any assertion so the temp dir is cleanable.
    fs::permissions(dir, fs::perms::owner_all);
    EXPECT_NE(story_rc, 0);
    EXPECT_NE(chronicle_rc, 0);

    fs::remove_all(dir);
}
