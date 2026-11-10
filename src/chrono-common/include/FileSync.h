#ifndef CHRONOLOG_FILE_SYNC_H
#define CHRONOLOG_FILE_SYNC_H

#include <string>

namespace chronolog
{

// The fsync the archive writers call: the grapher syncs each window file, its
// directory and the manifest before it reports a window written, because that
// report lets keepers drop their copies of the window's events.

using FsyncFn = int (*)(int fd);

// Calls the installed function, ::fsync unless a test installed another.
// Returns what it returns: 0, or -1 with errno set.
int archiveFsync(int fd);

// Installs the function archiveFsync calls; nullptr restores ::fsync.
void setArchiveFsyncForTest(FsyncFn fn);

// Opens path read-only (a directory too), syncs it with archiveFsync and
// closes it. Returns 0, or the errno of the step that failed.
int fsyncPath(std::string const& path);

} // namespace chronolog

#endif // CHRONOLOG_FILE_SYNC_H
