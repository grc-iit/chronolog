#ifndef CHRONOLOG_HDF5_FILE_ACCESS_H
#define CHRONOLOG_HDF5_FILE_ACCESS_H

#include <H5Cpp.h>
#include <chrono_monitor.h>

namespace chronolog
{

// File access properties for every archive file ChronoLog opens, writing or
// reading.
//
// HDF5 1.10 and later take an advisory lock on each file it opens. ChronoLog
// deploys over a shared global file system -- NFS, Lustre, GPFS -- where the
// grapher writes archive files that players on other nodes read at the same
// time. On NFS mounted without a lock daemon or with `nolock`, and on parallel
// file systems without full byte-range locking, that lock does not protect
// anything: it turns into H5Fopen/H5Fcreate failing with "Resource temporarily
// unavailable". Those failures land where they hurt most now -- a failed write
// stops the story's watermark, and a failed read makes a replay incomplete.
//
// Turning it off is safe for this access pattern. One grapher owns a given
// window file and writes it once, through a temporary name (see
// StoryChunkWriter); a window that has to be written again goes to a numbered
// sibling rather than back into the same file. Readers only ever read. There is
// no second writer for the lock to keep out.
//
// ignore_when_disabled leaves a file system whose driver cannot honour the
// setting working normally instead of failing the open.
inline H5::FileAccPropList archiveFileAccess()
{
    H5::FileAccPropList fapl;
    fapl.copy(H5::FileAccPropList::DEFAULT);
    if(H5Pset_file_locking(fapl.getId(), /*use_file_locking=*/false, /*ignore_when_disabled=*/true) < 0)
    {
        LOG_WARNING("[HDF5FileAccess] Could not turn off HDF5 file locking; opens may fail on a shared file system "
                    "without lock support");
    }
    return fapl;
}

} // namespace chronolog

#endif // CHRONOLOG_HDF5_FILE_ACCESS_H
