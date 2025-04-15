#ifndef CHRONO_VISOR_CLIENT_PORTAL_H
#define CHRONO_VISOR_CLIENT_PORTAL_H

#include <string>
#include <vector>
#include <map>

#include <thallium.hpp>

#include <chronolog_types.h>
#include <ConfigurationManager.h>

#include <AcquireStoryResponseMsg.h>
#include "ClientRegistryManager.h"
#include "ChronicleMetaDirectory.h"

namespace chronolog
{

class ClientPortalService;

class KeeperRegistry;
//class ClientRegistryManager;
//class ChronicleMetaDirectory;

class VisorClientPortal
{

    enum ClientPortalState
    {
        UNKNOWN = 0, INITIALIZED = 1, RUNNING = 2, // RegistryService and ClientPortalService are up and running
        SHUTTING_DOWN = 3 // Shutting down services
    };


public:
    VisorClientPortal();

    ~VisorClientPortal();

    int StartServices(ChronoLog::ConfigurationManager const &, KeeperRegistry*);

    void ShutdownServices();

    int ClientConnect(uint32_t client_account, uint32_t client_host_ip, uint32_t client_pid, ClientId &
                      , uint64_t &clock_offset);

    int ClientDisconnect(ClientId const &client_id);

    int CreateChronicle(ClientId const &name, ChronicleName const &
                        , const std::map <std::string, std::string> &attrs, int &flags);

    int DestroyChronicle(ClientId const &client_id, ChronicleName const &chronicle_name);

    int DestroyStory(ClientId const &client_id, std::string const &chronicle_name, std::string const &story_name);

    AcquireStoryResponseMsg AcquireStory(ClientId const &client_id,
            std::string const &chronicle_name, std::string const &story_name
            , const std::map <std::string, std::string> &attrs, int &flags);

    int ReleaseStory(ClientId const &client_id, std::string const &chronicle_name, std::string const &story_name);

/*int ReleaseStory( ClientId const&client_id, StoryId const&);
int DestroyStory( ClientId const&client_id, StoryId const&);
*/
    int
    GetChronicleAttr(ClientId const &, std::string const &chronicle_name, std::string const &key, std::string &value);

    int
    EditChronicleAttr(ClientId const &, std::string const &chronicle, std::string const &key, std::string const &value);

    int ShowChronicles(ClientId const &client_id, std::vector <std::string> &);

    int ShowStories(ClientId const &client_id, std::string const &chronicle_name, std::vector <std::string> &);


private:

    // role based authentication dummies return true for any client_id for the time being
    // TODO: will be implemented when we add ClientAuthentication module
    bool is_client_authenticated(uint32_t client_account);

    bool chronicle_action_is_authorized(ClientId const &, ChronicleName const &);

    bool story_action_is_authorized(ClientId const &, ChronicleName const &, StoryName const &);

    //ChronoLog::ConfigurationManager & visorConfiguration; 
    ClientPortalState clientPortalState;
    thallium::engine*clientPortalEngine;
    ClientPortalService*clientPortalService;
    chronolog::KeeperRegistry*theKeeperRegistry;
    ClientRegistryManager clientManager;
    ChronicleMetaDirectory chronicleMetaDirectory;


};

}

#endif
