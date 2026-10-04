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

TEST_P(MembershipContract, NewStoryRouteCarriesTheCreationFloor)
{
    // keeper-b holds the highest C and keeper-a the highest CP, so the floor must take each maximum separately.
    const auto a = extend("keeper-a", 100, 900);
    const auto b = extend("keeper-b", 700, 100);
    ASSERT_GT(b.ceiling().physical_ns(), a.ceiling().physical_ns());
    ASSERT_GT(a.physical_ceiling_ns(), b.physical_ceiling_ns());
    const auto story = commit(storyProposal("floored"));
    ASSERT_TRUE(story.ok()) << story.status();
    const auto update = applied->membershipRouteUpdate(*story).value();
    EXPECT_EQ(update.ordering_cut().physical_ns(), b.ceiling().physical_ns());
    EXPECT_EQ(update.ordering_cut().logical(), b.ceiling().logical());
    EXPECT_EQ(update.physical_floor_ns(), a.physical_ceiling_ns());
    EXPECT_EQ(update.route().epoch(), 1u);
    ASSERT_EQ(update.route().keepers_size(), 2);
    EXPECT_EQ(std::set<std::string>(update.observe_floor().begin(), update.observe_floor().end()),
              (std::set<std::string>{"keeper-a", "keeper-b"}));
}

TEST_P(MembershipContract, CreateStoryRefusedWhileAKeeperHasNoCeiling)
{
    wire::MembershipCommand q;
    auto* p = q.mutable_register_()->mutable_process();
    p->set_process_id("keeper-c");
    p->set_instance("keeper-c");
    p->set_endpoint("keeper-c:50052");
    p->set_role(wire::PROCESS_ROLE_KEEPER);
    ASSERT_EQ(call<wire::RegisterResponse>(q).status().code(), 0);
    const auto refused = commit(storyProposal("refused"));
    EXPECT_EQ(refused.status().code(), absl::StatusCode::kFailedPrecondition);
    EXPECT_EQ(applied->listStories("c")->size(), 1u);
    extend("keeper-c", 100, 100);
    EXPECT_TRUE(commit(storyProposal("refused")).ok());
}

TEST_P(MembershipContract, CreationFloorIsComputedAtApplyNotProposal)
{
    const auto proposal = storyProposal("late");
    const auto raised = extend("keeper-b", 900, 900);
    ASSERT_GT(raised.ceiling().physical_ns(), 5'000'000'100);
    const auto story = commit(proposal);
    ASSERT_TRUE(story.ok()) << story.status();
    const auto update = applied->membershipRouteUpdate(*story).value();
    EXPECT_EQ(update.ordering_cut().physical_ns(), raised.ceiling().physical_ns());
    EXPECT_EQ(update.physical_floor_ns(), raised.physical_ceiling_ns());
}
