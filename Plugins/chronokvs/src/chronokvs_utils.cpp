#include "chronokvs_utils.h"

std::string serialize(const std::string &key, const std::string &value)
{
    return key + ":" + value; // Simple serialization for demonstration
}

std::pair <std::string, std::string> deserialize(const std::string &serialized)
{
    size_t delimiterPos = serialized.find(":");
    if(delimiterPos != std::string::npos)
    {
        return {serialized.substr(0, delimiterPos), serialized.substr(delimiterPos + 1)};
    }
    return {"", ""}; // Error case handling
}
