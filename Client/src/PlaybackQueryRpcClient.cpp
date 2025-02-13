
#include <thallium.hpp>
#include <thallium/serialization/serialize.hpp>
#include <thallium/serialization/stl/string.hpp>
#include <thallium/serialization/stl/map.hpp>

#include "chrono_monitor.h"
#include "chronolog_errcode.h"
#include "ServiceId.h"
#include "PlaybackQueryRpcClient.h"
#include "ClientQueryService.h"


namespace tl = thallium;

namespace chl = chronolog;

 // constructor is private to make sure thalium rpc objects are created on the heap, not stack
chl::PlaybackQueryRpcClient::PlaybackQueryRpcClient(chl::ClientQueryService & clientQueryService
                , chl::ServiceId const& playback_service_id)
    : theClientQueryService(clientQueryService)
    , playback_service_id(playback_service_id)
{
    std::string service_addr_string;
    playback_service_id.get_service_as_string(service_addr_string);

    playback_service_handle = tl::provider_handle(theClientQueryService.get_engine().lookup(service_addr_string), playback_service_id.getProviderId());

    playback_service_available = theClientQueryService.get_engine().define("playback_service_available");
    story_playback_request = theClientQueryService.get_engine().define("story_playback_request");
}

chl::PlaybackQueryRpcClient::~PlaybackQueryRpcClient()
{
    playback_service_available.deregister();
    story_playback_request.deregister();
}
//////////////

int chl::PlaybackQueryRpcClient::is_playback_service_available()
{

    try
    {
        return ( playback_service_available.on(playback_service_handle)() );
    }
    catch (tl::exception const &ex)
    {
        LOG_ERROR("[PlaybackQueryRpcClient] Playback Service unavailable at {}, exceptiomn:{}", chl::to_string(playback_service_id), ex.what());
    }
    
    return chl::CL_ERR_UNKNOWN;
}
    
int chl::PlaybackQueryRpcClient::send_story_playback_request(chl::ChronicleName const &chronicle_name, chl::StoryName const &story_name, uint64_t start_time, uint64_t end_time)
{
    int return_code = chl::CL_ERR_UNKNOWN;

    uint32_t query_id = theClientQueryService.start_new_query( chronicle_name,story_name,start_time,end_time);
    
    try
    {
        LOG_DEBUG("[PlaybackQueryRpcClient] {} ; send_story_playback_request for Story {}{}", chl::to_string(playback_service_id), chronicle_name,story_name);
        story_playback_request.on(playback_service_handle)( theClientQueryService.get_service_id(), query_id, chronicle_name,story_name,start_time,end_time);

        return chl::CL_SUCCESS;

    } 
    catch (tl::exception const& ex)
    {
        LOG_ERROR("[PlaybackQueryRpcClient] {} ; send_story_playback_request exception {}", chl::to_string(playback_service_id), ex.what());
    }

    return return_code;
}


