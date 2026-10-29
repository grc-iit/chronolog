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
        return new ClientPortalService(engine, id, std::move(core));
    }
    ~ClientPortalService() { get_engine().pop_finalize_callback(this); }

private:
    ClientPortalService(thallium::engine& engine, uint16_t id, std::shared_ptr<VisorClientPortal::Core> c)
        : thallium::provider<ClientPortalService>(engine, id)
        , core_(std::move(c))
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
        get_engine().push_finalize_callback(this, [this]() { delete this; });
    }
    void Connect(thallium::request const& r, uint32_t account, ClientId id, uint32_t version)
    {
        core_->Dispatch(r, VisorControlAction::Connect, id, "", "", account, version);
    }
    void Disconnect(thallium::request const& r, ClientId id) { core_->Dispatch(r, VisorControlAction::Disconnect, id); }
    void CreateChronicle(thallium::request const& r, ClientId id, std::string const& name)
    {
        core_->Dispatch(r, VisorControlAction::CreateChronicle, id, name);
    }
    void DestroyChronicle(thallium::request const& r, ClientId id, std::string const& name)
    {
        core_->Dispatch(r, VisorControlAction::DestroyChronicle, id, name);
    }
    void AcquireStory(thallium::request const& r, ClientId id, std::string const& name, std::string const& story)
    {
        core_->Dispatch(r, VisorControlAction::Acquire, id, name, story);
    }
    void ReleaseStory(thallium::request const& r, ClientId id, std::string const& name, std::string const& story)
    {
        core_->Dispatch(r, VisorControlAction::Release, id, name, story);
    }
    void DestroyStory(thallium::request const& r, ClientId id, std::string const& name, std::string const& story)
    {
        core_->Dispatch(r, VisorControlAction::DestroyStory, id, name, story);
    }
    void ShowChronicles(thallium::request const& r, ClientId id)
    {
        core_->Dispatch(r, VisorControlAction::ShowChronicles, id);
    }
    void ShowStories(thallium::request const& r, ClientId id, std::string const& name)
    {
        core_->Dispatch(r, VisorControlAction::ShowStories, id, name);
    }
    std::shared_ptr<VisorClientPortal::Core> core_;
};
} // namespace chronolog
#endif
