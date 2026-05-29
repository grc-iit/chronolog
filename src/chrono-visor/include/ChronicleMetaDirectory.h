#ifndef CHRONOLOG_CHRONICLEMETADIRECTORY_H
#define CHRONOLOG_CHRONICLEMETADIRECTORY_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Chronicle.h"
#include "chronolog_types.h"

class ClientRegistryManager;

typedef uint64_t StoryId;

class ChronicleMetaDirectory
{
public:
    ChronicleMetaDirectory();

    ~ChronicleMetaDirectory();

    void set_client_registry_manager(ClientRegistryManager* pClientRegistryManager)
    {
        clientRegistryManager_ = pClientRegistryManager;
    }

    std::unordered_map<uint64_t, Chronicle*>* getChronicleMap() { return chronicleMap_; }

    int create_chronicle(const std::string& name);

    int destroy_chronicle(const std::string& name);

    int destroy_story(std::string const& chronicle_name, const std::string& story_name);

    // Inspect destroy eligibility for a single story under the directory mutex.
    // Returns CL_ERR_NOT_EXIST if the chronicle or story doesn't exist;
    // CL_ERR_ACQUIRED if any acquirer other than requester_client_id holds it;
    // CL_SUCCESS otherwise. Sets caller_holds_it when the requester is the
    // sole acquirer (Visor must auto-release before destroying), and sets
    // story_id to the resolved id when available.
    int evaluate_story_destroy(std::string const& chronicle_name,
                               std::string const& story_name,
                               chronolog::ClientId const& requester_client_id,
                               StoryId& story_id,
                               bool& caller_holds_it);

    // Inspect destroy eligibility for every story in the chronicle. Returns
    // CL_ERR_NOT_EXIST if the chronicle doesn't exist; CL_ERR_ACQUIRED if any
    // story has an acquirer other than requester_client_id; CL_SUCCESS
    // otherwise. Populates stories_to_auto_release with (id, name) pairs for
    // stories the requester holds (and only the requester holds).
    int evaluate_chronicle_destroy(std::string const& chronicle_name,
                                   chronolog::ClientId const& requester_client_id,
                                   std::vector<std::pair<StoryId, std::string>>& stories_to_auto_release);

    int acquire_story(chronolog::ClientId const& client_id,
                      const std::string& chronicle_name,
                      const std::string& story_name,
                      StoryId&);

    int release_story(chronolog::ClientId const& client_id,
                      const std::string& chronicle_name,
                      const std::string& story_name,
                      StoryId&,
                      bool& was_last_acquirer);

    // Release every story currently acquired by client_id. Returns the StoryIds
    // of stories whose last acquirer was this client, so the caller can notify
    // the recording groups to stop only those.
    int release_all_acquired_stories(chronolog::ClientId const& client_id,
                                     std::vector<StoryId>& released_with_no_acquirers_left);

    int show_chronicles(std::vector<std::string>&);

    int show_stories(const std::string& chronicle_name, std::vector<std::string>&);

private:
    std::unordered_map<uint64_t, Chronicle*>* chronicleMap_;
    std::mutex g_chronicleMetaDirectoryMutex_;
    ClientRegistryManager* clientRegistryManager_ = nullptr;
};

#endif //CHRONOLOG_CHRONICLEMETADIRECTORY_H
