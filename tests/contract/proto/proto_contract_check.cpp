#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace descriptor_pb = google::protobuf;
namespace
{
struct Schema
{
    descriptor_pb::DescriptorPool pool;
    std::vector<const descriptor_pb::FileDescriptor*> files;
    bool load(const std::string& path)
    {
        std::ifstream stream(path, std::ios::binary);
        descriptor_pb::FileDescriptorSet set;
        if(!stream || !set.ParseFromIstream(&stream))
        {
            std::cerr << "Cannot read descriptor set: " << path << '\n';
            return false;
        }
        for(const auto& proto: set.file())
        {
            auto* file = pool.BuildFile(proto);
            if(!file)
                return false;
            if(file->package().starts_with("chronolog."))
                files.push_back(file);
        }
        return !files.empty();
    }
};

int failures{};
void require(bool condition, std::string_view name, std::string_view rule)
{
    if(!condition)
    {
        ++failures;
        std::cerr << name << ": " << rule << '\n';
    }
}

void checkEnum(const descriptor_pb::EnumDescriptor* before, const descriptor_pb::DescriptorPool& after)
{
    auto* current = after.FindEnumTypeByName(before->full_name());
    require(current, before->full_name(), "removed enum");
    if(!current)
        return;
    for(int i = 0; i < before->value_count(); ++i)
    {
        auto* value = before->value(i);
        auto* next = current->FindValueByName(value->name());
        require(next && next->number() == value->number(), value->full_name(), "removed or renumbered enum value");
    }
}

void checkMessage(const descriptor_pb::Descriptor* before, const descriptor_pb::DescriptorPool& after)
{
    auto* current = after.FindMessageTypeByName(before->full_name());
    require(current, before->full_name(), "removed message");
    if(current)
        for(int i = 0; i < before->field_count(); ++i)
        {
            auto* field = before->field(i);
            auto* next = current->FindFieldByName(field->name());
            require(next, field->full_name(), "removed field");
            if(!next)
                continue;
            require(next->number() == field->number(), field->full_name(), "renumbered field");
            require(next->type() == field->type() && next->is_map() == field->is_map(),
                    field->full_name(),
                    "changed field type");
            require(next->is_required() == field->is_required() && next->is_repeated() == field->is_repeated() &&
                            next->has_presence() == field->has_presence(),
                    field->full_name(),
                    "changed field label or presence");
            if(field->message_type())
                require(next->message_type() && next->message_type()->full_name() == field->message_type()->full_name(),
                        field->full_name(),
                        "changed message type");
            if(field->enum_type())
                require(next->enum_type() && next->enum_type()->full_name() == field->enum_type()->full_name(),
                        field->full_name(),
                        "changed enum type");
            const auto* oneof = field->containing_oneof();
            const auto* next_oneof = next->containing_oneof();
            require((!oneof && !next_oneof) || (oneof && next_oneof && oneof->name() == next_oneof->name()),
                    field->full_name(),
                    "changed oneof");
        }
    for(int i = 0; i < before->nested_type_count(); ++i) checkMessage(before->nested_type(i), after);
    for(int i = 0; i < before->enum_type_count(); ++i) checkEnum(before->enum_type(i), after);
}

void compatibility(const Schema& before, const Schema& after)
{
    for(auto* file: before.files)
    {
        if(file->package() != "chronolog.v1")
            continue;
        require(after.pool.FindFileByName(file->name()), file->name(), "removed file");
        for(int i = 0; i < file->message_type_count(); ++i) checkMessage(file->message_type(i), after.pool);
        for(int i = 0; i < file->enum_type_count(); ++i) checkEnum(file->enum_type(i), after.pool);
        for(int i = 0; i < file->service_count(); ++i)
        {
            auto* service = file->service(i);
            auto* current = after.pool.FindServiceByName(service->full_name());
            require(current, service->full_name(), "removed service");
            if(!current)
                continue;
            for(int j = 0; j < service->method_count(); ++j)
            {
                auto* method = service->method(j);
                auto* next = current->FindMethodByName(method->name());
                require(next, method->full_name(), "removed RPC");
                if(next)
                    require(next->input_type()->full_name() == method->input_type()->full_name() &&
                                    next->output_type()->full_name() == method->output_type()->full_name() &&
                                    next->client_streaming() == method->client_streaming() &&
                                    next->server_streaming() == method->server_streaming(),
                            method->full_name(),
                            "changed RPC signature");
            }
        }
    }
}

const std::regex pascal("[A-Z][a-z0-9]*([A-Z][a-z0-9]+)*");
const std::regex snake("[a-z][a-z0-9]*(_[a-z0-9]+)*");
const std::regex upper("[A-Z][A-Z0-9]*(_[A-Z0-9]+)*");
void name(std::string_view value, const std::regex& pattern, std::string_view full)
{
    require(std::regex_match(value.begin(), value.end(), pattern), full, "invalid name");
}
std::string prefix(std::string_view value)
{
    std::string out;
    for(size_t i = 0; i < value.size(); ++i)
    {
        const char c = value[i];
        if(c >= 'A' && c <= 'Z' && i &&
           ((value[i - 1] >= 'a' && value[i - 1] <= 'z') ||
            (i + 1 < value.size() && value[i + 1] >= 'a' && value[i + 1] <= 'z')))
            out += '_';
        out += c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
    }
    return out + '_';
}
void lintEnum(const descriptor_pb::EnumDescriptor* type)
{
    name(type->name(), pascal, type->full_name());
    const auto start = prefix(type->name());
    require(!type->options().allow_alias(), type->full_name(), "enum aliases are forbidden");
    require(type->value_count() && type->value(0)->number() == 0 && type->value(0)->name() == start + "UNSPECIFIED",
            type->full_name(),
            "zero value must be ENUM_UNSPECIFIED");
    for(int i = 0; i < type->value_count(); ++i)
    {
        auto* value = type->value(i);
        name(value->name(), upper, value->full_name());
        require(value->name().starts_with(start), value->full_name(), "enum value lacks prefix " + start);
    }
}
void lintMessage(const descriptor_pb::Descriptor* type, std::set<const descriptor_pb::FileDescriptor*>& used)
{
    if(type->options().map_entry())
        return;
    name(type->name(), pascal, type->full_name());
    for(int i = 0; i < type->field_count(); ++i)
    {
        auto* field = type->field(i);
        name(field->name(), snake, field->full_name());
        if(field->message_type())
            used.insert(field->message_type()->file());
        if(field->enum_type())
            used.insert(field->enum_type()->file());
    }
    for(int i = 0; i < type->real_oneof_decl_count(); ++i)
        name(type->oneof_decl(i)->name(), snake, type->oneof_decl(i)->full_name());
    for(int i = 0; i < type->nested_type_count(); ++i) lintMessage(type->nested_type(i), used);
    for(int i = 0; i < type->enum_type_count(); ++i) lintEnum(type->enum_type(i));
}
const std::regex version("v[1-9][0-9]*((alpha|beta)[1-9][0-9]*)?");
void lint(const Schema& schema)
{
    std::map<std::string, std::string> packages;
    std::map<std::string, std::string> rpc_types;
    for(auto* file: schema.files)
    {
        descriptor_pb::FileDescriptorProto definition;
        file->CopyTo(&definition);
        require(definition.syntax() == "proto3", file->name(), "syntax must be proto3");
        const std::string package(file->package());
        const auto last = package.substr(package.find_last_of('.') + 1);
        require(std::regex_match(last, version), file->name(), "package must end in a version suffix");
        std::string directory = package;
        for(auto& c: directory)
            if(c == '.')
                c = '/';
        const auto slash = file->name().find_last_of('/');
        const std::string dir(slash == std::string::npos ? std::string_view() : file->name().substr(0, slash));
        require(dir == directory, file->name(), "package directory must be " + directory);
        const auto [seen, fresh] = packages.emplace(dir, package);
        require(fresh || seen->second == package, file->name(), "files in one directory must share a package");
        const auto base = file->name().substr(slash + 1);
        require(base.ends_with(".proto"), file->name(), "file extension must be .proto");
        name(base.substr(0, base.size() - 6), snake, file->name());
        require(!file->public_dependency_count(), file->name(), "public imports are forbidden");
        require(!file->weak_dependency_count(), file->name(), "weak imports are forbidden");
        std::set<const descriptor_pb::FileDescriptor*> used;
        for(int i = 0; i < file->message_type_count(); ++i) lintMessage(file->message_type(i), used);
        for(int i = 0; i < file->enum_type_count(); ++i) lintEnum(file->enum_type(i));
        for(int i = 0; i < file->service_count(); ++i)
        {
            auto* service = file->service(i);
            name(service->name(), pascal, service->full_name());
            for(int j = 0; j < service->method_count(); ++j)
            {
                auto* method = service->method(j);
                name(method->name(), pascal, method->full_name());
                used.insert(method->input_type()->file());
                used.insert(method->output_type()->file());
                require(method->input_type()->name() == std::string(method->name()) + "Request",
                        method->full_name(),
                        "RPC request must be MethodRequest");
                require(method->output_type()->name() == std::string(method->name()) + "Response",
                        method->full_name(),
                        "RPC response must be MethodResponse");
                for(auto* type: {method->input_type(), method->output_type()})
                {
                    const auto [owner, unique] =
                            rpc_types.emplace(std::string(type->full_name()), std::string(method->full_name()));
                    require(unique,
                            method->full_name(),
                            "RPC request and response types must be unique; " + owner->first + " is also used by " +
                                    owner->second);
                }
            }
        }
        for(int i = 0; i < file->dependency_count(); ++i)
            require(used.contains(file->dependency(i)),
                    file->name(),
                    "unused import " + std::string(file->dependency(i)->name()));
    }
}
} // namespace
int main(int argc, char** argv)
{
    Schema before, current;
    if(argc == 4 && std::string(argv[1]) == "compat")
    {
        if(!before.load(argv[2]) || !current.load(argv[3]))
            return 2;
        compatibility(before, current);
    }
    else if(argc == 3 && std::string(argv[1]) == "lint")
    {
        if(!current.load(argv[2]))
            return 2;
        lint(current);
    }
    else
        return 2;
    std::cout << argv[1] << ": " << failures << " violations\n";
    return failures ? 1 : 0;
}
