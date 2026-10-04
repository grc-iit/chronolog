#include "chrono-grapher/server/GrapherConfig.h"
#include <iostream>

int main(int argc, char** argv)
{
    using namespace chronolog;
    if(argc != 3)
        return 2;
    auto config = grapher::GrapherConfig::load(argv[2]);
    if(!config.ok())
    {
        std::cerr << config.status() << '\n';
        return 1;
    }
    const bool seed = std::string(argv[1]) == "seed";
    auto store = seed ? FileTierStore::Open(config->archive_root,
                                            config->manifest_writer,
                                            {{1, {100, 0}}},
                                            std::make_shared<ProtoChunkCodec>(),
                                            {},
                                            {},
                                            0,
                                            {},
                                            {},
                                            config->tierChain())
                      : FileTierStore::OpenReadOnly(config->archive_root);
    if(!store.ok())
    {
        std::cerr << store.status() << '\n';
        return 1;
    }
    Event event;
    event.id = {1, 7, 1, 1};
    event.hlc = {110, 0};
    event.envelope.payload = "restart-event";
    if(seed)
    {
        Chunk chunk;
        chunk.id = "restart";
        chunk.story_id = 1;
        chunk.start = {100, 0};
        chunk.end = {200, 0};
        chunk.events = {event};
        auto record = (*store)->publish(chunk);
        if(!record.ok() || record->state != ManifestState::Published)
            return 1;
        return 0;
    }
    auto chain = config->tierChain();
    if(!(*store)->configureTiers(chain.deployment_id, chain.tiers).ok() || !(*store)->probeTiers().ok())
        return 1;
    auto records = (*store)->manifest(1);
    if(!records.ok())
        return 1;
    bool migrated = false;
    for(const auto& record: *records)
    {
        if(record.state == ManifestState::Lost)
            return 1;
        auto location = (*store)->location(record.file);
        if(!location.ok())
            return 1;
        if(*location)
        {
            migrated = true;
            if((*location)->tier != "slow" || !std::filesystem::exists(chain.tiers[0].root / record.file) ||
               std::filesystem::exists(std::filesystem::path(config->archive_root) / record.file))
                return 1;
        }
    }
    auto read = (*store)->read(1, {Range::Axis::Hlc, {100, 0}, {200, 0}});
    if(!migrated || !read.ok() || read->size() != 1 || read->front().id != event.id || read->front().hlc != event.hlc ||
       read->front().envelope.payload != event.envelope.payload)
        return 1;
    std::cout << "MIGRATED RESTART PASSED identical_events=1 effective_tier=slow\n";
}
