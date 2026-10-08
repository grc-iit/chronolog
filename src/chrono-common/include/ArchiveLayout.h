#ifndef CHRONOLOG_ARCHIVE_LAYOUT_H
#define CHRONOLOG_ARCHIVE_LAYOUT_H

#include <cstdint>
#include <filesystem>
#include <regex>
#include <string>

namespace chronolog
{
// Where the HDF5 archive keeps a story's files:
//
//   <archive>/<chronicle>/<story>/<start second>.<writer tag>.<incarnation>.<sequence>.vlen.h5
//
// The writer tag is "<recording group>.<grapher start time, ns>", the
// incarnation numbers the grapher's pipeline of the story (a story destroyed
// and created again gets a new one, see StoryPipeline), and the sequence counts
// the files that grapher process has published, so a file name is used once
// and never again. A destroy deletes a story's files but keeps the
// chronicle and story directories. So no path a player has looked up ever
// comes back after it was removed: on NFS, a client that looked a path up
// while it was missing can keep answering "not found" for it for up to
// acdirmax (lookupcache=all, the default), and a path that came back would be
// hidden that long.
//
// The directories keep a chronicle's name apart from its story's, so names are
// never parsed back out of a path and may hold anything. A name is written as
// typed, except what a directory name cannot be: '/' is written %2F, a NUL
// %00, and '%' %25 so that the encoding can be undone; a name that is exactly
// "." or ".." is written %2E or %2E%2E, and an empty one "%".
//
// Every '%' in an encoded name is followed by two hex digits or ends an empty
// name, so no chronicle directory can take a name such as the archive
// manifest's "%manifest".
inline std::string encodeArchiveName(std::string const& name)
{
    if(name.empty())
    {
        return "%";
    }
    if(name == ".")
    {
        return "%2E";
    }
    if(name == "..")
    {
        return "%2E%2E";
    }
    std::string encoded;
    encoded.reserve(name.size());
    for(char const c: name)
    {
        if(c == '%')
        {
            encoded += "%25";
        }
        else if(c == '/')
        {
            encoded += "%2F";
        }
        else if(c == '\0')
        {
            // the OS would cut the path here, and the name would land in
            // another story's directory
            encoded += "%00";
        }
        else
        {
            encoded += c;
        }
    }
    return encoded;
}

inline std::filesystem::path chronicleArchiveDirectory(std::filesystem::path const& archive_root,
                                                       std::string const& chronicle_name)
{
    return archive_root / encodeArchiveName(chronicle_name);
}

inline std::filesystem::path storyArchiveDirectory(std::filesystem::path const& archive_root,
                                                   std::string const& chronicle_name,
                                                   std::string const& story_name)
{
    return chronicleArchiveDirectory(archive_root, chronicle_name) / encodeArchiveName(story_name);
}

// The name of a file of the window starting at start_time (ns), published by
// the writer writer_tag for its pipeline incarnation as its sequence-th file.
inline std::string
windowFileName(uint64_t start_time, std::string const& writer_tag, uint64_t incarnation, uint64_t sequence)
{
    return std::to_string(start_time / 1000000000ULL) + "." + writer_tag + "." + std::to_string(incarnation) + "." +
           std::to_string(sequence) + ".vlen.h5";
}

// The fields of a window file's name (windowFileName), or of the partial file
// of one being written.
struct WindowFileName
{
    uint64_t start_second = 0;
    uint64_t recording_group = 0;
    uint64_t writer_start = 0; // the grapher process's start time, ns
    uint64_t incarnation = 0;
    uint64_t sequence = 0;
};

// false for a name that is not windowFileName's (including window files named
// before writer tags and incarnations)
inline bool parseWindowFileName(std::string const& file_name, WindowFileName& parsed)
{
    static std::regex const pattern(
            R"(^([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)\.vlen(\.[0-9]+)?\.h5(\.partial\..+)?$)");
    std::smatch match;
    if(!std::regex_match(file_name, match, pattern))
    {
        return false;
    }
    try
    {
        parsed.start_second = std::stoull(match[1].str());
        parsed.recording_group = std::stoull(match[2].str());
        parsed.writer_start = std::stoull(match[3].str());
        parsed.incarnation = std::stoull(match[4].str());
        parsed.sequence = std::stoull(match[5].str());
    }
    catch(std::exception const&)
    {
        return false;
    }
    return true;
}

// Whether a name in a story directory is one of the archive's own files: a
// window file (windowFileName, or <start second>.vlen[.<n>].h5 from before
// writer tags), or the partial file of one being written
// (<window file>.partial.<host>.<pid>.<n>, see StoryChunkWriter).
inline bool isWindowFileName(std::string const& file_name)
{
    static std::regex const pattern(R"(^[0-9]+(\.[0-9]+)*\.vlen(\.[0-9]+)?\.h5(\.partial\..+)?$)");
    return std::regex_match(file_name, pattern);
}

inline bool isPartialFileName(std::string const& file_name) { return file_name.find(".partial.") != std::string::npos; }
} // namespace chronolog

#endif // CHRONOLOG_ARCHIVE_LAYOUT_H
