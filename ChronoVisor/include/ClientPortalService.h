#ifndef CLIENT_PORTAL_SERVICE_H
#define CLIENT_PORTAL_SERVICE_H

#include <thallium.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/vector.hpp>
#include <thallium/serialization/stl/unordered_map.hpp>

#include "chronolog_types.h"
#include "KeeperIdCard.h"

#include "VisorClientPortal.h"
#include "ConnectResponseMsg.h"
#include "AcquireStoryResponseMsg.h"

namespace tl = thallium;

namespace chronolog
{

class ClientPortalService: public thallium::provider <ClientPortalService>
{
public:

    static ClientPortalService*CreateClientPortalService(thallium::engine &tl_engine, uint16_t service_provider_id
                                                         , VisorClientPortal &visor_portal)
    {
        return new ClientPortalService(tl_engine, service_provider_id, visor_portal);
    }

    ~ClientPortalService()
    {
        LOG_DEBUG("[ClientPortalService] Destructor is called.");
        get_engine().pop_finalize_callback(this);
    }

    void Connect(tl::request const &request, uint32_t client_account, uint32_t client_host_ip, uint32_t client_pid)
    {
        ClientId client_id;
        uint64_t clock_offset;
        int return_code = theVisorClientPortal.ClientConnect(client_account, client_host_ip, client_pid, client_id
                                                             , clock_offset);
        if(chronolog::CL_SUCCESS == return_code)
        { request.respond(ConnectResponseMsg(chronolog::CL_SUCCESS, client_id)); }
        else
        { request.respond(ConnectResponseMsg(return_code, ClientId{0})); }
    }

    void Disconnect(tl::request const &request, ClientId const &client_token)
    {
        int return_code = theVisorClientPortal.ClientDisconnect(client_token);
        request.respond(return_code);
    }

    void CreateChronicle(tl::request const &request, ClientId const &client_id, std::string const &chronicle_name
                         , const std::map <std::string, std::string> &attrs, int &flags)
    {
        int return_code = theVisorClientPortal.CreateChronicle(client_id, chronicle_name, attrs, flags);
        request.respond(return_code);
    }

    void DestroyChronicle(tl::request const &request, ClientId const &client_id, ChronicleName const &chronicle_name)
    {
        int return_code = theVisorClientPortal.DestroyChronicle(client_id, chronicle_name);
        request.respond(return_code);
    }

    void AcquireStory(tl::request const &request, ClientId const &client_id, std::string const &chronicle_name
                      , std::string const &story_name, const std::map <std::string, std::string> &attrs
                      , int &flags)
    {
        AcquireStoryResponseMsg acquire_response = theVisorClientPortal.AcquireStory(client_id, chronicle_name
                                                                                     , story_name, attrs, flags);
        request.respond(acquire_response);
    }

    void ReleaseStory(tl::request const &request, ClientId const &client_id, std::string const &chronicle_name
                      , std::string const &story_name)
    {
        int return_code = theVisorClientPortal.ReleaseStory(client_id, chronicle_name, story_name);
        request.respond(return_code);
    }

    void DestroyStory(tl::request const &request, ClientId const &client_id, std::string const &chronicle_name
                      , std::string const &story_name)
    {
        request.respond(theVisorClientPortal.DestroyStory(client_id, chronicle_name, story_name));
    }

    void GetChronicleAttr(tl::request const &request, ClientId const &client_id, std::string const &chronicle_name
                          , std::string const &key, std::string &value)
    {
        int return_code = theVisorClientPortal.GetChronicleAttr(client_id, chronicle_name, key, value);
        request.respond(return_code);
    }

    void EditChronicleAttr(tl::request const &request, ClientId const &client_id, std::string const &chronicle_name
                           , std::string const &key, std::string const &value)
    {
        int return_code = theVisorClientPortal.EditChronicleAttr(client_id, chronicle_name, key, value);
        request.respond(return_code);
    }

    void ShowChronicles(tl::request const &request, ClientId const &client_id)
    {
        std::vector <std::string> chronicles;
        theVisorClientPortal.ShowChronicles(client_id, chronicles);
        request.respond(chronicles);
    }

    void ShowStories(tl::request const &request, ClientId const &client_id, const std::string &chronicle_name)
    {
        std::vector <std::string> stories;
        theVisorClientPortal.ShowStories(client_id, chronicle_name, stories);

        request.respond(stories);
    }

private:

    ClientPortalService(tl::engine &tl_engine, uint16_t service_provider_id, VisorClientPortal &visorPortal)
            : thallium::provider <ClientPortalService>(tl_engine, service_provider_id), theVisorClientPortal(
            visorPortal)
    {
        define("Connect", &ClientPortalService::Connect);
        define("Disconnect", &ClientPortalService::Disconnect);
        define("CreateChronicle", &ClientPortalService::CreateChronicle);
        define("DestroyChronicle", &ClientPortalService::DestroyChronicle);
        define("AcquireStory", &ClientPortalService::AcquireStory);
        define("ReleaseStory", &ClientPortalService::ReleaseStory);
        define("DestroyStory", &ClientPortalService::DestroyStory);
        define("GetChronicleAttr", &ClientPortalService::GetChronicleAttr);
        define("EditChronicleAttr", &ClientPortalService::EditChronicleAttr);
        define("ShowChronicles", &ClientPortalService::ShowChronicles);
        define("ShowStories", &ClientPortalService::ShowStories);
        //setup finalization callback in case this ser vice provider is still alive when the engine is finalized
        std::stringstream ss;
        ss << get_engine().self();
        LOG_INFO("[ClientPortalService] Started at address {} with provider id {}", ss.str(), service_provider_id);
        get_engine().push_finalize_callback(this, [p = this]()
        { delete p; });
    }

    ClientPortalService(ClientPortalService const &) = delete;

    ClientPortalService &operator=(ClientPortalService const &) = delete;

    VisorClientPortal &theVisorClientPortal;
};
}// namespace chronolog

#endif
