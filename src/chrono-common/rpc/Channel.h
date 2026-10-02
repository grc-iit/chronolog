#pragma once

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace chronolog::rpc
{

// The one channel policy every inter-service channel uses (ARCHITECTURE.md M11.3). Each value has its
// reason next to it; no call site tunes any of them.

// A failed lookup, such as aardvark-dns answering late while a container joins the network, or a peer
// that returned on a new address, must not keep the channel dark: gRPC otherwise waits
// dns_min_time_between_resolutions (30 s) before it resolves again, far beyond every call deadline here.
inline constexpr int kDnsMinResolveIntervalMs = 1000;
// Bounded reconnect backoff: a restarted peer is reached within a second of coming back, and a dead one
// is probed often enough to notice it returning without hammering it.
inline constexpr int kInitialBackoffMs = 100;
inline constexpr int kMinBackoffMs = 100;
inline constexpr int kMaxBackoffMs = 1000;
// HTTP/2 keepalive even with no call in flight, so a peer that vanished without closing the socket (killed
// container, dropped route) is noticed in about two seconds and re-resolved instead of failing the next call.
inline constexpr int kKeepaliveTimeMs = 1000;
inline constexpr int kKeepaliveTimeoutMs = 1000;
// gRPC sends a bandwidth probe ping on its own and allows one ping in flight. If the path goes silent while that
// probe is out, the keepalive ping queues behind it and the only limit left is the transport's ping timeout, one
// minute by default, so the dead path would not be noticed for that long. The same bound as the keepalive timeout
// closes the transport on any unanswered ping. The argument has no public macro.
inline constexpr char kPingTimeoutArg[] = "grpc.http2.ping_timeout_ms";
// Servers accept those pings; below the client interval so no ping is a strike, and permitted when idle.
inline constexpr int kServerMinPingIntervalMs = 500;
// A Visor follower forwarding to the leader never waits longer than this, whatever the inbound deadline.
inline constexpr std::chrono::milliseconds kForwardCap{3000};
// A call that feeds a liveness timer gets this fraction of the shortest timer it feeds, so a late answer
// is detected, and one retry still fits, before the timer fires.
inline constexpr int kLivenessDeadlineDivisor = 2;
// Smallest liveness deadline that still covers a Visor leader hop and a loaded Keeper heartbeat.
inline constexpr std::chrono::milliseconds kMinLivenessDeadline{100};

// One endpoint, or a comma separated list of Visor replicas, as one gRPC target. A list becomes a
// single pick_first channel over the resolved IPv4 addresses, so a call or watch stream that loses
// the replica it was attached to reconnects to the next one (I9.1, section 12).
inline std::string visorTarget(const std::string& endpoints)
{
    std::vector<std::string> list;
    std::stringstream stream(endpoints);
    for(std::string item; std::getline(stream, item, ',');)
        if(!item.empty())
            list.push_back(item);
    if(list.size() < 2)
        return endpoints;
    std::string target = "ipv4:";
    bool resolved = false;
    for(const auto& item: list)
    {
        const auto colon = item.rfind(':');
        if(colon == std::string::npos)
            continue;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if(::getaddrinfo(item.substr(0, colon).c_str(), nullptr, &hints, &found) != 0 || !found)
            continue;
        char text[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr, text, sizeof(text));
        ::freeaddrinfo(found);
        target += (resolved ? "," : "") + std::string(text) + item.substr(colon);
        resolved = true;
    }
    return resolved ? target : list.front();
}

inline grpc::ChannelArguments channelArguments()
{
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_DNS_MIN_TIME_BETWEEN_RESOLUTIONS_MS, kDnsMinResolveIntervalMs);
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, kInitialBackoffMs);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, kMinBackoffMs);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, kMaxBackoffMs);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, kKeepaliveTimeMs);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, kKeepaliveTimeoutMs);
    args.SetInt(kPingTimeoutArg, kKeepaliveTimeoutMs);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
    return args;
}

// Without this a server answers the client keepalive above with GOAWAY too_many_pings.
inline void applyServerPolicy(grpc::ServerBuilder& builder)
{
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, kServerMinPingIntervalMs);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
}

// Waits for a connection instead of failing at once, up to the caller's deadline.
inline void withDeadline(grpc::ClientContext& context, std::chrono::system_clock::time_point deadline)
{
    context.set_deadline(deadline);
    context.set_wait_for_ready(true);
}

inline void withTimeout(grpc::ClientContext& context, std::chrono::milliseconds timeout)
{
    withDeadline(context, std::chrono::system_clock::now() + timeout);
}

// min(inbound deadline, forward cap) for a Visor forwarding to the leader. It fails fast instead of waiting
// for ready: the leader may have moved, and the caller's retry looks it up again.
inline void forwardingContext(grpc::ClientContext& context, std::chrono::system_clock::time_point inbound)
{
    context.set_deadline(std::min(inbound, std::chrono::system_clock::now() + kForwardCap));
}

// The deadline of a call that feeds liveness timers: shorter than every one of them (M11.3).
inline std::chrono::milliseconds livenessDeadline(std::initializer_list<std::chrono::milliseconds> timers)
{
    auto shortest = std::chrono::milliseconds::max();
    for(auto timer: timers) shortest = std::min(shortest, timer);
    return shortest / kLivenessDeadlineDivisor;
}

// One shared channel per peer target, created on first use, connected at once and reused by every call,
// so a lookup that stalls or a peer that moves is handled by the channel instead of by each caller.
class ChannelPool
{
public:
    static ChannelPool& peers()
    {
        static ChannelPool pool;
        return pool;
    }

    std::shared_ptr<grpc::Channel> get(const std::string& endpoints)
    {
        std::lock_guard lock(mutex_);
        auto& channel = channels_[endpoints];
        if(!channel)
        {
            channel = grpc::CreateCustomChannel(visorTarget(endpoints),
                                                grpc::InsecureChannelCredentials(),
                                                channelArguments());
            channel->GetState(true);
        }
        return channel;
    }

    size_t created() const
    {
        std::lock_guard lock(mutex_);
        return channels_.size();
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<grpc::Channel>> channels_;
};

inline std::shared_ptr<grpc::Channel> peerChannel(const std::string& endpoints)
{
    return ChannelPool::peers().get(endpoints);
}

} // namespace chronolog::rpc
