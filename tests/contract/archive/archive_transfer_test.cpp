TEST(ArchiveTransferTest, PartialStreamYieldsNoReceipt)
{
    Server server;
    auto frame = Frame();
    frame.set_data(frame.data().substr(0, frame.data().size() / 2));
    frame.set_final(false);
    auto [status, receipt] = Send(server, {frame});
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(receipt.receipt(), 0u);
    EXPECT_TRUE(server.store->manifest(1)->empty());
}

TEST(ArchiveTransferTest, ChecksumMismatchYieldsNoReceipt)
{
    Server server;
    auto frame = Frame();
    auto checksum = frame.checksum();
    checksum[0] ^= 1;
    frame.set_checksum(checksum);
    auto [status, receipt] = Send(server, {frame});
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(receipt.receipt(), 0u);
    EXPECT_TRUE(server.store->manifest(1)->empty());
}
