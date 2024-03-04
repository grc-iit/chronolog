#ifndef CHRONOKVS_H_
#define CHRONOKVS_H_

#include <string>
#include <optional>
#include <vector>

namespace chronolog
{

/**
 * @brief Represents the interface for the ChronoLog Key-Value Store.
 *
 * ChronoKVS is a key-value store that allows storing strings indexed by
 * a string key and supports retrieving the history of these values based on
 * timestamps.
 */
class ChronoKVS
{
public:
    /**
     * @brief Constructs a new instance of ChronoKVS.
     */
    ChronoKVS();

    /**
     * @brief Destructs the ChronoKVS instance.
     */
    ~ChronoKVS();

    /**
     * @brief Stores a key-value pair with an associated timestamp.
     *
     * @param key The key under which the value is stored.
     * @param value The value to be stored.
     * @return A timestamp (as std::uint64_t) indicating when the value was stored.
     */
    static std::uint64_t put(const std::string &key, const std::string &value);

    /**
     * @brief Retrieves all key-value pairs that match the given timestamp.
     *
     * @param timestamp The timestamp for which to retrieve the key-value pairs.
     * @return A vector of key-value pairs (as std::pair<std::string, std::string>)
     *         that were stored at the given timestamp.
     */
    static std::vector <std::pair <std::string, std::string>> get(uint64_t timestamp);

    /**
     * @brief Retrieves all timestamps and values associated with a given key.
     *
     * @param key The key for which to retrieve the timestamps and values.
     * @return A vector of timestamp-value pairs (as std::pair<uint64_t, std::string>)
     *         associated with the given key.
     */
    static std::vector <std::pair <uint64_t, std::string>> get(const std::string &key);

    /**
     * @brief Retrieves the value associated with a given key at a specific timestamp.
     *
     * @param key The key for which to retrieve the value.
     * @param timestamp The timestamp at which the value was stored.
     * @return The value (as std::string) associated with the given key at the specified
     *         timestamp, or an empty string if no such value exists.
     */
    static std::vector <std::string> get(const std::string &key, uint64_t timestamp);
};

} // namespace chronolog

#endif // CHRONOKVS_H_