#pragma once
#include "chronolog/context/context.h"
#include <deque>
#include <mutex>
#include <tuple>

namespace chronolog::context
{
namespace detail
{
using Key = std::pair<std::string, StoryId>;
using OperationKey = std::pair<Key, std::string>;
struct Operation
{
    std::string digest;
    std::string conversation;
    PriorOutcome result;
    std::optional<client::AppendSpec> pending;
    size_t bytes{};
    bool unresolved{true};
};
struct Record
{
    ContextRef context;
    AgentIdentity identity;
    Key key;
    std::string session_id;
    Access access{Access::ReadWrite};
    SessionState state{SessionState::Ready};
    std::optional<client::Writer> writer;
    std::optional<WriterStamp> stamp;
    std::optional<std::string> blocking;
    std::optional<Position> processed;
    bool takeover_required{false};
    bool recovery_required{false};
    bool release_committed{false};
    bool release_fenced{false};
    bool counted{false};
    std::timed_mutex mutex;
    std::weak_ptr<ContextSession> handle;
    std::mutex io_mutex;
    bool closing{false};
    uint64_t next_io{};
    std::map<uint64_t, std::function<void()>> cancellations;
};
absl::StatusOr<std::string> encodeIdentity(const AgentIdentity&);
size_t rawBytes(const Envelope&);
bool validPosition(const Position&, StoryId, bool sentinel = false);
Deadline deadline(const ContextOptions&, Deadline);
absl::StatusOr<Hlc> successor(Hlc);
Hlc realtime();
client::HlcRange probeRange(Hlc end, std::chrono::nanoseconds width);
class IoRegistration
{
public:
    IoRegistration(std::shared_ptr<Record> record, std::function<void()> cancel)
        : record_(std::move(record))
    {
        if(!record_)
            return;
        std::lock_guard lock(record_->io_mutex);
        id_ = ++record_->next_io;
        record_->cancellations[id_] = std::move(cancel);
        if(record_->closing)
            record_->cancellations[id_]();
    }
    ~IoRegistration()
    {
        if(!record_)
            return;
        std::lock_guard lock(record_->io_mutex);
        record_->cancellations.erase(id_);
    }

private:
    std::shared_ptr<Record> record_;
    uint64_t id_{};
};
Page readPage(client::Client&, StoryId, client::HlcRange, PageLimits, Deadline, std::shared_ptr<Record> = {});
} // namespace detail
struct ContextClient::Impl
{
    Impl(client::Client value, ContextOptions config)
        : sdk(std::move(value))
        , options(std::move(config))
    {}
    client::Client sdk;
    ContextOptions options;
    std::string session_id;
    std::timed_mutex opens;
    std::mutex mutex;
    size_t writable{};
    size_t pending_bytes{};
    size_t unresolved{};
    std::map<std::pair<detail::Key, Access>, std::shared_ptr<detail::Record>> records;
    std::map<detail::OperationKey, std::shared_ptr<detail::Operation>> operations;
    std::deque<detail::OperationKey> completed;
};
struct ContextSession::Impl
{
    std::shared_ptr<ContextClient::Impl> core;
    std::shared_ptr<detail::Record> record;
};
} // namespace chronolog::context
