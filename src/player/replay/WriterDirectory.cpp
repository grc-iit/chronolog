#include "player/replay/WriterDirectory.h"
#include <chrono>

namespace chronolog::player
{

WriterDirectory::WriterDirectory(std::shared_ptr<grpc::Channel> visor_internal)
    : stub_(internal::v1::Cluster::NewStub(std::move(visor_internal)))
{}

WriterDirectory::~WriterDirectory() { stop(); }

void WriterDirectory::watch(const std::string& keeper_id)
{
    std::lock_guard lk(mu_);
    if(stopped_ || !watched_.insert(keeper_id).second)
        return;
    threads_.emplace_back([this, keeper_id] { run(keeper_id); });
}

void WriterDirectory::stop()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard lk(mu_);
        stopped_ = true;
        for(auto& [id, context]: active_) context->TryCancel();
        threads.swap(threads_);
    }
    cv_.notify_all();
    for(auto& t: threads)
        if(t.joinable())
            t.join();
}

std::vector<WriterAssignment> WriterDirectory::writers(StoryId story) const
{
    std::vector<WriterAssignment> out;
    std::lock_guard lk(mu_);
    for(const auto& [keeper, entries]: by_keeper_)
        for(const auto& [key, assignment]: entries)
            if(std::get<0>(key) == story)
                out.push_back(assignment);
    return out;
}

void WriterDirectory::apply(const std::string& keeper_id, const internal::v1::AcquisitionUpdate& update)
{
    Key key{update.story_id(), update.writer_id(), update.incarnation()};
    auto& entries = by_keeper_[keeper_id];
    if(update.state() == internal::v1::ACQUISITION_STATE_ACQUIRED)
        entries[key] =
                WriterAssignment{update.writer_id(), update.incarnation(), update.assigned_keeper().process_id()};
    else
        entries.erase(key);
}

void WriterDirectory::run(std::string keeper_id)
{
    using namespace std::chrono_literals;
    for(;;)
    {
        grpc::ClientContext context;
        {
            std::lock_guard lk(mu_);
            if(stopped_)
                return;
            active_[keeper_id] = &context;
        }
        internal::v1::WatchAcquisitionsRequest request;
        request.set_keeper_id(keeper_id);
        auto reader = stub_->WatchAcquisitions(&context, request);
        internal::v1::WatchAcquisitionsResponse response;
        while(reader->Read(&response))
        {
            std::lock_guard lk(mu_);
            if(response.has_snapshot())
            {
                by_keeper_[keeper_id].clear();
                for(const auto& update: response.snapshot().acquisitions()) apply(keeper_id, update);
            }
            else if(response.has_update())
            {
                apply(keeper_id, response.update());
            }
        }
        reader->Finish();
        std::unique_lock lk(mu_);
        active_.erase(keeper_id);
        // Entries stay until a fresh snapshot replaces them: a stale view only affects naming.
        if(cv_.wait_for(lk, 500ms, [&] { return stopped_; }))
            return;
    }
}

} // namespace chronolog::player
