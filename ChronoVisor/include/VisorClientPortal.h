
#ifndef CHRONO_VISOR_CLIENT_PORTAL_H
#define CHRONO_VISOR_CLIENT_PORTAL_H

#include <string>
#include <vector>
#include <map>

#include "ConfigurationManager.h"


namespace chronolog
{


class VisorClientPortal
{

public:
	VisorClientPortal( ConfigurationManager const&);

	~VisorClientPortal();

int LocalConnect( const std::string &uri, std::string const &client_id, int &flags, uint64_t &clock_offset); //old

std::pair<int, uint32_t>  ClientConnect( uint32_t client_host_ip, std::string const &client_account, int &flags, uint64_t &clock_offset);

int LocalDisconnect(std::string const& client_account); //old
int ClientDisconnect(uint32_t client_id);

int LocalCreateChronicle( std::string const&name, const std::unordered_map<std::string, std::string> &attrs, int &flags);//old
int CreateChronicle( uint62_t client_id, std::string const&name, std::map<std::string, std::string> const& attrs, int &flags);

int LocalDestroyChronicle( std::string const&name);
int LocalDestroyStory(std::string const& chronicle_name, std::string const&story_name);

AcquireStoryResponseMsg LocalAcquireStory( 
                              std::string const&client_id,
                              std::string const&chronicle_name,
                              std::string const&story_name,
                              const std::unordered_map<std::string, std::string> &attrs,
                              int &flags);

int LocalReleaseStory( std::string const&client_id, std::string const&chronicle_name, std::string const&story_name);

int LocalGetChronicleAttr( std::string const&name, const std::string &key, std::string &value);

int LocalEditChronicleAttr( std::string const&name, const std::string &key, const std::string &value);

std::vector<std::string> & ShowChronicles( std::string &client_id, std::vector<std::string> &);

std::vector<std::string>& ShowStories( std::string &client_id, const std::string &chronicle_name, std::vector<std::string> &);



private:

	void authenticate_client( std::string const& client_account)
	{  return true; }

    chronolog::KeeperRegistry * keeperRegistry;
    std::shared_ptr<ClientRegistryManager> clientManager;
    std::shared_ptr<ChronicleMetaDirectory> chronicleMetaDirectory;

};

}

#endif
