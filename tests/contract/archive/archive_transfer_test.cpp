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

TEST(ArchiveTransferTest, TombstonedStoryRefusesLateChunks)
{
    Server server;
    auto [status, receipt] = Send(server, {Frame()});
    ASSERT_TRUE(status.ok());
    server.service->tombstone(1);
    ASSERT_TRUE(server.service->waitDestroyed(1, std::chrono::seconds(5)));
    for(int attempt = 0; attempt < 2; ++attempt)
    {
        auto [late_status, late] = Send(server, {Frame()});
        EXPECT_EQ(late_status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
        EXPECT_EQ(late.receipt(), 0u);
    }
    const auto records = server.store->manifest(1);
    ASSERT_TRUE(records.ok());
    for(const auto& record: *records) EXPECT_NE(record.state, ManifestState::Published) << record.file;
    size_t files = 0;
    for(const auto& entry: std::filesystem::directory_iterator(server.root / "1")) files += entry.is_regular_file();
    EXPECT_EQ(files, 0u) << "the refused chunk leaves no file behind and the destroyed one is erased";
    auto [other_status, other] = Send(server, {Frame(2)});
    ASSERT_TRUE(other_status.ok());
    EXPECT_EQ(other.receipt(), 2u);
}

TEST(ArchiveTransferTest, TombstoneWaitsForInFlightPublish)
{
    auto codec = std::make_shared<FailingCodec>();
    Server server(codec);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    wire::WatchWatermarksRequest subscription;
    subscription.set_keeper_id("keeper-1");
    subscription.add_story_ids(1);
    auto watch = server.stub->WatchWatermarks(&context, subscription);
    wire::WatchWatermarksResponse report;
    ASSERT_TRUE(watch->Read(&report));
    codec->closeGate();
    std::pair<grpc::Status, wire::TransferChunkResponse> sent;
    std::thread sender([&] { sent = Send(server, {Frame()}); });
    ASSERT_TRUE(codec->waitEntered());
    server.service->tombstone(1);
    bool dropped_while_pending = false;
    while(!dropped_while_pending && watch->Read(&report))
        if(report.dropped())
            dropped_while_pending = report.pending_receipts_size() == 1 && report.pending_receipts(0) == 1;
    EXPECT_TRUE(dropped_while_pending);
    codec->release();
    sender.join();
    ASSERT_TRUE(sent.first.ok()) << sent.first.error_message();
    EXPECT_EQ(sent.second.receipt(), 1u);
    ASSERT_TRUE(server.service->waitDestroyed(1, std::chrono::seconds(5)));
    const auto records = server.store->manifest(1);
    ASSERT_TRUE(records.ok());
    ASSERT_EQ(records->size(), 1u);
    EXPECT_EQ(records->front().state, ManifestState::Deleted)
            << "the in-flight chunk landed and was erased with the rest";
    EXPECT_FALSE(std::filesystem::exists(server.root / records->front().file));
    context.TryCancel();
}

TEST(ArchiveTransferTest, RestartResumesDestroyFromManifest)
{
    Server server;
    for(const StoryId story: {StoryId{1}, StoryId{2}})
    {
        auto [status, receipt] = Send(server, {Frame(story)});
        ASSERT_TRUE(status.ok());
    }
    const auto doomed = server.store->manifest(1)->front().file;
    const auto spared = server.store->manifest(2)->front().file;
    // The Grapher recorded the tombstone and went down before it erased anything.
    ASSERT_TRUE(server.store->tombstone(1).ok());
    server.service->shutdown();
    server.store.reset();
    ASSERT_TRUE(std::filesystem::exists(server.root / doomed));
    auto reopened =
            FileTierStore::Open(server.root, "test-writer", {{1, {100, 0}}}, std::make_shared<HDF5ChunkCodec>());
    ASSERT_TRUE(reopened.ok()) << reopened.status();
    ArchiveService restarted(**reopened, "restarted-instance");
    ASSERT_TRUE(restarted.waitDestroyed(1, std::chrono::seconds(5)));
    EXPECT_FALSE(std::filesystem::exists(server.root / doomed));
    EXPECT_TRUE(std::filesystem::exists(server.root / spared)) << "only the tombstoned story is erased";
    const auto records = (*reopened)->manifest(1);
    ASSERT_TRUE(records.ok());
    ASSERT_EQ(records->size(), 1u);
    EXPECT_EQ(records->front().state, ManifestState::Deleted);
    EXPECT_EQ((*reopened)->manifest(2)->front().state, ManifestState::Published);
    EXPECT_EQ((*reopened)->contiguousWatermark(1).value(), (Hlc{200, 0})) << "erasing never lowers W (I13.5)";
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&restarted);
    auto endpoint = builder.BuildAndStart();
    ASSERT_NE(endpoint, nullptr);
    auto stub = wire::Archive::NewStub(
            grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
    wire::TransferChunkResponse response;
    auto stream = stub->TransferChunk(&context, &response);
    ASSERT_TRUE(stream->Write(Frame()));
    stream->WritesDone();
    EXPECT_EQ(stream->Finish().error_code(), grpc::StatusCode::FAILED_PRECONDITION)
            << "the refusal comes from the manifest, so it survives the restart";
    restarted.shutdown();
    endpoint->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    reopened->reset();
}
