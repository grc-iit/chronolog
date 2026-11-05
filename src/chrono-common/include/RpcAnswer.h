#ifndef CHRONOLOG_RPC_ANSWER_H
#define CHRONOLOG_RPC_ANSWER_H

#include <thallium.hpp>

#include <chrono_monitor.h>

namespace chronolog
{

// Answers the caller of an RPC handler. A thallium error from respond (the
// caller died, gave up on the handle, or the network timed it out) is logged
// and swallowed: thrown out of the handler it would end the process. Returns
// whether the answer was sent. A template over the request so a test can pass
// one whose respond throws; the handlers pass a tl::request.
template <typename Request, typename Value>
bool answerRpcCaller(Request const& request, Value const& value, char const* service)
{
    try
    {
        request.respond(value);
        return true;
    }
    catch(thallium::exception const& ex)
    {
        LOG_WARNING("[{}] Could not answer the caller: {}", service, ex.what());
        return false;
    }
}

} // namespace chronolog

#endif
