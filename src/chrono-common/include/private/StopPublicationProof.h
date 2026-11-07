#ifndef CHRONOLOG_STOP_PUBLICATION_PROOF_H
#define CHRONOLOG_STOP_PUBLICATION_PROOF_H
#include <string>
#include <utility>
namespace chronolog::rpc
{
class ManagedStopBarrier;
struct StopPublicationTestAccess;
// Only the managed-status validator can authenticate a ready target. This proof
// closes future sends; it cannot settle a native call already admitted.
class PublisherParkProof
{
    friend class ManagedStopBarrier;
    friend struct StopPublicationTestAccess;
    std::string target_, identity_;
    PublisherParkProof(std::string target, std::string identity)
        : target_(std::move(target))
        , identity_(std::move(identity))
    {}

public:
    PublisherParkProof() = default;
    bool valid() const noexcept { return !target_.empty() && !identity_.empty(); }
    std::string const& target() const noexcept { return target_; }
    std::string const& identity() const noexcept { return identity_; }
};
} // namespace chronolog::rpc
#endif
