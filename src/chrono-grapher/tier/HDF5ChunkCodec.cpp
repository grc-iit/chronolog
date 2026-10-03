#include "chrono-grapher/tier/ChunkCodec.h"
#include "chrono-grapher/tier/FileIO.h"
#include <hdf5.h>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>

namespace chronolog
{
namespace
{
std::mutex hdf5_mutex;
constexpr std::size_t MaxBytes = 256 * 1024 * 1024;
constexpr std::size_t MaxEvents = 65536;
constexpr std::size_t MaxAttributes = 1024 * 1024;

struct ReadOnlyImage
{
    unsigned char* data;
    std::size_t size;
    static void* Allocate(std::size_t size, H5FD_file_image_op_t, void* context)
    {
        auto& image = *static_cast<ReadOnlyImage*>(context);
        return size == image.size ? image.data : nullptr;
    }
    static void* Copy(void* dest, const void* source, std::size_t size, H5FD_file_image_op_t, void* context)
    {
        auto& image = *static_cast<ReadOnlyImage*>(context);
        return dest == image.data && source == image.data && size <= image.size ? dest : nullptr;
    }
    static void* Resize(void*, std::size_t, H5FD_file_image_op_t, void*) { return nullptr; }
    static herr_t Free(void*, H5FD_file_image_op_t, void*) { return 0; }
    static void* CopyContext(void* context) { return context; }
    static herr_t FreeContext(void*) { return 0; }
};

void Check(herr_t result)
{
    if(result < 0)
        throw std::runtime_error("HDF5 operation failed");
}
class Handle
{
public:
    Handle(hid_t id, herr_t (*close)(hid_t))
        : id_(id)
        , close_(close)
    {
        if(id < 0)
            throw std::runtime_error("HDF5 object open failed");
    }
    ~Handle()
    {
        if(id_ >= 0)
            close_(id_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator hid_t() const { return id_; }
    void close()
    {
        auto id = id_;
        id_ = -1;
        Check(close_(id));
    }

private:
    hid_t id_;
    herr_t (*close_)(hid_t);
};
struct Row
{
    uint64_t story_id, writer_id, incarnation, sequence;
    int64_t hlc_physical_ns, physical_ns;
    uint64_t uncertainty_ns;
    uint32_t hlc_logical;
    uint8_t has_uncertainty, clock_status, durability;
    hvl_t content_type, payload, trace_id, span_id;
};
struct Attribute
{
    uint64_t event_index;
    char* key;
    char* value;
};

hid_t EventType()
{
    Handle type(H5Tcreate(H5T_COMPOUND, sizeof(Row)), H5Tclose);
    Handle bytes(H5Tvlen_create(H5T_NATIVE_UCHAR), H5Tclose);
#define FIELD(name, kind) Check(H5Tinsert(type, #name, HOFFSET(Row, name), kind))
    FIELD(story_id, H5T_NATIVE_UINT64);
    FIELD(writer_id, H5T_NATIVE_UINT64);
    FIELD(incarnation, H5T_NATIVE_UINT64);
    FIELD(sequence, H5T_NATIVE_UINT64);
    FIELD(hlc_physical_ns, H5T_NATIVE_INT64);
    FIELD(hlc_logical, H5T_NATIVE_UINT32);
    FIELD(physical_ns, H5T_NATIVE_INT64);
    FIELD(uncertainty_ns, H5T_NATIVE_UINT64);
    FIELD(has_uncertainty, H5T_NATIVE_UINT8);
    FIELD(clock_status, H5T_NATIVE_UINT8);
    FIELD(durability, H5T_NATIVE_UINT8);
    FIELD(content_type, bytes);
    FIELD(payload, bytes);
    FIELD(trace_id, bytes);
    FIELD(span_id, bytes);
#undef FIELD
    return H5Tcopy(type);
}
hid_t AttributeType()
{
    Handle type(H5Tcreate(H5T_COMPOUND, sizeof(Attribute)), H5Tclose);
    Handle string(H5Tcopy(H5T_C_S1), H5Tclose);
    Check(H5Tset_size(string, H5T_VARIABLE));
    Check(H5Tinsert(type, "event_index", HOFFSET(Attribute, event_index), H5T_NATIVE_UINT64));
    Check(H5Tinsert(type, "key", HOFFSET(Attribute, key), string));
    Check(H5Tinsert(type, "value", HOFFSET(Attribute, value), string));
    return H5Tcopy(type);
}

// Escaping preserves embedded NULs while keeping HDF5 variable-length strings.
std::string Escape(const std::string& value)
{
    std::string result;
    for(char c: value)
    {
        if(c == '\0')
            result += "\\0";
        else if(c == '\\')
            result += "\\\\";
        else
            result += c;
    }
    return result;
}
std::string Unescape(const char* value)
{
    std::string result;
    if(!value)
        return result;
    for(std::size_t i = 0; value[i]; ++i)
    {
        if(value[i] != '\\')
            result += value[i];
        else
        {
            ++i;
            if(value[i] == '0')
                result += '\0';
            else if(value[i] == '\\')
                result += '\\';
            else
                throw std::runtime_error("invalid attribute escape");
        }
    }
    return result;
}
hvl_t Bytes(const std::string& value) { return {value.size(), const_cast<char*>(value.data())}; }
std::string String(const hvl_t& value)
{
    if(!value.len)
        return {};
    if(!value.p || value.len > MaxBytes)
        throw std::runtime_error("invalid HDF5 byte field");
    return {static_cast<const char*>(value.p), value.len};
}
void Scalar(hid_t group, const char* name, hid_t type, const void* value)
{
    Handle space(H5Screate(H5S_SCALAR), H5Sclose);
    Handle attr(H5Acreate2(group, name, type, space, H5P_DEFAULT, H5P_DEFAULT), H5Aclose);
    Check(H5Awrite(attr, type, value));
}
void Dataset(hid_t group, const char* name, hid_t type, hsize_t count, const void* data)
{
    Handle space(H5Screate_simple(1, &count, nullptr), H5Sclose);
    Handle dataset(H5Dcreate2(group, name, type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
    if(count)
        Check(H5Dwrite(dataset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, data));
}
absl::Status Write(const std::filesystem::path& path, std::span<const Event> events, StoryId story, Hlc start, Hlc end)
{
    if(events.size() > MaxEvents)
        return absl::InvalidArgumentError("too many chunk events");
    std::size_t total = 0, attributes = 0;
    for(const auto& event: events)
    {
        total += event.envelope.content_type.size() + event.envelope.payload.size() + event.envelope.trace_id.size() +
                 event.envelope.span_id.size();
        attributes += event.envelope.attributes.size();
        for(const auto& [key, value]: event.envelope.attributes) total += 2 * (key.size() + value.size()) + 2;
        if(total > MaxBytes || attributes > MaxAttributes)
            return absl::InvalidArgumentError("chunk byte or attribute limit exceeded");
    }
    std::lock_guard lock(hdf5_mutex);
    try
    {
        // Shared archives sit on NFS or PFS, where flock fails or blocks readers, so HDF5 must not lock.
        Handle access(H5Pcreate(H5P_FILE_ACCESS), H5Pclose);
        Check(H5Pset_file_locking(access, false, true));
        Handle file(H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, access), H5Fclose);
        {
            Handle group(H5Gcreate2(file, "chunk", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
            Scalar(group, "story_id", H5T_NATIVE_UINT64, &story);
            Scalar(group, "start_time", H5T_NATIVE_INT64, &start.physical_ns);
            Scalar(group, "start_logical", H5T_NATIVE_UINT32, &start.logical);
            Scalar(group, "end_time", H5T_NATIVE_INT64, &end.physical_ns);
            Scalar(group, "end_logical", H5T_NATIVE_UINT32, &end.logical);
            const uint32_t encoding = 1;
            Scalar(group, "attribute_backslash_encoding", H5T_NATIVE_UINT32, &encoding);
            std::vector<Row> rows;
            std::vector<std::pair<std::string, std::string>> strings;
            std::vector<Attribute> attrs;
            strings.reserve(attributes);
            for(const auto& event: events)
            {
                const auto& envelope = event.envelope;
                for(const auto& [key, value]: envelope.attributes)
                {
                    auto& pair = strings.emplace_back(Escape(key), Escape(value));
                    attrs.push_back({rows.size(), pair.first.data(), pair.second.data()});
                }
                rows.push_back({event.id.story_id,
                                event.id.writer_id,
                                event.id.incarnation,
                                event.id.sequence,
                                event.hlc.physical_ns,
                                event.physical.physical_ns,
                                event.physical.uncertainty_ns.value_or(0),
                                event.hlc.logical,
                                static_cast<uint8_t>(event.physical.uncertainty_ns.has_value()),
                                static_cast<uint8_t>(event.physical.status),
                                static_cast<uint8_t>(event.durability),
                                Bytes(envelope.content_type),
                                Bytes(envelope.payload),
                                Bytes(envelope.trace_id),
                                Bytes(envelope.span_id)});
            }
            Handle type(EventType(), H5Tclose);
            Handle attr_type(AttributeType(), H5Tclose);
            Dataset(group, "events.vlen_bytes", type, rows.size(), rows.data());
            Dataset(group, "attributes", attr_type, attrs.size(), attrs.data());
        }
        file.close();
        return absl::OkStatus();
    }
    catch(const std::exception& error)
    {
        return absl::UnavailableError(error.what());
    }
}

struct Budget
{
    std::size_t remaining = MaxBytes;
    static void* Allocate(std::size_t size, void* context)
    {
        auto& budget = *static_cast<Budget*>(context);
        if(size > budget.remaining)
            return nullptr;
        budget.remaining -= size;
        return std::malloc(size);
    }
    static void Free(void* pointer, void*) { std::free(pointer); }
};
template <class T, class Function>
void ReadDataset(hid_t group, const char* name, hid_t type, std::size_t limit, Budget& budget, Function consume)
{
    Handle dataset(H5Dopen2(group, name, H5P_DEFAULT), H5Dclose);
    Handle space(H5Dget_space(dataset), H5Sclose);
    hsize_t count = 0;
    if(H5Sget_simple_extent_ndims(space) != 1 || H5Sget_simple_extent_dims(space, &count, nullptr) < 0 || count > limit)
        throw std::runtime_error("invalid HDF5 dataset extent");
    std::vector<T> rows(count);
    if(!count)
    {
        consume(rows);
        return;
    }
    Handle transfer(H5Pcreate(H5P_DATASET_XFER), H5Pclose);
    Check(H5Pset_vlen_mem_manager(transfer, Budget::Allocate, &budget, Budget::Free, nullptr));
    struct Reclaim
    {
        hid_t type, space, transfer;
        void* data;
        ~Reclaim() { H5Dvlen_reclaim(type, space, transfer, data); }
    } reclaim{type, space, transfer, rows.data()};
    Check(H5Dread(dataset, type, H5S_ALL, H5S_ALL, transfer, rows.data()));
    consume(rows);
}
} // namespace

absl::Status HDF5ChunkCodec::write(const std::filesystem::path& file, std::span<const Event> events) const
{
    Hlc start{}, end{};
    StoryId story = 0;
    if(!events.empty())
    {
        story = events.front().id.story_id;
        start = end = events.front().hlc;
        for(const auto& event: events)
        {
            start = std::min(start, event.hlc);
            end = std::max(end, event.hlc);
        }
        if(end.logical == std::numeric_limits<uint32_t>::max())
        {
            if(end.physical_ns == std::numeric_limits<int64_t>::max())
                return absl::InvalidArgumentError("HLC window overflow");
            ++end.physical_ns;
            end.logical = 0;
        }
        else
            ++end.logical;
    }
    return Write(file, events, story, start, end);
}
absl::Status HDF5ChunkCodec::writeChunk(const std::filesystem::path& file, const Chunk& chunk) const
{
    return Write(file, chunk.events, chunk.story_id, chunk.start, chunk.end);
}
absl::StatusOr<std::vector<Event>> HDF5ChunkCodec::decode(std::span<unsigned char> bytes) const
{
    if(bytes.empty() || bytes.size() > 2 * MaxBytes)
        return absl::UnavailableError("invalid HDF5 chunk file size");
    try
    {
        // The non-thread-safe HDF5 API only sees memory; disk reads can overlap.
        std::lock_guard lock(hdf5_mutex);
        ReadOnlyImage borrowed{bytes.data(), bytes.size()};
        Handle access(H5Pcreate(H5P_FILE_ACCESS), H5Pclose);
        Check(H5Pset_fapl_core(access, 64 * 1024, false));
        // Every HDF5 handle closes before the borrowed read-only image is released.
        H5FD_file_image_callbacks_t callbacks{ReadOnlyImage::Allocate,
                                              ReadOnlyImage::Copy,
                                              ReadOnlyImage::Resize,
                                              ReadOnlyImage::Free,
                                              ReadOnlyImage::CopyContext,
                                              ReadOnlyImage::FreeContext,
                                              &borrowed};
        Check(H5Pset_file_image_callbacks(access, &callbacks));
        Check(H5Pset_file_image(access, bytes.data(), bytes.size()));
        Handle input(H5Fopen("chronolog-archive-file-image", H5F_ACC_RDONLY, access), H5Fclose);
        Handle group(H5Gopen2(input, "chunk", H5P_DEFAULT), H5Gclose);
        Handle type(EventType(), H5Tclose);
        Handle attr_type(AttributeType(), H5Tclose);
        Budget budget;
        std::vector<Event> events;
        ReadDataset<Row>(group,
                         "events.vlen_bytes",
                         type,
                         MaxEvents,
                         budget,
                         [&](const auto& rows)
                         {
                             events.reserve(rows.size());
                             for(const auto& row: rows)
                             {
                                 if(row.has_uncertainty > 1 || row.clock_status > 2 || row.durability > 2)
                                     throw std::runtime_error("invalid HDF5 event enum");
                                 Event event;
                                 event.id = {row.story_id, row.writer_id, row.incarnation, row.sequence};
                                 event.hlc = {row.hlc_physical_ns, row.hlc_logical};
                                 event.physical.physical_ns = row.physical_ns;
                                 event.physical.status = static_cast<ClockStatus>(row.clock_status);
                                 if(row.has_uncertainty)
                                     event.physical.uncertainty_ns = row.uncertainty_ns;
                                 event.durability = static_cast<Durability>(row.durability);
                                 event.envelope.content_type = String(row.content_type);
                                 event.envelope.payload = String(row.payload);
                                 event.envelope.trace_id = String(row.trace_id);
                                 event.envelope.span_id = String(row.span_id);
                                 events.push_back(std::move(event));
                             }
                         });
        ReadDataset<Attribute>(group,
                               "attributes",
                               attr_type,
                               MaxAttributes,
                               budget,
                               [&](const auto& rows)
                               {
                                   for(const auto& row: rows)
                                   {
                                       if(row.event_index >= events.size() ||
                                          !events[row.event_index]
                                                   .envelope.attributes.emplace(Unescape(row.key), Unescape(row.value))
                                                   .second)
                                           throw std::runtime_error("invalid HDF5 attribute index or duplicate key");
                                   }
                               });
        return events;
    }
    catch(const std::exception& error)
    {
        return absl::UnavailableError(error.what());
    }
}
} // namespace chronolog
