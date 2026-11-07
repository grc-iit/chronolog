#ifndef CHRONO_VISOR_CLIENT_PORTAL_H
#define CHRONO_VISOR_CLIENT_PORTAL_H
#include <thallium.hpp>
#include <VisorAdminRuntime.h>
#include <ClientRegistryManager.h>
#include <ChronicleMetaDirectory.h>
#include <AcquireStoryResponseMsg.h>
#include <ConfigurationBlocks.h>
#include <memory>
#include <mutex>
namespace chronolog
{
class KeeperRegistry;
class ClientPortalService;
enum class VisorControlAction
{
    Connect,
    Disconnect,
    CreateChronicle,
    DestroyChronicle,
    Acquire,
    Release,
    DestroyStory,
    ShowChronicles,
    ShowStories
};
struct VisorControlReply
{
    int status = CL_ERR_UNKNOWN;
    ClientId client = 0;
    AcquireStoryResponseMsg acquisition;
    std::vector<std::string> names;
};
class VisorClientPortal
{
public:
    // Provider and admitted work share this state. No continuation borrows the
    // public portal, metadata directory, client manager, or registry lifetime.
    struct Core: std::enable_shared_from_this<Core>
    {
        Core();
        int StartServices(RPCProviderConf const&, std::shared_ptr<KeeperRegistry>);
        int StartOwnedServices(RPCProviderConf const&,
                               std::shared_ptr<KeeperRegistry>,
                               std::shared_ptr<thallium::engine> const&);
        void releaseOwnedServices(); // Charged worker, after actual native handler fence.
        bool quiesce(rpc::RpcDeadline const&);
        void Dispatch(thallium::request const&,
                      VisorControlAction,
                      ClientId,
                      std::string const& chronicle = {},
                      std::string const& story = {},
                      uint32_t account = 0,
                      uint32_t protocol = 3);
        std::shared_ptr<VisorControlReply> Invoke(VisorControlAction,
                                                  ClientId,
                                                  std::string const& chronicle = {},
                                                  std::string const& story = {},
                                                  uint32_t account = 0);
        std::shared_ptr<VisorControlReply> Execute(VisorControlAction,
                                                   ClientId,
                                                   std::string const&,
                                                   std::string const&,
                                                   uint32_t,
                                                   uint32_t,
                                                   rpc::RpcDeadline const&);
        std::atomic<bool> shuttingDown{false};
        std::atomic<std::size_t> incomingRequests{0};
        std::shared_ptr<KeeperRegistry> registry;
        std::shared_ptr<thallium::engine> engine;
        ClientPortalService* provider = nullptr;
        std::shared_ptr<ClientPortalService> ownedProvider;
        ClientRegistryManager clients;
        ChronicleMetaDirectory directory;
        std::mutex operationMutex;
        std::timed_mutex admissionMutex;
    };
    VisorClientPortal();
    ~VisorClientPortal();
    int StartServices(RPCProviderConf const&, std::shared_ptr<KeeperRegistry>);
    void ShutdownServices();
    int ClientConnect(uint32_t, ClientId&, uint64_t&);
    int ClientDisconnect(ClientId const&);
    int CreateChronicle(ClientId const&, ChronicleName const&);
    int DestroyChronicle(ClientId const&, ChronicleName const&);
    int DestroyStory(ClientId const&, std::string const&, std::string const&);
    AcquireStoryResponseMsg AcquireStory(ClientId const&, std::string const&, std::string const&);
    int ReleaseStory(ClientId const&, std::string const&, std::string const&);
    int ShowChronicles(ClientId const&, std::vector<std::string>&);
    int ShowStories(ClientId const&, std::string const&, std::vector<std::string>&);
    std::shared_ptr<Core> core() const { return core_; }

private:
    std::shared_ptr<Core> core_;
};
} // namespace chronolog
#endif
