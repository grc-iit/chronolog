#ifndef CHRONO_KEEPER_EXTRACTION_ENGINE_H
#define CHRONO_KEEPER_EXTRACTION_ENGINE_H

#include <string>
#include <thallium.hpp>

namespace tl = thallium;

namespace chronolog
{

// The client engine the keeper's extraction streams send chunks to the grapher
// through. It gets a progress thread of its own: without one, margo runs the
// engine's progress loop in the primary pool, on the keeper's main thread, and
// that thread spends its time in sleep(10) between stats messages. A chunk's
// receive_story_chunk call then completed only while the main thread waited
// on a stats message, about three chunks per keeper every 10 s.
inline tl::engine* createExtractionEngine(std::string const& protocol)
{
    return new tl::engine(protocol, THALLIUM_CLIENT_MODE, true);
}

} // namespace chronolog

#endif
