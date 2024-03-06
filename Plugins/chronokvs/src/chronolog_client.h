#ifndef CHRONOLOG_CLIENT_H_
#define CHRONOLOG_CLIENT_H_

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include "mock_chronolog.h"

namespace chronolog
{
/**
 * Client for interacting with the ChronoLog system.
 */
class ChronoLogClient
{
private:
    std::shared_ptr <MockChronolog> chronolog;
    StoryHandle*storyHandle;

public:
    ChronoLogClient();

    ~ChronoLogClient();

    std::uint64_t storeEvent(const std::string &serializedEvent);

    std::vector <std::string> retrieveEvents(std::uint64_t timestamp);
};
}
#endif // CHRONOLOG_CLIENT_H_
