#ifndef PLAYER_STATS_MSG_H
#define PLAYER_STATS_MSG_H

#include <iostream>
#include "PlayerIdCard.h"


namespace chronolog
{

class PlayerStatsMsg
{

    PlayerIdCard playerIdCard;
    uint32_t active_story_count;

public:


    PlayerStatsMsg(PlayerIdCard const & player_card = PlayerIdCard{}, uint32_t count = 0)
        : playerIdCard(player_card)
        , active_story_count(count)
    {}

    ~PlayerStatsMsg() = default;

    PlayerIdCard const & getPlayerIdCard() const
    { return playerIdCard; }

    uint32_t getActiveStoryCount() const
    { return active_story_count; }

    template <typename SerArchiveT>
    void serialize(SerArchiveT & serT)
    {
        serT & playerIdCard;
        serT & active_story_count;
    }

};

inline std::string to_string(chronolog::PlayerStatsMsg const &stats_msg)
{
    return std::string("PlayerStatsMsg{") + to_string(stats_msg.getPlayerIdCard()) + "}";
}


}

inline std::ostream & operator<<(std::ostream &out, chronolog::PlayerStatsMsg const &stats_msg)
{
    out << "PlayerStatsMsg{" << stats_msg.getPlayerIdCard() << "}";
    return out;
}

inline std::string & operator+= (std::string & a_string, chronolog::PlayerStatsMsg const &stats_msg)
{
    a_string += std::string("PlayerStatsMsg{") + chronolog::to_string(stats_msg.getPlayerIdCard()) + "}";
    return a_string;
}

#endif
