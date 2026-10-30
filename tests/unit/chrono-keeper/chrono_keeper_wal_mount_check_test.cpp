// Unit tests for the keeper's WAL directory mount check: the warnings for
// each kind of file system, the boot disk and the reserve, on fabricated
// WalMountInfo values, and one inspection of a real temporary directory.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <WalMountCheck.h>

namespace chl = chronolog;
namespace fs = std::filesystem;

namespace
{

constexpr uint64_t GiB = 1024ULL * 1024 * 1024;

// A local ext4 directory off the boot disk with room to spare: no warnings.
chl::WalMountInfo localDisk()
{
    chl::WalMountInfo info;
    info.fs_name = "ext4";
    info.shared_or_network = false;
    info.tmpfs = false;
    info.overlay = false;
    info.same_device_as_root = false;
    info.free_bytes = 100 * GiB;
    info.total_bytes = 200 * GiB;
    return info;
}

bool anyContains(std::vector<std::string> const& warnings, std::string const& text)
{
    for(auto const& warning: warnings)
    {
        if(warning.find(text) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(WalMountCheck, LocalDiskWithRoomHasNoWarnings) { EXPECT_TRUE(chl::walMountWarnings(localDisk(), GiB).empty()); }

TEST(WalMountCheck, NfsWarningNamesExportSyncCacheAndLockService)
{
    auto info = localDisk();
    info.fs_name = "nfs";
    info.shared_or_network = true;
    auto warnings = chl::walMountWarnings(info, GiB);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_TRUE(anyContains(warnings, "nfs"));
    EXPECT_TRUE(anyContains(warnings, "sync"));
    EXPECT_TRUE(anyContains(warnings, "cache"));
    EXPECT_TRUE(anyContains(warnings, "flock"));
    EXPECT_TRUE(anyContains(warnings, "lock service"));
}

TEST(WalMountCheck, OtherSharedFileSystemsWarnWithTheirName)
{
    for(std::string name: {"lustre", "gpfs", "beegfs", "cifs", "smb2", "ceph", "fuse"})
    {
        auto info = localDisk();
        info.fs_name = name;
        info.shared_or_network = true;
        auto warnings = chl::walMountWarnings(info, GiB);
        ASSERT_EQ(warnings.size(), 1u) << name;
        EXPECT_TRUE(anyContains(warnings, name)) << name;
        EXPECT_TRUE(anyContains(warnings, "shared or network")) << name;
    }
}

TEST(WalMountCheck, TmpfsWarnsThatOnlyAKeeperCrashIsSurvived)
{
    auto info = localDisk();
    info.fs_name = "tmpfs";
    info.tmpfs = true;
    auto warnings = chl::walMountWarnings(info, GiB);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_TRUE(anyContains(warnings, "tmpfs"));
    EXPECT_TRUE(anyContains(warnings, "reboot"));
}

TEST(WalMountCheck, OverlayWarns)
{
    auto info = localDisk();
    info.fs_name = "overlay";
    info.overlay = true;
    auto warnings = chl::walMountWarnings(info, GiB);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_TRUE(anyContains(warnings, "overlay"));
}

TEST(WalMountCheck, RootDeviceWarns)
{
    auto info = localDisk();
    info.same_device_as_root = true;
    auto warnings = chl::walMountWarnings(info, GiB);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_TRUE(anyContains(warnings, "boot disk"));
}

TEST(WalMountCheck, FreeSpaceBelowReserveWarnsWithBothNumbers)
{
    auto info = localDisk();
    info.free_bytes = 100;
    auto warnings = chl::walMountWarnings(info, 4096);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_TRUE(anyContains(warnings, "reserve"));
    EXPECT_TRUE(anyContains(warnings, "100"));
    EXPECT_TRUE(anyContains(warnings, "4096"));
}

TEST(WalMountCheck, FreeSpaceAtTheReserveDoesNotWarn)
{
    auto info = localDisk();
    info.free_bytes = 4096;
    EXPECT_TRUE(chl::walMountWarnings(info, 4096).empty());
}

TEST(WalMountCheck, EachProblemGetsItsOwnWarning)
{
    auto info = localDisk();
    info.fs_name = "tmpfs";
    info.tmpfs = true;
    info.same_device_as_root = true;
    info.free_bytes = 0;
    EXPECT_EQ(chl::walMountWarnings(info, GiB).size(), 3u);
}

TEST(WalMountCheck, InspectsATemporaryDirectory)
{
    std::string pattern = (fs::temp_directory_path() / "wal_mount_check_XXXXXX").string();
    ASSERT_NE(mkdtemp(pattern.data()), nullptr);

    chl::WalMountInfo info;
    EXPECT_EQ(chl::inspectWalDirectory(pattern, info), 0);
    EXPECT_FALSE(info.fs_name.empty());
    EXPECT_GT(info.total_bytes, 0u);
    EXPECT_LE(info.free_bytes, info.total_bytes);
    // a file system is at most one of these
    EXPECT_LE(int(info.shared_or_network) + int(info.tmpfs) + int(info.overlay), 1);
    EXPECT_EQ(info.total_bytes, fs::space(pattern).capacity);

    std::error_code ec;
    fs::remove_all(pattern, ec);
}

TEST(WalMountCheck, InspectingAMissingDirectoryReturnsTheError)
{
    chl::WalMountInfo info;
    EXPECT_EQ(chl::inspectWalDirectory("/nonexistent/wal/mount/check", info), ENOENT);
}
