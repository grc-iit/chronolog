#ifndef CHRONOLOG_RPC_REPLY_H
#define CHRONOLOG_RPC_REPLY_H
#include <thallium.hpp>
#include <utility>
namespace chronolog::rpc
{
// Only sending/serialization is contained. The operation has already produced
// its result; a departed caller cannot roll it back or receive a false ACK.
// The native handler and its actual owner retain request/provider/data lifetime.
template <class... Reply>
bool respond(thallium::request const& request, Reply&&... reply) noexcept
{
    try
    {
        request.respond(std::forward<Reply>(reply)...);
        return true;
    }
    catch(...)
    {
        return false;
    }
}
} // namespace chronolog::rpc
#endif
