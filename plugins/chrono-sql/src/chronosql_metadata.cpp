#include "chronosql_metadata.h"

#include <algorithm>
#include <stdexcept>

#include <json-c/json.h>

namespace chronosql
{
namespace
{

json_object* columnsToJson(const std::vector<Column>& cols)
{
    json_object* arr = json_object_new_array();
    for(const auto& c: cols)
    {
        json_object* obj = json_object_new_object();
        json_object_object_add(obj, "name", json_object_new_string(c.name.c_str()));
        json_object_object_add(obj, "type", json_object_new_string(columnTypeToString(c.type)));
        json_object_array_add(arr, obj);
    }
    return arr;
}

std::string toJsonString(json_object* obj)
{
    const char* s = json_object_to_json_string_ext(obj, JSON_C_TO_STRING_PLAIN);
    std::string out(s ? s : "");
    json_object_put(obj);
    return out;
}

} // namespace

std::string encodeCreateOp(const MetaCreateOp& op)
{
    json_object* root = json_object_new_object();
    json_object_object_add(root, "op", json_object_new_string("create_table"));
    json_object_object_add(root, "name", json_object_new_string(op.table.c_str()));
    json_object_object_add(root, "columns", columnsToJson(op.columns));
    return toJsonString(root);
}

std::string encodeDropOp(const MetaDropOp& op)
{
    json_object* root = json_object_new_object();
    json_object_object_add(root, "op", json_object_new_string("drop_table"));
    json_object_object_add(root, "name", json_object_new_string(op.table.c_str()));
    return toJsonString(root);
}

void MetadataState::apply(const std::string& payload)
{
    json_tokener* tok = json_tokener_new();
    json_object* root = json_tokener_parse_ex(tok, payload.c_str(), static_cast<int>(payload.size()));
    json_tokener_error err = json_tokener_get_error(tok);
    json_tokener_free(tok);
    if(!root || err != json_tokener_success || !json_object_is_type(root, json_type_object))
    {
        if(root)
        {
            json_object_put(root);
        }
        // Skip malformed metadata events rather than aborting replay.
        return;
    }

    json_object* op_obj = nullptr;
    json_object* name_obj = nullptr;
    if(!json_object_object_get_ex(root, "op", &op_obj) || !json_object_object_get_ex(root, "name", &name_obj))
    {
        json_object_put(root);
        return;
    }
    std::string op = json_object_get_string(op_obj);
    std::string table = json_object_get_string(name_obj);

    if(op == "create_table")
    {
        json_object* cols_obj = nullptr;
        if(!json_object_object_get_ex(root, "columns", &cols_obj) || !json_object_is_type(cols_obj, json_type_array))
        {
            json_object_put(root);
            return;
        }
        MetaCreateOp create_op;
        create_op.table = table;
        std::size_t n = json_object_array_length(cols_obj);
        for(std::size_t i = 0; i < n; ++i)
        {
            json_object* col_obj = json_object_array_get_idx(cols_obj, static_cast<int>(i));
            if(!json_object_is_type(col_obj, json_type_object))
            {
                continue;
            }
            json_object* cname_obj = nullptr;
            json_object* ctype_obj = nullptr;
            if(!json_object_object_get_ex(col_obj, "name", &cname_obj))
            {
                continue;
            }
            Column c;
            c.name = json_object_get_string(cname_obj);
            if(json_object_object_get_ex(col_obj, "type", &ctype_obj))
            {
                auto resolved = columnTypeFromString(json_object_get_string(ctype_obj));
                if(resolved)
                {
                    c.type = *resolved;
                }
            }
            create_op.columns.push_back(std::move(c));
        }
        recordCreate(create_op);
    }
    else if(op == "drop_table")
    {
        recordDrop(MetaDropOp{table});
    }
    json_object_put(root);
}

bool MetadataState::has(const std::string& table) const { return schemas_.find(table) != schemas_.end(); }

std::optional<Schema> MetadataState::get(const std::string& table) const
{
    auto it = schemas_.find(table);
    if(it == schemas_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::vector<std::string> MetadataState::listTables() const
{
    std::vector<std::string> out;
    out.reserve(order_.size());
    for(const auto& t: order_)
    {
        if(schemas_.find(t) != schemas_.end())
        {
            out.push_back(t);
        }
    }
    return out;
}

void MetadataState::clear()
{
    order_.clear();
    schemas_.clear();
}

void MetadataState::recordCreate(const MetaCreateOp& op)
{
    if(schemas_.find(op.table) != schemas_.end())
    {
        return; // already exists; replay idempotency.
    }
    Schema s;
    s.table = op.table;
    s.columns = op.columns;
    schemas_.emplace(op.table, std::move(s));
    if(std::find(order_.begin(), order_.end(), op.table) == order_.end())
    {
        order_.push_back(op.table);
    }
}

void MetadataState::recordDrop(const MetaDropOp& op)
{
    schemas_.erase(op.table);
    // Keep order_ entry; it filters via schemas_ in listTables().
}

} // namespace chronosql
