#pragma once
#include <libnuraft/nuraft.hxx>
#include <libnuraft/launcher.hxx>
#include "visor/raft/DurableState.h"
#include "visor/raft/RaftTestControl.h"
#include "visor/catalog/SqliteMetadataStore.h"
#include "visor/catalog/LeaseAuthority.h"
#include "chronolog/internal/v1/internal.pb.h"
namespace chronolog::visor
{
class RaftMetadataStore final
    : public MetadataStore
    , public AcquisitionLedger
{
public:
    static absl::StatusOr<std::unique_ptr<RaftMetadataStore>> open(const std::string& path,
                                                                   Topology topology,
                                                                   RaftConfig config,
                                                                   FenceWaiter fence_waiter = nullptr,
                                                                   AcquisitionLeaseConfig leases = {},
                                                                   std::shared_ptr<RaftTestControl> control = {});
    ~RaftMetadataStore() override;
    using MetadataStore::createChronicle;
    using MetadataStore::createStory;
    absl::StatusOr<Chronicle> createChronicle(std::string name, Properties properties) override;
    absl::StatusOr<Chronicle> getChronicle(std::string name) const override;
    absl::StatusOr<std::vector<Chronicle>> listChronicles() const override;
    absl::Status destroyChronicle(std::string name) override;
    absl::StatusOr<Story> createStory(std::string chronicle, std::string name, Properties properties) override;
    absl::StatusOr<Story> getStory(StoryId id) const override;
    absl::StatusOr<std::vector<Story>> listStories(std::string chronicle) const override;
    absl::StatusOr<StoriesByPrefix> listStoriesByPrefix(std::string prefix, uint32_t limit) const override;
    absl::Status destroyStory(StoryId id) override;
    absl::StatusOr<Acquisition> acquire(StoryId id, std::string writer_identity) override;
    absl::StatusOr<Acquisition> acquire(StoryId id, std::string writer_identity, AcquireOptions options) override;
    absl::StatusOr<std::vector<RenewAcquisitionResult>>
    renewAcquisitions(const std::vector<RenewAcquisition>& acquisitions) override;
    LeaseAuthority& leaseAuthority() { return leases_; }
    // Leader-local Keeper heartbeat evidence; never proposed. Returns the number of tuples renewed.
    absl::StatusOr<size_t> acceptKeeperEvidence(const std::string& keeper, const std::vector<RenewAcquisition>& tuples);

    absl::StatusOr<ReleaseResult> release(StoryId id, uint64_t writer_id, uint64_t incarnation) override;
    absl::StatusOr<Epoch> compareAndSetEpoch(StoryId id, Epoch expected, Epoch desired) override;
    absl::StatusOr<AcquisitionSnapshot> snapshotAcquisitions() const override;
    void setObserver(AcquisitionObserver* observer) override;
    absl::StatusOr<Acquisition> requestGrant(const std::string& request_id) const override;
    bool leaderLease() const;
    bool appliedStateCurrent() const;
    int leaderId() const;
    bool isLocalLeader() const { return leaderId() == config_.server_id; }
    std::string leaderEndpoint(bool internal) const;
    absl::StatusOr<std::string> propose(const internal::v1::CatalogCommand& command);
    uint64_t term() const { return server_->get_term(); }
    std::vector<std::string> replicaEndpoints() const
    {
        std::vector<std::string> out;
        for(const auto& peer: config_.peers) out.push_back(peer.internal_endpoint);
        return out;
    }
    // Reconciliation, one bounded expiry proposal, then a completed service tick.
    absl::Status serviceTick();
    absl::Status reconcileLeases();
    // Qualified leader only: select one bounded due batch and propose ExpireAcquisitions.
    absl::Status sweepExpiry();
    SqliteMetadataStore& appliedStore() { return *store_; }

private:
    class Machine;
    RaftMetadataStore(std::unique_ptr<SqliteMetadataStore> store,
                      RaftConfig config,
                      FenceWaiter waiter,
                      AcquisitionLeaseConfig leases,
                      std::shared_ptr<RaftTestControl> control);
    LeaseAuthority leases_;
    std::shared_ptr<RaftTestControl> control_;
    std::mutex activation_mutex_;
    uint64_t active_term_{};
    std::pair<StoryId, uint64_t> scan_cursor_{};
    absl::Status activateLeases();
    std::unique_ptr<SqliteMetadataStore> store_;
    RaftConfig config_;
    FenceWaiter fence_waiter_;
    nuraft::ptr<DurableState> durable_;
    nuraft::ptr<Machine> machine_;
    nuraft::raft_launcher launcher_;
    nuraft::ptr<nuraft::raft_server> server_;
};
} // namespace chronolog::visor
