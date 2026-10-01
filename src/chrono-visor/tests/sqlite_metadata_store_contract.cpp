// Instantiates MetadataStoreContract for the SQLite store and the in-memory double.
// The suite source is included, not linked, as the contract suite requires.
#include <atomic>
#include <memory>

#include "TestSupport.h"
#include "catalog/InMemoryMetadataStore.h"
#include "catalog/SqliteMetadataStore.h"
#include "metadata_store_contract_test.cpp"

namespace chronolog::contract
{
namespace
{

using visor::InMemoryMetadataStore;
using visor::SqliteMetadataStore;
using visor::testing::TempDir;
using visor::testing::twoKeeperTopology;

MetadataStoreFactory sqliteFactory()
{
    return [] {
        auto dir = std::make_shared<TempDir>();
        // Delivery of the Keeper's fence confirmation; the fence wait itself is instant.
        auto confirm = std::make_shared<std::atomic<bool>>(true);
        auto open = [dir, confirm]() -> std::unique_ptr<MetadataStore> {
            auto store = SqliteMetadataStore::open((dir->path() / "catalog.sqlite").string(), twoKeeperTopology(),
                                                   [confirm](const std::string&, uint64_t) { return confirm->load(); });
            if(!store.ok())
            {
                ADD_FAILURE() << store.status();
                return nullptr;
            }
            return std::move(*store);
        };
        auto harness = std::make_unique<MetadataStoreHarness>();
        harness->sut = open();
        harness->confirmReleaseFence = [confirm](bool value) { confirm->store(value); };
        MetadataStoreHarness* raw = harness.get();
        harness->restart = [raw, open] {
            raw->sut.reset();
            raw->sut = open();
        };
        return harness;
    };
}

MetadataStoreFactory inMemoryFactory()
{
    return [] {
        auto confirm = std::make_shared<std::atomic<bool>>(true);
        auto harness = std::make_unique<MetadataStoreHarness>();
        harness->sut = std::make_unique<InMemoryMetadataStore>(
                twoKeeperTopology(), [confirm](const std::string&, uint64_t) { return confirm->load(); });
        harness->confirmReleaseFence = [confirm](bool value) { confirm->store(value); };
        // The double has no durable state, so a restart keeps the live instance.
        harness->restart = [] {};
        return harness;
    };
}

std::string paramName(const ::testing::TestParamInfo<MetadataStoreFactory>&)
{
    return "Default";
}

INSTANTIATE_TEST_SUITE_P(Sqlite, MetadataStoreContract, ::testing::Values(sqliteFactory()), paramName);
INSTANTIATE_TEST_SUITE_P(InMemory, MetadataStoreContract, ::testing::Values(inMemoryFactory()), paramName);

} // namespace
} // namespace chronolog::contract
