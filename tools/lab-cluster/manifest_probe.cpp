#include "common/tier/FileTierStore.h"
#include <chrono>
#include <iostream>
#include <set>
#include <thread>

int main(int argc, char** argv)
{
    if(argc < 3)
        return 2;
    auto store = chronolog::FileTierStore::OpenReadOnly(argv[1], std::chrono::milliseconds(200));
    if(!store.ok())
    {
        std::cerr << store.status() << std::endl;
        return 1;
    }
    std::set<std::string> seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
    while(std::chrono::steady_clock::now() < deadline)
    {
        for(int i = 2; i < argc; ++i)
        {
            auto records = (*store)->manifest(std::stoull(argv[i]));
            if(absl::IsNotFound(records.status()))
                continue;
            if(!records.ok())
            {
                std::cerr << records.status() << std::endl;
                return 1;
            }
            for(const auto& record: *records)
                if(seen.insert(record.manifest_writer + ":" + record.chunk_id).second)
                    std::cout << "archive_visible chunk=" << record.chunk_id << " story=" << record.story_id
                              << " writer=" << record.manifest_writer << " count=" << record.event_count
                              << " monotonic_ns="
                              << std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count()
                              << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
