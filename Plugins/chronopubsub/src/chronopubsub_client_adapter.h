#ifndef CHRONOPUBSUB_CLIENT_ADAPTER_H_
#define CHRONOPUBSUB_CLIENT_ADAPTER_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <chronolog_client.h>

namespace chronolog
{
class ClientConfiguration;
}

#include "chronopubsub_logger.h"
#include "chronopubsub_types.h"

namespace chronopubsub
{

class ChronoPubSubClientAdapter
{
private:
    std::unique_ptr<chronolog::Client> chronolog;
    const std::string defaultChronicle = "ChronoPubSubChronicle";
    LogLevel logLevel_;

    // Cache of acquired publish handles (one per topic). Reads do not use the
    // cache: each ReplayStory call acquires and releases its own handle.
    std::unordered_map<std::string, chronolog::StoryHandle*> publishHandleCache;
    mutable std::mutex cacheMutex;

    chronolog::StoryHandle* getOrAcquirePublishHandle(const std::string& topic);
    void flushCachedHandle(const std::string& topic);

    void initialize(const chronolog::ClientConfiguration& client_config);

public:
    explicit ChronoPubSubClientAdapter(LogLevel level);
    explicit ChronoPubSubClientAdapter(const std::string& config_path, LogLevel level);

    ~ChronoPubSubClientAdapter();

    std::uint64_t publishEvent(const std::string& topic, const std::string& payload);

    std::vector<Message> replayEvents(const std::string& topic, std::uint64_t start_ts, std::uint64_t end_ts);

    void flush();
};

} // namespace chronopubsub

#endif // CHRONOPUBSUB_CLIENT_ADAPTER_H_
