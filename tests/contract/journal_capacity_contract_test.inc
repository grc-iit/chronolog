// Include after journal_contract_test.cpp when the harness supports capacity control.
namespace chronolog::contract
{
TEST_P(JournalContract, CapacityRefusalIsDefiniteAndKeepsDuplicateResults)
{
    ASSERT_TRUE(h->forceCapacity);
    const auto durability = h->supports_durable ? Durability::Durable : Durability::Accepted;
    auto admitted = h->sut->append(Batch({Item()}), durability);
    ASSERT_TRUE(admitted.ok()) << admitted.status();
    ASSERT_EQ(admitted->size(), 1u);
    ASSERT_TRUE(admitted->front().status.ok());
    const auto original = admitted->front();
    h->forceCapacity(true);
    auto refused = h->sut->append(Batch({Item(2), Item(), Item(3)}), durability);
    ASSERT_TRUE(refused.ok()) << refused.status();
    ASSERT_EQ(refused->size(), 3u);
    for(size_t index: {0u, 2u})
    {
        EXPECT_EQ(refused->at(index).status.code(), absl::StatusCode::kResourceExhausted);
        EXPECT_EQ(refused->at(index).rejection, AppendRejection::Capacity);
        EXPECT_EQ(refused->at(index).achieved, Durability::Unspecified);
    }
    const auto& duplicate = refused->at(1);
    EXPECT_EQ(duplicate.status, original.status);
    EXPECT_EQ(duplicate.rejection, original.rejection);
    EXPECT_EQ(duplicate.id, original.id);
    EXPECT_EQ(duplicate.hlc, original.hlc);
    EXPECT_EQ(duplicate.achieved, original.achieved);
    EXPECT_EQ(eventCount(), 1u);
    h->forceCapacity(false);
    auto resumed = h->sut->append(Batch({Item(2), Item(3)}), durability);
    ASSERT_TRUE(resumed.ok()) << resumed.status();
    ASSERT_EQ(resumed->size(), 2u);
    for(size_t index: {0u, 1u})
    {
        EXPECT_TRUE(resumed->at(index).status.ok()) << resumed->at(index).status;
        EXPECT_EQ(resumed->at(index).rejection, AppendRejection::Unspecified);
        EXPECT_EQ(resumed->at(index).id.sequence, index + 2);
    }
    EXPECT_EQ(eventCount(), 3u);
}
} // namespace chronolog::contract
