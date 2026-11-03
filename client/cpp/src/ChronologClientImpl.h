#ifndef CHRONOLOG_CLIENT_IMPL_H
#define CHRONOLOG_CLIENT_IMPL_H
#include "ClientRpcContext.h"
#include <chronolog_client.h>
#include <memory>
#include <mutex>
namespace chronolog
{
enum ClientMode
{
    WRITER_MODE = 0,
    READER_MODE = 1
};
class AcquireStoryResponseMsg;
class ChronologClientImpl
{
public:
    using Clock = rpc::RpcDeadline::Clock;
    static std::shared_ptr<ChronologClientImpl> GetClientImplInstance(ClientPortalServiceConf const&,
                                                                      ClientMode const&,
                                                                      ClientQueryServiceConf const&,
                                                                      ChronologTimer::RawSource = {});
    ChronologClientImpl(ChronologClientImpl const&) = delete;
    ChronologClientImpl& operator=(ChronologClientImpl const&) = delete;
    ~ChronologClientImpl();
    int SetRpcTimeoutPolicy(RpcTimeoutPolicy const&);
    int Connect(Clock::time_point = Clock::now());
    int Disconnect(Clock::time_point = Clock::now());
    ClientId client_id() const { return context_->clientId.load(std::memory_order_acquire); }
    int CreateChronicle(std::string const&, Clock::time_point = Clock::now());
    int DestroyChronicle(std::string const&, Clock::time_point = Clock::now());
    std::pair<int, StoryHandle*> AcquireStory(std::string const&, std::string const&, Clock::time_point = Clock::now());
    int ReleaseStory(std::string const&, std::string const&, Clock::time_point = Clock::now());
    int DestroyStory(std::string const&, std::string const&, Clock::time_point = Clock::now());
    std::pair<int, std::vector<std::string>> ShowChronicles(Clock::time_point = Clock::now());
    std::pair<int, std::vector<std::string>> ShowStories(std::string const&, Clock::time_point = Clock::now());
    int replay_story(ChronicleName const&,
                     StoryName const&,
                     uint64_t,
                     uint64_t,
                     std::vector<Event>&,
                     Clock::time_point = Clock::now());
    int replay_story(ChronicleName const&,
                     StoryName const&,
                     uint64_t,
                     uint64_t,
                     Client::EventCallback,
                     Clock::time_point = Clock::now());
    // Private producers: acquire while this facade is alive. Work retains the
    // actual context, never a facade/output buffer or an engine copy alone.
    std::shared_ptr<ClientRpcContext> context() const noexcept { return context_; }
    std::shared_ptr<rpc::RpcRuntime> runtime() const noexcept { return runtime_; }

private:
    enum class Action
    {
        Connect,
        Disconnect,
        CreateChronicle,
        DestroyChronicle,
        AcquireStory,
        ReleaseStory,
        DestroyStory,
        ShowChronicles,
        ShowStories
    };
    struct Reply
    {
        int status = CL_ERR_UNKNOWN;
        StoryHandle* handle = nullptr;
        std::vector<std::string> names;
        ClientId id = 0;
        std::shared_ptr<AcquireStoryResponseMsg> acquisition{};
    };
    class Work;
    ChronologClientImpl(std::shared_ptr<ClientRpcContext>, std::shared_ptr<rpc::RpcRuntime>, bool);
    Reply invoke(Action, std::string const&, std::string const&, Clock::time_point);
    static std::mutex factoryMutex_;
    static std::weak_ptr<ClientRpcContext> instance_;
    std::shared_ptr<ClientRpcContext> context_;
    std::shared_ptr<rpc::RpcRuntime> runtime_;
    bool facadeAdmitted_;
};
} // namespace chronolog
#endif
