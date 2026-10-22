#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>

#include <sys/uio.h>

#include <chrono_monitor.h>
#include <WalFormat.h>
#include <WalSegmentManager.h>

namespace chronolog
{

namespace
{

bool endsWith(std::string const& name, std::string const& suffix)
{
    return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool allZeroFrom(std::string const& bytes, std::size_t from)
{
    return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(std::min(from, bytes.size())),
                       bytes.end(),
                       [](char c) { return c == '\0'; });
}

uint64_t nowNs()
{
    return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
}

} // namespace

WalSegmentManager::WalSegmentManager(WalFileSink& sink,
                                     uint64_t segment_bytes,
                                     std::string keeper_endpoint,
                                     std::string directory)
    : sink(sink)
    , segmentBytes(segment_bytes)
    , keeperEndpoint(std::move(keeper_endpoint))
    , directory(std::move(directory))
{}

WalSegmentManager::~WalSegmentManager()
{
    stopPreparer();
    if(spare)
    {
        sink.closeFile(spare->fd);
    }
}

int WalSegmentManager::open()
{
    if(opened)
    {
        lastError = "WAL directory " + directory + " is already open";
        return EBUSY;
    }

    int err = sink.lockDirectory();
    if(err == EWOULDBLOCK)
    {
        lastError = "WAL directory " + directory + " is locked: another keeper is using it";
        LOG_ERROR("[WalSegmentManager] {}", lastError);
        return err;
    }
    if(err != 0)
    {
        lastError = "cannot lock WAL directory " + directory + ": " + std::strerror(err);
        LOG_ERROR("[WalSegmentManager] {}", lastError);
        return err;
    }

    std::vector<std::string> names;
    if((err = sink.list(names)) != 0)
    {
        lastError = "cannot list WAL directory " + directory + ": " + std::strerror(err);
        return err;
    }

    // a segment cut short by a crash while it was being prepared; never written to
    std::size_t removed = 0;
    for(std::string const& name: names)
    {
        if(!endsWith(name, ".partial"))
        {
            continue;
        }
        if((err = sink.remove(name)) != 0 && err != ENOENT)
        {
            lastError = "cannot remove " + name + " in WAL directory " + directory + ": " + std::strerror(err);
            return err;
        }
        ++removed;
    }
    if(removed > 0 && (err = sink.syncDirectory()) != 0)
    {
        lastError = "cannot sync WAL directory " + directory + ": " + std::strerror(err);
        return err;
    }

    segmentList.clear();
    for(std::string const& name: names)
    {
        if(std::optional<uint64_t> segment = wal::parseSegmentFileName(name))
        {
            segmentList.push_back(*segment);
        }
    }
    std::sort(segmentList.begin(), segmentList.end());

    {
        std::lock_guard<thallium::mutex> lock(mutex);
        nextSegment = segmentList.empty() ? 1 : segmentList.back() + 1;
    }
    opened = true;
    lastError.clear();
    LOG_INFO("[WalSegmentManager] Opened WAL directory {}: {} segments, {} partial files removed",
             directory,
             segmentList.size(),
             removed);
    return 0;
}

void WalSegmentManager::startPreparer()
{
    if(preparerRunning)
    {
        return;
    }
    {
        std::lock_guard<thallium::mutex> lock(mutex);
        stopping = false;
    }
    preparerStream = thallium::xstream::create();
    preparerThread = preparerStream->make_thread([this]() { preparerLoop(); });
    preparerRunning = true;
}

void WalSegmentManager::stopPreparer()
{
    if(!preparerRunning)
    {
        return;
    }
    {
        std::lock_guard<thallium::mutex> lock(mutex);
        stopping = true;
        changed.notify_all();
    }
    // a preparation under way finishes first
    preparerThread->join();
    preparerStream->join();
    preparerThread = thallium::managed<thallium::thread>();
    preparerStream = thallium::managed<thallium::xstream>();
    preparerRunning = false;
}

void WalSegmentManager::preparerLoop()
{
    std::unique_lock<thallium::mutex> lock(mutex);
    while(true)
    {
        changed.wait(lock, [this]() { return stopping || (!spare && !preparing && !preparerFailed); });
        if(stopping)
        {
            return;
        }
        preparing = true;
        uint64_t const segment = nextSegment++;
        lock.unlock();

        Spare made{};
        int err = prepare(segment, segmentBytes, made);
        if(err != 0)
        {
            LOG_ERROR("[WalSegmentManager] Cannot prepare WAL segment {} in {}: {}",
                      wal::segmentFileName(segment),
                      directory,
                      std::strerror(err));
        }

        lock.lock();
        preparing = false;
        if(err != 0)
        {
            preparerFailed = true; // the next takeSpare tries on its own thread and reports the error
        }
        else
        {
            spare = made;
        }
        changed.notify_all();
    }
}

int WalSegmentManager::prepare(uint64_t segment, uint64_t bytes, Spare& out)
{
    std::string const name = wal::segmentFileName(segment);
    std::string const partial = name + ".partial";
    std::string const header =
            wal::encodeHeader(wal::HeaderRecord{wal::kFormatVersion, segment, nowNs(), keeperEndpoint});
    bytes = std::max<uint64_t>(bytes, header.size());

    int fd = -1;
    bool renamed = false;
    int err = sink.createZeroed(partial, bytes);
    if(err == 0)
    {
        err = sink.openForWrite(partial, fd);
    }
    if(err == 0)
    {
        iovec iov{const_cast<char*>(header.data()), header.size()};
        err = sink.writeAt(fd, 0, &iov, 1);
    }
    if(err == 0)
    {
        err = sink.syncData(fd);
    }
    if(err == 0)
    {
        err = sink.rename(partial, name);
        renamed = (err == 0);
    }
    if(err == 0)
    {
        err = sink.syncDirectory();
    }
    if(err != 0)
    {
        if(fd >= 0)
        {
            sink.closeFile(fd);
        }
        sink.remove(renamed ? name : partial); // best effort; open() removes a leftover partial
        return err;
    }
    out = Spare{segment, fd, header.size(), bytes};
    return 0;
}

int WalSegmentManager::extend(Spare& target, uint64_t bytes)
{
    // zeros past the end: the file grows, and the data sync below commits the
    // new size before anything is written there
    static std::string const zeros(1u << 20, '\0');
    uint64_t offset = target.size_bytes;
    while(offset < bytes)
    {
        iovec iov{const_cast<char*>(zeros.data()),
                  static_cast<std::size_t>(std::min<uint64_t>(zeros.size(), bytes - offset))};
        if(int err = sink.writeAt(target.fd, offset, &iov, 1))
        {
            return err;
        }
        offset += iov.iov_len;
    }
    if(int err = sink.syncData(target.fd))
    {
        return err;
    }
    target.size_bytes = bytes;
    return 0;
}

int WalSegmentManager::takeSpare(uint64_t min_bytes,
                                 uint64_t& segment,
                                 int& fd,
                                 uint64_t& data_offset,
                                 uint64_t* size_bytes)
{
    if(!opened)
    {
        return EBADF;
    }

    std::unique_lock<thallium::mutex> lock(mutex);
    changed.wait(lock, [this]() { return spare.has_value() || !preparing; });
    if(!spare)
    {
        // no preparer, or its last attempt failed: prepare on this thread so
        // the caller gets the error
        preparing = true;
        uint64_t const number = nextSegment++;
        lock.unlock();
        Spare made{};
        int err = prepare(number, std::max(segmentBytes, min_bytes), made);
        lock.lock();
        preparing = false;
        if(err != 0)
        {
            preparerFailed = true;
            changed.notify_all();
            lastError = "cannot prepare WAL segment " + wal::segmentFileName(number) + " in " + directory + ": " +
                        std::strerror(err);
            return err;
        }
        spare = made;
    }

    if(spare->size_bytes < min_bytes)
    {
        // grown in place rather than replaced by a new number, so the spare's
        // number keeps its place in log order. The preparer leaves a ready
        // spare alone, so it can be grown unlocked.
        Spare grown = *spare;
        lock.unlock();
        int err = extend(grown, min_bytes);
        lock.lock();
        if(err != 0)
        {
            lastError = "cannot extend WAL segment " + wal::segmentFileName(grown.segment) + " in " + directory + ": " +
                        std::strerror(err);
            return err; // the spare stays ready at its old size
        }
        spare->size_bytes = grown.size_bytes;
    }

    Spare taken = *spare;
    spare.reset();
    preparerFailed = false;
    changed.notify_all();
    lock.unlock();

    segmentList.push_back(taken.segment);
    segmentHandedOut = true;
    segment = taken.segment;
    fd = taken.fd;
    data_offset = taken.data_offset;
    if(size_bytes != nullptr)
    {
        *size_bytes = taken.size_bytes;
    }
    return 0;
}

int WalSegmentManager::adoptExisting(uint64_t segment, int& fd, uint64_t& data_offset, uint64_t* size_bytes)
{
    if(!opened)
    {
        return EBADF;
    }
    std::string const name = wal::segmentFileName(segment);
    // writing into anything but the newest segment found would put new
    // records before older ones in log order
    if(segmentHandedOut || segmentList.empty() || segmentList.back() != segment)
    {
        lastError = "WAL segment " + name + " in " + directory + " is not the newest one found";
        return EINVAL;
    }

    std::string bytes;
    if(int err = sink.readFile(name, bytes))
    {
        lastError = "cannot read WAL segment " + name + " in " + directory + ": " + std::strerror(err);
        return err;
    }
    wal::ScanResult scan = wal::scanSegment(bytes);
    wal::HeaderRecord header{};
    if(scan.records.size() != 1 || scan.end != wal::ScanEnd::EndOfData ||
       scan.records[0].type != static_cast<uint8_t>(wal::RecordType::Header) ||
       wal::decodeHeader(scan.records[0].body, header) != 0 || header.segment_number != segment ||
       !allZeroFrom(bytes, scan.end_offset))
    {
        lastError = "WAL segment " + name + " in " + directory + " does not hold only its header";
        return EINVAL;
    }

    if(int err = sink.openForWrite(name, fd))
    {
        lastError = "cannot open WAL segment " + name + " in " + directory + ": " + std::strerror(err);
        return err;
    }
    segmentHandedOut = true;
    data_offset = scan.end_offset;
    if(size_bytes != nullptr)
    {
        *size_bytes = bytes.size();
    }
    return 0;
}

int WalSegmentManager::deleteSegment(uint64_t segment)
{
    auto listed = std::find(segmentList.begin(), segmentList.end(), segment);
    if(listed == segmentList.end())
    {
        return ENOENT;
    }
    int err = sink.remove(wal::segmentFileName(segment));
    if(err != 0 && err != ENOENT)
    {
        return err;
    }
    segmentList.erase(listed);
    return sink.syncDirectory();
}

} // namespace chronolog
