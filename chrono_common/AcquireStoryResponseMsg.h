#ifndef STORY_ACQUISITION_RESPONSE_MSG_H
#define STORY_ACQUISITION_RESPONSE_MSG_H

#include <iostream>
#include "chronolog_types.h"
#include "ServiceId.h"
#include "KeeperIdCard.h"

namespace chronolog
{

class AcquireStoryResponseMsg
{
    int error_code;
    StoryId storyId;
    std::vector <KeeperIdCard> keepers;
    ServiceId player;

public:

    AcquireStoryResponseMsg()
        : error_code(chronolog::CL_SUCCESS)
        , storyId(0)
        , player(ServiceId())
    {}

    AcquireStoryResponseMsg(int code, StoryId const &story_id, std::vector <KeeperIdCard> const &keepers_to_use
                        , ServiceId const& player_to_use=ServiceId())
        : error_code(code)
        , storyId(story_id)
        , keepers(keepers_to_use)
        , player(player_to_use)
    {}

    ~AcquireStoryResponseMsg() = default;

    int getErrorCode() const
    { return error_code; }

    StoryId const &getStoryId() const
    { return storyId; }

    std::vector <KeeperIdCard> const &getKeepers() const
    { return keepers; }

    ServiceId const& getPlayer() const
    { return player; }

    template <typename SerArchiveT>
    void serialize(SerArchiveT &serT)
    {
        serT & error_code;
        serT & storyId;
        serT & keepers;
        serT & player;
    }

};

}//namespace


inline std::ostream &operator<<(std::ostream &out, chronolog::AcquireStoryResponseMsg const &msg)
{
    out << "AcquireStoryResponseMsg{" << msg.getErrorCode() << "}{story_id:" << msg.getStoryId() << "}{";
    for(chronolog::KeeperIdCard keeper_card: msg.getKeepers())
    { out << keeper_card; }
    out << "}{"<<msg.getPlayer()<<"}";

    return out;
}

#endif
