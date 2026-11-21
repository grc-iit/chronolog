#ifndef CHRONOLOG_NATIVE_THREAD_LABEL_H
#define CHRONOLOG_NATIVE_THREAD_LABEL_H
#include <abt.h>
#include <string>
namespace chronolog::rpc
{
// Diagnostics must not require a native ULT. The fixed lifecycle workers are
// ordinary threads; the actual ABI primary remains the existing process owner.
inline std::string nativeThreadLabel()
{
    ABT_unit_id id = 0;
    return ABT_thread_self_id(&id) == ABT_SUCCESS ? std::to_string(id) : "external";
}
} // namespace chronolog::rpc
#endif
