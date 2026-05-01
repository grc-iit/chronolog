#ifndef STORY_ACQUISITION_RESPONSE_MSG_H
#define STORY_ACQUISITION_RESPONSE_MSG_H

#include <iostream>
#include <vector>

#include "chronolog_types.h"
#include "ServiceId.h"
#include "client_errcode.h"

namespace chronolog
{

class AcquireStoryResponseMsg
{
    int error_code;
    StoryId storyId;
    std::vector<ServiceId> keepers;
    ServiceId player;

public:
    AcquireStoryResponseMsg()
        : error_code(chronolog::CL_SUCCESS)
        , storyId(0)
        , player(ServiceId())
    {}

    AcquireStoryResponseMsg(int code,
                            StoryId const& story_id,
                            std::vector<ServiceId> const& keepers_to_use,
                            ServiceId const& player_to_use = ServiceId())
        : error_code(code)
        , storyId(story_id)
        , keepers(keepers_to_use)
        , player(player_to_use)
    {}

    ~AcquireStoryResponseMsg() = default;

    int getErrorCode() const { return error_code; }

    StoryId const& getStoryId() const { return storyId; }

    std::vector<ServiceId> const& getKeepers() const { return keepers; }

    ServiceId const& getPlayer() const { return player; }

    template <typename SerArchiveT>
    void serialize(SerArchiveT& serT)
    {
        serT & error_code;
        serT & storyId;
        serT & keepers;
        serT & player;
    }
};

} // namespace chronolog


inline std::ostream& operator<<(std::ostream& out, chronolog::AcquireStoryResponseMsg const& msg)
{
    out << "AcquireStoryResponseMsg{" << msg.getErrorCode() << "}{story_id:" << msg.getStoryId() << "}{";
    for(chronolog::ServiceId const& keeper_service: msg.getKeepers()) { out << keeper_service; }
    out << "}{" << msg.getPlayer() << "}";

    return out;
}

#endif
