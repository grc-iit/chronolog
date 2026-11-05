#ifndef CHRONOLOG_WAL_MOUNT_CHECK_H
#define CHRONOLOG_WAL_MOUNT_CHECK_H

#include <cstdint>
#include <string>
#include <vector>

namespace chronolog
{

// What the keeper learns at startup about the file system holding its WAL
// directory. The deploy script does the same check with findmnt and also
// compares physical disks; the keeper repeats it so a hand-written config is
// checked too.
struct WalMountInfo
{
    // short file system name from statfs f_type, such as "ext4" or "nfs";
    // "unknown(0x...)" for a type not in the table
    std::string fs_name;
    // NFS, Lustre, GPFS, BeeGFS, CIFS/SMB2, Ceph or FUSE
    bool shared_or_network = false;
    bool tmpfs = false;
    bool overlay = false;
    // the directory's st_dev equals that of "/": the keeper's approximation
    // of "on the boot disk"
    bool same_device_as_root = false;
    // space available to an unprivileged writer, and the file system's size
    uint64_t free_bytes = 0;
    uint64_t total_bytes = 0;
};

// Fills `info` from statfs/statvfs of `dir` and stat of `dir` and "/".
// Returns 0, or the errno of the first call that failed.
int inspectWalDirectory(std::string const& dir, WalMountInfo& info);

// One warning per problem: a shared or network file system (the NFS one
// names the export's sync option, the server's cache and the lock service
// flock needs), tmpfs, overlayfs, the root device, and free space below
// `reserve_bytes`. Empty for a local disk off the root device with room.
std::vector<std::string> walMountWarnings(WalMountInfo const& info, uint64_t reserve_bytes);

} // namespace chronolog

#endif
