#ifndef CHRONOLOG_RPC_BULK_SERIALIZATION_H
#define CHRONOLOG_RPC_BULK_SERIALIZATION_H
#include <cereal/archives/binary.hpp>
#include <streambuf>
#include <istream>
#include <ostream>
#include <string>
namespace chronolog::rpc
{
class BulkOutputBuffer: public std::streambuf
{
    std::string& bytes_;

public:
    explicit BulkOutputBuffer(std::string& bytes)
        : bytes_(bytes)
    {}

protected:
    std::streamsize xsputn(char const* bytes, std::streamsize size) override
    {
        bytes_.append(bytes, static_cast<size_t>(size));
        return size;
    }
    int_type overflow(int_type c) override
    {
        if(!traits_type::eq_int_type(c, traits_type::eof()))
            bytes_.push_back(traits_type::to_char_type(c));
        return traits_type::not_eof(c);
    }
};
class BulkInputBuffer: public std::streambuf
{
public:
    BulkInputBuffer(char const* bytes, size_t size)
    {
        auto begin = const_cast<char*>(bytes);
        setg(begin, begin, begin + size);
    }
};
template <class Value>
std::string serializeBulk(Value const& value)
{
    std::string bytes;
    BulkOutputBuffer buffer(bytes);
    std::ostream stream(&buffer);
    cereal::BinaryOutputArchive archive(stream);
    archive(value);
    return bytes;
}
template <class Value>
bool deserializeBulk(char const* bytes, size_t size, Value& value) noexcept
{
    try
    {
        BulkInputBuffer buffer(bytes, size);
        std::istream stream(&buffer);
        cereal::BinaryInputArchive archive(stream);
        archive(value);
        return true;
    }
    catch(...)
    {
        return false;
    }
}
} // namespace chronolog::rpc
#endif
