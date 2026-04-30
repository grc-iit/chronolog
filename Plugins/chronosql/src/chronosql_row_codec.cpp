#include "chronosql_row_codec.h"

#include <stdexcept>
#include <string>

#include <json-c/json.h>

namespace chronosql
{

std::string encodeRow(const std::map<std::string, std::string>& row)
{
    json_object* obj = json_object_new_object();
    for(const auto& [k, v]: row) { json_object_object_add(obj, k.c_str(), json_object_new_string(v.c_str())); }
    const char* out = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
    std::string result(out ? out : "{}");
    json_object_put(obj);
    return result;
}

std::map<std::string, std::string> decodeRow(const std::string& payload)
{
    json_tokener* tok = json_tokener_new();
    json_object* root = json_tokener_parse_ex(tok, payload.c_str(), static_cast<int>(payload.size()));
    json_tokener_error err = json_tokener_get_error(tok);
    json_tokener_free(tok);

    if(!root || err != json_tokener_success)
    {
        if(root)
        {
            json_object_put(root);
        }
        throw std::runtime_error("ChronoSQL: failed to parse row JSON payload");
    }

    if(!json_object_is_type(root, json_type_object))
    {
        json_object_put(root);
        throw std::runtime_error("ChronoSQL: row payload must be a JSON object");
    }

    std::map<std::string, std::string> out;
    json_object_object_foreach(root, key, val)
    {
        if(json_object_is_type(val, json_type_string))
        {
            out.emplace(key, json_object_get_string(val));
        }
        else
        {
            // Non-string values are stringified for uniform handling.
            const char* s = json_object_to_json_string_ext(val, JSON_C_TO_STRING_PLAIN);
            out.emplace(key, s ? s : "");
        }
    }
    json_object_put(root);
    return out;
}

} // namespace chronosql
