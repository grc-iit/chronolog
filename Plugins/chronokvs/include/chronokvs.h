#ifndef CHRONOKVS_H_
#define CHRONOKVS_H_

#include <string>
#include <vector>
#include "../src/chronokvs_mapper.h"

namespace chronokvs
{

class ChronoKVS
{
private:
    std::unique_ptr <chronokvs_mapper> mapper;

public:

    ChronoKVS();

     * @brief Destructs the ChronoKVS instance.
     */
    ~ChronoKVS() = default;

    std::uint64_t put(const std::string &key, const std::string &value);

    std::string get(const std::string &key, uint64_t timestamp);

    std::vector <std::pair <uint64_t, std::string>> get_history(const std::string &key);
};

}

#endif // CHRONOKVS_H_