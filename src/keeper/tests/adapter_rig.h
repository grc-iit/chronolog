#pragma once

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>

#include "adapter/ArchiveService.h"
#include "rpc/Channel.h"
#include "adapter/JournalService.h"
#include "chronolog/internal/v1/internal.grpc.pb.h"
#include "chronolog/v1/chronolog.grpc.pb.h"
#include "ram_harness.h"
#include "worker/WorkerPool.h"

namespace chronolog::test
{

// Journal and Archive services over one in-process server. Story 1 is at epoch 7 and
// writer 2 incarnation 3 is registered.
class AdapterRig
{
public:
    AdapterRig()
    {
        grpc::ServerBuilder builder;
        int port = 0;
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&journal_service_);
        builder.RegisterService(&archive_service_);
        server_ = builder.BuildAndStart();
        auto channel = rpc::peerChannel("127.0.0.1:" + std::to_string(port));
        journal = v1::Journal::NewStub(channel);
        archive = internal::v1::Archive::NewStub(channel);
    }

    ~AdapterRig()
    {
        if(server_)
            server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    }

    static std::unique_ptr<grpc::ClientContext> context()
    {
        auto ctx = std::make_unique<grpc::ClientContext>();
        ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        return ctx;
    }

    static v1::AppendItem item(uint64_t sequence, uint64_t writer = 2, uint64_t incarnation = 3)
    {
        v1::AppendItem i;
        i.set_writer_id(writer);
        i.set_incarnation(incarnation);
        i.set_sequence(sequence);
        i.mutable_physical()->set_physical_ns(100);
        i.mutable_physical()->set_status(v1::CLOCK_STATUS_UNSYNCED);
        i.mutable_envelope()->set_payload("event " + std::to_string(sequence));
        return i;
    }

    template <class Req>
    static void fillRequest(Req& request, uint64_t epoch, std::initializer_list<uint64_t> sequences)
    {
        request.set_story_id(1);
        request.set_epoch(epoch);
        request.set_durability(v1::DURABILITY_ACCEPTED);
        for(uint64_t s: sequences) *request.add_items() = item(s);
    }

    RamRig rig;
    std::unique_ptr<v1::Journal::Stub> journal;
    std::unique_ptr<internal::v1::Archive::Stub> archive;

private:
    WorkerPool pool_{2, 64};
    keeper::JournalService journal_service_{*rig.journal, pool_};
    keeper::ArchiveService archive_service_{*rig.journal, *rig.membership, pool_};
    std::unique_ptr<grpc::Server> server_;
};

} // namespace chronolog::test
