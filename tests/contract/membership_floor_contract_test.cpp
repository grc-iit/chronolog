TEST_P(MembershipContract, RemovedKeeperRemapsItsReleasedWritersWithAFloor)
{
    const auto writer = releasedWriter();
    remove(writer.assigned_keeper.process_id);
    const auto update = route();
    const auto next = catalog->acquire(1, "released");
    ASSERT_TRUE(next.ok()) << next.status();
    EXPECT_EQ(next->incarnation, writer.incarnation + 1);
    EXPECT_NE(next->assigned_keeper.process_id, writer.assigned_keeper.process_id);
    EXPECT_NE(std::find(update.observe_floor().begin(), update.observe_floor().end(), next->assigned_keeper.process_id),
              update.observe_floor().end());
}

TEST_P(MembershipContract, ReassignedReleasedWriterObservesOldFrontier)
{
    auto writer = catalog->acquire(1, "released").value();
    auto initial = visor::dynamic::routeState(route());
    auto old_clock = std::make_shared<FakeClock>(1000, 0);
    auto new_clock = std::make_shared<FakeClock>(100, 0);
    old_clock->setStatus(ClockStatus::Synced);
    new_clock->setStatus(ClockStatus::Synced);
    auto old_membership = std::make_shared<keeper::ConfigMembership>();
    auto new_membership = std::make_shared<keeper::ConfigMembership>();
    old_membership->setRouteState(1, initial);
    new_membership->setRouteState(1, initial);
    const auto survivor = initial.route.keepers[0].process_id == writer.assigned_keeper.process_id
                                  ? initial.route.keepers[1].process_id
                                  : initial.route.keepers[0].process_id;
    RamJournal old_journal(
            old_clock,
            old_membership,
            {.process_id = writer.assigned_keeper.process_id, .instance = writer.assigned_keeper.process_id});
    RamJournal new_journal(new_clock, new_membership, {.process_id = survivor, .instance = survivor});
    ASSERT_TRUE(old_journal.registerWriter(1, writer.writer_id, writer.incarnation).ok());
    AppendItem item;
    item.writer_id = writer.writer_id;
    item.incarnation = writer.incarnation;
    item.physical = {1000, 0, ClockStatus::Synced};
    Hlc last{};
    for(uint64_t sequence = 1; sequence <= 3; ++sequence)
    {
        item.sequence = sequence;
        auto result = old_journal.append({1, initial.route.epoch, {item}}, Durability::Accepted);
        ASSERT_TRUE(result.ok());
        ASSERT_TRUE(result->front().status.ok()) << result->front().status;
        last = std::max(last, result->front().hlc);
    }
    ASSERT_TRUE(catalog->release(1, writer.writer_id, writer.incarnation).ok());
    old_journal.releaseWriter(1, writer.writer_id, writer.incarnation);
    remove(writer.assigned_keeper.process_id);
    const auto update = route();
    const auto state = visor::dynamic::routeState(update);
    ASSERT_GT(state.ordering_cut, last);
    new_journal.enableDynamic(survivor);
    new_journal.applyRoute(1, initial, false, 0, [] {});
    new_journal.applyRoute(1,
                           state,
                           std::find(update.observe_floor().begin(), update.observe_floor().end(), survivor) !=
                                   update.observe_floor().end(),
                           update.revision(),
                           [&] { new_membership->setRouteState(1, state); });
    new_journal.extendCeiling({state.ordering_cut.physical_ns + 1000, 0}, state.physical_floor + 1000);
    auto next = catalog->acquire(1, "released");
    ASSERT_TRUE(next.ok());
    ASSERT_EQ(next->assigned_keeper.process_id, survivor);
    ASSERT_TRUE(new_journal.registerWriter(1, next->writer_id, next->incarnation).ok());
    item.incarnation = next->incarnation;
    item.sequence = 1;
    item.physical = {100, 0, ClockStatus::Synced};
    auto result = new_journal.append({1, state.route.epoch, {item}}, Durability::Accepted);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result->front().status.ok()) << result->front().status;
    EXPECT_GT(result->front().hlc, last);
    EXPECT_GT(result->front().hlc, state.ordering_cut);
}

TEST_P(MembershipContract, ReleasedBeforeEarlierTransitionGetsFloorAfterAbandonAndJoin)
{
    const auto writer = releasedWriter();
    join("keeper-c");
    ASSERT_EQ(route().observe_floor_size(), 1);
    EXPECT_EQ(route().observe_floor(0), "keeper-c");
    remove(writer.assigned_keeper.process_id, true);
    auto update = route();
    ASSERT_EQ(update.observe_floor_size(), 1);
    const auto& first_target = update.route().keepers(static_cast<int>(writer.writer_id % 2)).process_id();
    EXPECT_EQ(update.observe_floor(0), first_target);
    join("keeper-d");
    update = route();
    const auto& next_target = update.route().keepers(static_cast<int>(writer.writer_id % 3)).process_id();
    EXPECT_NE(std::find(update.observe_floor().begin(), update.observe_floor().end(), next_target),
              update.observe_floor().end());
    EXPECT_EQ(update.observe_floor_size(), next_target == "keeper-d" ? 1 : 2);
    auto next = catalog->acquire(1, "released");
    ASSERT_TRUE(next.ok()) << next.status();
    EXPECT_EQ(next->assigned_keeper.process_id, next_target);
    EXPECT_EQ(next->incarnation, writer.incarnation + 1);
}
