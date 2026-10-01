#include "membership/AcquisitionWatcher.h"

#include <set>

namespace chronolog::keeper
{

namespace iv1 = chronolog::internal::v1;

AcquisitionWatcher::AcquisitionWatcher(RamJournal& journal,
                                       std::string keeper_id,
                                       FenceHook on_fence,
                                       bool gate_admission)
    : journal_(journal)
    , keeper_id_(std::move(keeper_id))
    , on_fence_(std::move(on_fence))
    , gate_admission_(gate_admission)
{
    if(gate_admission_)
        journal_.setAdmissionReady(false);
}

AcquisitionWatcher::~AcquisitionWatcher() { watcher_.reset(); }

void AcquisitionWatcher::start(std::shared_ptr<grpc::Channel> channel)
{
    stub_ = iv1::Cluster::NewStub(std::move(channel));
    watcher_ = std::make_unique<Watcher>([this](std::stop_token stop) { return session(stop); });
}

bool AcquisitionWatcher::session(std::stop_token stop)
{
    if(gate_admission_)
        journal_.setAdmissionReady(false);
    grpc::ClientContext context;
    std::stop_callback cancel(stop, [&context] { context.TryCancel(); });
    iv1::WatchAcquisitionsRequest request;
    request.set_keeper_id(keeper_id_);
    auto reader = stub_->WatchAcquisitions(&context, request);
    iv1::WatchAcquisitionsResponse message;
    bool progressed = false;
    while(reader->Read(&message))
    {
        progressed = true;
        if(message.has_snapshot())
            applySnapshot(message.snapshot());
        else if(message.has_update())
            applyUpdate(message.update());
    }
    reader->Finish();
    return progressed;
}

void AcquisitionWatcher::advance(uint64_t revision)
{
    {
        std::lock_guard lock(mutex_);
        applied_ = std::max(applied_, revision);
    }
    cv_.notify_all();
}

void AcquisitionWatcher::applySnapshot(const iv1::AcquisitionSnapshot& snapshot)
{
    if(snapshot.revision() < appliedRevision())
        return;
    if(gate_admission_)
        journal_.setAdmissionReady(false);
    std::set<RamJournal::WriterKey> listed;
    for(const auto& update: snapshot.acquisitions())
        if(update.state() == iv1::ACQUISITION_STATE_ACQUIRED)
            listed.insert({update.story_id(), update.writer_id(), update.incarnation()});

    bool fenced = false;
    for(const auto& key: journal_.liveWriters())
    {
        if(!listed.contains(key))
        {
            journal_.releaseWriter(key.story_id, key.writer_id, key.incarnation);
            fenced = true;
        }
    }
    for(const auto& update: snapshot.acquisitions())
    {
        // A snapshot lists only live acquisitions; a RELEASED entry is applied as a fence.
        if(update.state() == iv1::ACQUISITION_STATE_RELEASED)
            fenced = true;
        applyWriter(update);
    }
    if(gate_admission_)
        journal_.setAdmissionReady(true);
    advance(snapshot.revision());
    if(fenced && on_fence_)
        on_fence_();
}

void AcquisitionWatcher::applyWriter(const iv1::AcquisitionUpdate& update)
{
    switch(update.state())
    {
        case iv1::ACQUISITION_STATE_ACQUIRED:
            if(update.assigned_keeper().process_id() == keeper_id_)
                (void)journal_.registerWriter(update.story_id(), update.writer_id(), update.incarnation());
            else
                journal_.unassignWriter(update.story_id(), update.writer_id());
            break;
        case iv1::ACQUISITION_STATE_RELEASED:
            journal_.releaseWriter(update.story_id(), update.writer_id(), update.incarnation());
            break;
        default:
            break;
    }
}

void AcquisitionWatcher::applyUpdate(const iv1::AcquisitionUpdate& update)
{
    if(update.revision() <= appliedRevision())
        return;
    applyWriter(update);
    advance(update.revision());
    if(update.state() == iv1::ACQUISITION_STATE_RELEASED && on_fence_)
        on_fence_();
}

uint64_t AcquisitionWatcher::appliedRevision() const
{
    std::lock_guard lock(mutex_);
    return applied_;
}

bool AcquisitionWatcher::waitApplied(uint64_t revision, std::chrono::milliseconds timeout) const
{
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return applied_ >= revision; });
}

} // namespace chronolog::keeper
