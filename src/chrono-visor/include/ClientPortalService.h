#ifndef CLIENT_PORTAL_SERVICE_H
#define CLIENT_PORTAL_SERVICE_H
#include <VisorClientPortal.h>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
namespace chronolog
{
class ClientPortalService: public thallium::provider<ClientPortalService>
{
public:
    static ClientPortalService*
    CreateClientPortalService(thallium::engine& engine, uint16_t id, std::shared_ptr<VisorClientPortal::Core> core)
    {
        auto service = new ClientPortalService(engine, id, core);
        service->legacyCore_ = std::move(core);
        service->legacy_ = true;
        service->registerProcedures();
        engine.push_finalize_callback(service, [service] { delete service; });
        return service;
    }
    static ClientPortalService* CreateOwned(thallium::engine& engine,
                                            uint16_t id,
                                            std::shared_ptr<VisorClientPortal::Core> const& core,
                                            std::shared_ptr<ClientPortalService>& root)
    {
        if(root)
            throw std::logic_error("Portal provider already exists for this MID");
        root.reset(new ClientPortalService(engine, id, core));
        root->registerProcedures(); // Retain partial registration before exposure.
        return root.get();
    }
    ~ClientPortalService()
    {
        if(legacy_)
            get_engine().pop_finalize_callback(this);
    }

private:
    ClientPortalService(thallium::engine& engine, uint16_t id, std::shared_ptr<VisorClientPortal::Core> c)
        : thallium::provider<ClientPortalService>(engine, id)
        , core_(std::move(c))
    {}
    void registerProcedures()
    {
        define("Connect", &ClientPortalService::Connect);
        define("Disconnect", &ClientPortalService::Disconnect);
        define("CreateChronicle", &ClientPortalService::CreateChronicle);
        define("DestroyChronicle", &ClientPortalService::DestroyChronicle);
        define("AcquireStory", &ClientPortalService::AcquireStory);
        define("ReleaseStory", &ClientPortalService::ReleaseStory);
        define("DestroyStory", &ClientPortalService::DestroyStory);
        define("ShowChronicles", &ClientPortalService::ShowChronicles);
        define("ShowStories", &ClientPortalService::ShowStories);
    }
    void Connect(thallium::request const& r, uint32_t account, ClientId id, uint32_t version)
    {
        if(auto core = core_.lock())
            core->Dispatch(r, VisorControlAction::Connect, id, "", "", account, version);
    }
    void Disconnect(thallium::request const& r, ClientId id)
    {
        if(auto core = core_.lock())
            core->Dispatch(r, VisorControlAction::Disconnect, id);
    }
    void CreateChronicle(thallium::request const& r, ClientId id, std::string const& name)
    {
        if(auto core = core_.lock())
            core->Dispatch(r, VisorControlAction::CreateChronicle, id, name);
    }
    void DestroyChronicle(thallium::request const& r, ClientId id, std::string const& name)
    {
        if(auto core = core_.lock())
            core->Dispatch(r, VisorControlAction::DestroyChronicle, id, name);
    }
    void AcquireStory(thallium::request const& r, ClientId id, std::string const& name, std::string const& story)
    {
        if(auto core = core_.lock())
            core->Dispatch(r, VisorControlAction::Acquire, id, name, story);
    }
    void ReleaseStory(thallium::request const& r, ClientId id, std::string const& name, std::string const& story)
    {
        if(auto core = core_.lock())
            core->Dispatch(r, VisorControlAction::Release, id, name, story);
    }
    void DestroyStory(thallium::request const& r, ClientId id, std::string const& name, std::string const& story)
    {
        if(auto core = core_.lock())
            core->Dispatch(r, VisorControlAction::DestroyStory, id, name, story);
    }
    void ShowChronicles(thallium::request const& r, ClientId id)
    {
        if(auto core = core_.lock())
            core->Dispatch(r, VisorControlAction::ShowChronicles, id);
    }
    void ShowStories(thallium::request const& r, ClientId id, std::string const& name)
    {
        if(auto core = core_.lock())
            core->Dispatch(r, VisorControlAction::ShowStories, id, name);
    }
    std::weak_ptr<VisorClientPortal::Core> core_;
    std::shared_ptr<VisorClientPortal::Core> legacyCore_;
    bool legacy_ = false;
};
} // namespace chronolog
#endif
