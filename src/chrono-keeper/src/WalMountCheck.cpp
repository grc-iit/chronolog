#include <cerrno>
#include <cstdio>

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>

#include <WalMountCheck.h>

namespace chronolog
{

namespace
{

enum class MountKind
{
    Local,
    SharedOrNetwork,
    Tmpfs,
    Overlay
};

struct FileSystemType
{
    uint64_t magic;
    char const* name;
    MountKind kind;
};

// statfs f_type values, from linux/magic.h and the file systems' own headers
// (Lustre, GPFS, BeeGFS and ZFS are out of tree). NFSv3 and NFSv4 share one.
constexpr FileSystemType fileSystemTypes[] = {
        {0x6969, "nfs", MountKind::SharedOrNetwork},
        {0x0BD00BD0, "lustre", MountKind::SharedOrNetwork},
        {0x47504653, "gpfs", MountKind::SharedOrNetwork},
        {0x19830326, "beegfs", MountKind::SharedOrNetwork},
        {0xFF534D42, "cifs", MountKind::SharedOrNetwork},
        {0xFE534D42, "smb2", MountKind::SharedOrNetwork},
        {0x517B, "smb", MountKind::SharedOrNetwork},
        {0x00C36400, "ceph", MountKind::SharedOrNetwork},
        {0x65735546, "fuse", MountKind::SharedOrNetwork},
        {0x01021994, "tmpfs", MountKind::Tmpfs},
        {0x794C7630, "overlay", MountKind::Overlay},
        {0xEF53, "ext4", MountKind::Local},
        {0x58465342, "xfs", MountKind::Local},
        {0x9123683E, "btrfs", MountKind::Local},
        {0x2FC12FC1, "zfs", MountKind::Local},
        {0xF2F52010, "f2fs", MountKind::Local},
};

} // namespace

int inspectWalDirectory(std::string const& dir, WalMountInfo& info)
{
    struct statfs fsInfo
    {
    };
    if(::statfs(dir.c_str(), &fsInfo) != 0)
    {
        return errno;
    }
    // statvfs gives the sizes in f_frsize units, as std::filesystem::space does
    struct statvfs space
    {
    };
    if(::statvfs(dir.c_str(), &space) != 0)
    {
        return errno;
    }
    struct stat dirStat
    {
    };
    if(::stat(dir.c_str(), &dirStat) != 0)
    {
        return errno;
    }
    struct stat rootStat
    {
    };
    if(::stat("/", &rootStat) != 0)
    {
        return errno;
    }

    // f_type is signed on some ABIs; the magic numbers are 32 bits
    uint64_t magic = static_cast<uint64_t>(fsInfo.f_type) & 0xFFFFFFFFULL;
    info = WalMountInfo{};
    for(auto const& type: fileSystemTypes)
    {
        if(type.magic == magic)
        {
            info.fs_name = type.name;
            info.shared_or_network = type.kind == MountKind::SharedOrNetwork;
            info.tmpfs = type.kind == MountKind::Tmpfs;
            info.overlay = type.kind == MountKind::Overlay;
            break;
        }
    }
    if(info.fs_name.empty())
    {
        char name[32];
        std::snprintf(name, sizeof(name), "unknown(0x%llx)", static_cast<unsigned long long>(magic));
        info.fs_name = name;
    }
    info.same_device_as_root = dirStat.st_dev == rootStat.st_dev;
    info.free_bytes = static_cast<uint64_t>(space.f_bavail) * space.f_frsize;
    info.total_bytes = static_cast<uint64_t>(space.f_blocks) * space.f_frsize;
    return 0;
}

std::vector<std::string> walMountWarnings(WalMountInfo const& info, uint64_t reserve_bytes)
{
    std::vector<std::string> warnings;
    if(info.shared_or_network)
    {
        if(info.fs_name == "nfs")
        {
            warnings.push_back("WAL directory is on nfs, a shared or network file system: a durable write is only "
                               "as durable as the server's export (it must be exported sync) and the server's "
                               "storage cache, each fsync is a round trip to the server, and the WAL's flock "
                               "depends on the server's lock service");
        }
        else
        {
            warnings.push_back("WAL directory is on " + info.fs_name +
                               ", a shared or network file system: a durable write is only as durable as that "
                               "file system's own fsync, and the WAL's flock depends on its locking");
        }
    }
    if(info.tmpfs)
    {
        warnings.push_back("WAL directory is on tmpfs: durable writes survive a keeper crash but not a node reboot");
    }
    if(info.overlay)
    {
        warnings.push_back("WAL directory is on overlay: the WAL lives in a container's writable layer and is lost "
                           "with the container; mount a host directory for it");
    }
    if(info.same_device_as_root)
    {
        warnings.push_back("WAL directory is on the same device as /, likely the boot disk; prefer a local "
                           "non-OS drive");
    }
    if(info.free_bytes < reserve_bytes)
    {
        warnings.push_back("WAL file system has " + std::to_string(info.free_bytes) + " bytes free, below the " +
                           std::to_string(reserve_bytes) +
                           " byte reserve: durable writes are refused until space is freed");
    }
    return warnings;
}

} // namespace chronolog
