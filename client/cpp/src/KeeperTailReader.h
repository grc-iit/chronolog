#ifndef KEEPER_TAIL_READER_H
#define KEEPER_TAIL_READER_H
#include <cstddef>
#include <memory>
#include <vector>
#include <RpcRuntime.h>
#include <chronolog_client.h>
namespace chronolog
{
class KeeperRecordingClient;
// One shared deadline covers lookup and both fanout phases. The coordinator
// occupies no worker; every keeper requires one reserved slot before dispatch.
// The current owner has two slots. Larger/occupied rosters fail with capacity
// before any lookup or forward. Missing sources and payloads report partial.
// Retention owns the actual context, entry/native lease and policy snapshot.
int gather_story_tail(std::vector<KeeperRecordingClient*> const&,
                      StoryId const&,
                      size_t,
                      std::vector<Event>&,
                      rpc::RpcDeadline const&,
                      rpc::RpcRuntime&,
                      std::shared_ptr<void> retention,
                      uint64_t attempt_ms);
} // namespace chronolog
#endif
