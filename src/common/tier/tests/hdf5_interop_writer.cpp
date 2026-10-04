#include "tier/FileTierStore.h"
#include <iostream>

int main(int argc, char** argv)
{
    if(argc != 2)
        return 2;
    auto store = chronolog::FileTierStore::Open(argv[1], "interop", {{42, {100, 3}}});
    if(!store.ok())
        return 1;
    chronolog::Event event;
    event.id = {42, 7, 8, 9};
    event.hlc = {123, 4};
    event.physical = {-17, 99, chronolog::ClockStatus::Synced};
    event.durability = chronolog::Durability::Durable;
    event.envelope = {"application/octet-stream",
                      std::string("a\0\xffz", 4),
                      std::string(16, '\0'),
                      std::string(8, '\xff'),
                      {{"host", "dragon"}}};
    auto empty = event;
    empty.id.sequence++;
    empty.envelope = {};
    empty.physical.uncertainty_ns.reset();
    chronolog::Chunk chunk{"interop", 42, {100, 3}, {200, 5}, {event, empty}, false};
    auto record = (*store)->publish(chunk);
    if(!record.ok())
    {
        std::cerr << record.status();
        return 1;
    }
    std::cout << (std::filesystem::path(argv[1]) / record->file).string() << '\n';
}
