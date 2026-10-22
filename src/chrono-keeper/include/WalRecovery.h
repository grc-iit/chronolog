#ifndef CHRONOLOG_WAL_RECOVERY_H
#define CHRONOLOG_WAL_RECOVERY_H

#include <string>

#include <WalFileSink.h>
#include <WalRecoveredState.h>
#include <WalSegmentManager.h>

namespace chronolog
{

// Reads a keeper WAL directory back at startup, before the keeper takes
// requests, and returns the durable events it still holds.
//
// It opens the manager (the directory lock, removal of *.partial, the segment
// list), then reads every segment oldest first:
//   - Each segment must start with an H record of this build's format version
//     that names the segment. Any other header refuses.
//   - Records are replayed in log order: N sets a story's names, E adds the
//     event and a copy in that segment, D erases the story's events and copies
//     so far (its names stay). An unknown record type, or an E of an event
//     codec version this build cannot read, refuses.
//   - Only the newest segment whose bytes after its H are not all zero can end
//     in a torn write. There the first bad frame or zero length ends the data,
//     and when anything but zeros follows, it is overwritten with zeros to the
//     end of the segment and data-synced: a torn group can leave a later page
//     written after a missing one. In any other segment a bad frame, or
//     anything but zeros after its end of data, refuses.
// Every check runs before anything is written, so a refused recovery changes
// no segment. Stories left with no events are not returned.
//
// The sink must have its directory open, and the manager must be built on that
// sink and not yet opened. Returns CL_SUCCESS, or CL_ERR_UNKNOWN with `error`
// naming the directory, the segment, the offset and the reason. `out` is
// written only on CL_SUCCESS.
int recoverWal(WalFileSink& sink, WalSegmentManager& manager, RecoveredState& out, std::string& error);

} // namespace chronolog

#endif // CHRONOLOG_WAL_RECOVERY_H
