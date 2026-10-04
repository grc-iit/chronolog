#pragma once

#include <chrono>
#include <functional>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "common/worker/WorkerPool.h"

namespace chronolog::visor
{

// Queues a unary call's work; a call already cancelled or past its deadline when a worker reaches it is answered
// CANCELLED or DEADLINE_EXCEEDED without running.
inline bool SubmitCall(WorkerPool& pool,
                       grpc::CallbackServerContext* context,
                       grpc::ServerUnaryReactor* reactor,
                       std::function<void()> task)
{
    return pool.submit(
            std::move(task),
            [context] { return context->IsCancelled() || std::chrono::system_clock::now() >= context->deadline(); },
            [context, reactor]
            {
                reactor->Finish(context->IsCancelled() ? grpc::Status::CANCELLED
                                                       : grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                                                      "deadline passed while queued"));
            });
}

} // namespace chronolog::visor
