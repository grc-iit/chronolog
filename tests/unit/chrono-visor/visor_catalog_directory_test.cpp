// The visor's directory writes its chronicles and stories through a
// VisorCatalogStore before it changes memory, and is rebuilt from the store's
// rows at startup (visor persistence, PR 1). The graphers, keepers and player
// are the fake peers of admin_test_fixture.h.
#include "admin_test_fixture.h"

#include <algorithm>
#include <functional>
#include <future>
#include <string>
#include <vector>

#include <InMemoryCatalogStore.h>
#include <VisorCatalogStore.h>

namespace
{
// An InMemoryCatalogStore that logs each change, can be told to fail the next
// one, and can hold a load until released.
class RecordingCatalogStore: public VisorCatalogStore
{
public:
    InMemoryCatalogStore inner;
    std::atomic<bool> failNext{false};
    std::atomic<bool> holdLoad{false};
    std::promise<void> loadEntered;
    std::promise<void> loadReleased;
    // Runs on each change before it is applied, with its log entry.
    std::function<void(std::string const&)> onChange;

    int open() override { return inner.open(); }
    int load(CatalogSnapshot& out) override
    {
        if(holdLoad.load())
        {
            loadEntered.set_value();
            loadReleased.get_future().wait();
        }
        return inner.load(out);
    }
    int insertChronicle(ChronicleId id, std::string const& name) override
    {
        return change("insertChronicle " + name, [&] { return inner.insertChronicle(id, name); });
    }
    int insertStory(StoryId id, ChronicleId cid, std::string const& chronicle, std::string const& story) override
    {
        return change("insertStory " + chronicle + "/" + story,
                      [&] { return inner.insertStory(id, cid, chronicle, story); });
    }
    int deleteStory(StoryId id) override
    {
        return change("deleteStory " + std::to_string(id), [&] { return inner.deleteStory(id); });
    }
    int deleteChronicle(ChronicleId id) override
    {
        return change("deleteChronicle " + std::to_string(id), [&] { return inner.deleteChronicle(id); });
    }
    int setStoryDestroyPending(StoryId id, bool pending) override
    {
        return change("setStoryDestroyPending " + std::to_string(id) + " " + std::to_string(pending),
                      [&] { return inner.setStoryDestroyPending(id, pending); });
    }
    int setChronicleDestroyPending(ChronicleId id, bool pending) override
    {
        return change("setChronicleDestroyPending " + std::to_string(id) + " " + std::to_string(pending),
                      [&] { return inner.setChronicleDestroyPending(id, pending); });
    }
    std::vector<std::string> log()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return log_;
    }
    CatalogSnapshot rows()
    {
        CatalogSnapshot snapshot;
        inner.load(snapshot);
        return snapshot;
    }

private:
    int change(std::string const& entry, std::function<int()> const& apply)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            log_.push_back(entry);
        }
        if(onChange)
            onChange(entry);
        if(failNext.exchange(false))
            return CL_ERR_UNKNOWN;
        return apply();
    }
    std::mutex mutex_;
    std::vector<std::string> log_;
};

ChronicleId chronicleId(std::string const& name) { return catalogChronicleIdOf(name); }

CatalogChronicle const* findChronicle(CatalogSnapshot const& rows, std::string const& name)
{
    for(auto const& c: rows.chronicles)
        if(c.name == name)
            return &c;
    return nullptr;
}

CatalogStory const* findStory(CatalogSnapshot const& rows, std::string const& chronicle, std::string const& story)
{
    for(auto const& s: rows.stories)
        if(s.chronicle == chronicle && s.story == story)
            return &s;
    return nullptr;
}

std::size_t indexOf(std::vector<std::string> const& log, std::string const& entry)
{
    return std::find(log.begin(), log.end(), entry) - log.begin();
}

std::size_t count(std::vector<std::string> const& log, std::string const& prefix)
{
    return std::count_if(log.begin(), log.end(), [&](std::string const& e) { return e.rfind(prefix, 0) == 0; });
}
} // namespace

class VisorCatalog: public AdminDeadline
{
protected:
    std::shared_ptr<RecordingCatalogStore> store = std::make_shared<RecordingCatalogStore>();
    std::vector<std::string> names(VisorClientPortal& portal)
    {
        std::vector<std::string> out;
        EXPECT_EQ(portal.ShowChronicles(100, out), CL_SUCCESS);
        std::sort(out.begin(), out.end());
        return out;
    }
    std::vector<std::string> stories(VisorClientPortal& portal, std::string const& chronicle)
    {
        std::vector<std::string> out;
        portal.ShowStories(100, chronicle, out);
        std::sort(out.begin(), out.end());
        return out;
    }
    void load(VisorClientPortal& portal)
    {
        CatalogSnapshot loaded;
        ASSERT_EQ(portal.LoadCatalog(loaded), CL_SUCCESS);
        registry->restoreUnconfirmedDestructions(loaded);
    }
};

// The row is in the store before the chronicle is in the directory.
TEST_P(VisorCatalog, CreateChronicleCommitsBeforeMemory)
{
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);
    bool inMemoryAtCommit = true;
    store->onChange = [&](std::string const& entry)
    {
        if(entry == "insertChronicle fresh")
            inMemoryAtCommit = portal.core()->directory.getChronicleMap()->count(chronicleId("fresh")) != 0;
    };
    EXPECT_EQ(portal.CreateChronicle(100, "fresh"), CL_SUCCESS);
    EXPECT_FALSE(inMemoryAtCommit);
    EXPECT_NE(findChronicle(store->rows(), "fresh"), nullptr);
    EXPECT_EQ(names(portal), (std::vector<std::string>{"chronicle", "fresh"}));
}

// A create, a first acquire and a destroy whose commit fails each answer
// CL_ERR_UNKNOWN, and the directory, the store and the components are as they
// were: no start or destroy is sent.
TEST_P(VisorCatalog, FailedCommitLeavesDirectoryUnchanged)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);

    store->failNext = true;
    EXPECT_EQ(portal.CreateChronicle(100, "fresh"), CL_ERR_UNKNOWN);
    EXPECT_EQ(names(portal), (std::vector<std::string>{"chronicle"}));
    EXPECT_EQ(findChronicle(store->rows(), "fresh"), nullptr);

    store->failNext = true;
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_UNKNOWN);
    for(auto const& c: starts) EXPECT_EQ(c.load(), 0);
    EXPECT_TRUE(stories(portal, "chronicle").empty());
    EXPECT_TRUE(store->rows().stories.empty());
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_ERR_NOT_EXIST);

    ASSERT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    ASSERT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
    store->failNext = true;
    EXPECT_EQ(portal.DestroyStory(100, "chronicle", "story"), CL_ERR_UNKNOWN);
    EXPECT_EQ(storyDestroys[0].load(), 0);
    EXPECT_EQ(stories(portal, "chronicle"), (std::vector<std::string>{"story"}));
    auto const rows = store->rows();
    auto row = findStory(rows, "chronicle", "story");
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->destroyPending);

    store->failNext = true;
    EXPECT_EQ(portal.DestroyChronicle(100, "chronicle"), CL_ERR_UNKNOWN);
    EXPECT_EQ(chronicleDestroys[0].load(), 0);
    EXPECT_EQ(names(portal), (std::vector<std::string>{"chronicle"}));
    ASSERT_NE(findChronicle(store->rows(), "chronicle"), nullptr);
    EXPECT_FALSE(findChronicle(store->rows(), "chronicle")->destroyPending);

    // Nothing is left pending: the next destroy goes through once.
    EXPECT_EQ(portal.DestroyStory(100, "chronicle", "story"), CL_SUCCESS);
    EXPECT_EQ(storyDestroys[0].load(), 1);
    EXPECT_EQ(findStory(store->rows(), "chronicle", "story"), nullptr);
}

// The caller holds the story when the destroy's flag write fails: the answer
// is CL_ERR_UNKNOWN and nothing goes to the graphers, but the hold is already
// released and its stop sent, as when the destroy answers -22 or -24. The row
// stays unflagged and the name still works.
TEST_P(VisorCatalog, FailedFlagWriteOfAHeldStoryReleasesTheHold)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);
    store->onChange = [&](std::string const& entry)
    {
        if(entry.rfind("set", 0) == 0)
            store->failNext = true;
    };
    auto stopsSent = [&]
    {
        int total = 0;
        for(auto const& c: stops) total += c.load();
        return total;
    };

    ASSERT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    auto stopsBefore = stopsSent();
    EXPECT_EQ(portal.DestroyStory(100, "chronicle", "story"), CL_ERR_UNKNOWN);
    EXPECT_GT(stopsSent(), stopsBefore);
    EXPECT_EQ(storyDestroys[0].load(), 0);
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_ERR_NOT_ACQUIRED);
    auto const rows = store->rows();
    auto row = findStory(rows, "chronicle", "story");
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->destroyPending);
    EXPECT_EQ(stories(portal, "chronicle"), (std::vector<std::string>{"story"}));

    ASSERT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    stopsBefore = stopsSent();
    EXPECT_EQ(portal.DestroyChronicle(100, "chronicle"), CL_ERR_UNKNOWN);
    EXPECT_GT(stopsSent(), stopsBefore);
    EXPECT_EQ(chronicleDestroys[0].load(), 0);
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_ERR_NOT_ACQUIRED);
    ASSERT_NE(findChronicle(store->rows(), "chronicle"), nullptr);
    EXPECT_FALSE(findChronicle(store->rows(), "chronicle")->destroyPending);
    ASSERT_NE(findStory(store->rows(), "chronicle", "story"), nullptr);
    EXPECT_FALSE(findStory(store->rows(), "chronicle", "story")->destroyPending);

    store->onChange = nullptr;
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(storyDestroys[0].load(), 0);
    EXPECT_EQ(chronicleDestroys[0].load(), 0);
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
}

TEST_P(VisorCatalog, FirstAcquireInsertsStoryLaterAcquireWritesNothing)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);
    ClientId second = 200;
    uint64_t offset = 0;
    ASSERT_EQ(portal.ClientConnect(0, second, offset), CL_SUCCESS);
    auto before = store->log().size();
    ASSERT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(store->log().size(), before + 1);
    EXPECT_EQ(store->log().back(), "insertStory chronicle/story");
    EXPECT_NE(findStory(store->rows(), "chronicle", "story"), nullptr);
    // A second holder, the holder's repeated acquire, a release and an
    // acquire after it write nothing.
    EXPECT_EQ(portal.AcquireStory(200, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
    EXPECT_EQ(portal.ReleaseStory(200, "chronicle", "story"), CL_SUCCESS);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(store->log().size(), before + 1);
}

// The acquire is refused before any start is sent (no free slot for them), so
// its acquisition is rolled back; the story it created stays, in the
// directory and in the store, as after a release.
TEST_P(VisorCatalog, StoryRowStaysWhenStartNotificationFails)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);
    auto tokens = occupy(4, rpc::RpcDeadline::after(120ms));
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_ERR_RPC_CAPACITY);
    for(auto const& c: starts) EXPECT_EQ(c.load(), 0);
    tokens.clear();
    std::this_thread::sleep_for(10ms);
    EXPECT_NE(findStory(store->rows(), "chronicle", "story"), nullptr);
    EXPECT_EQ(stories(portal, "chronicle"), (std::vector<std::string>{"story"}));
    EXPECT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_ERR_NOT_ACQUIRED);
    // The next acquire finds the story and writes nothing.
    auto before = store->log().size();
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(store->log().size(), before);
}

// The rows of an earlier run come back: they are listed, and a client acquires
// an old story without creating its chronicle.
TEST_P(VisorCatalog, DirectoryRebuiltFromSnapshot)
{
    delays[1] = delays[2] = 0;
    ASSERT_EQ(store->inner.insertChronicle(chronicleId("old"), "old"), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertChronicle(chronicleId("empty"), "empty"), CL_SUCCESS);
    for(std::string s: {"s1", "s2"})
        ASSERT_EQ(store->inner.insertStory(storyIdOf("old", s), chronicleId("old"), "old", s), CL_SUCCESS);
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);
    EXPECT_EQ(names(portal), (std::vector<std::string>{"chronicle", "empty", "old"}));
    EXPECT_EQ(stories(portal, "old"), (std::vector<std::string>{"s1", "s2"}));
    EXPECT_TRUE(stories(portal, "empty").empty());
    EXPECT_EQ(portal.CreateChronicle(100, "old"), CL_ERR_CHRONICLE_EXISTS);
    auto before = store->log().size();
    auto acquired = portal.AcquireStory(100, "old", "s1");
    EXPECT_EQ(acquired.getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(acquired.getStoryId(), storyIdOf("old", "s1"));
    EXPECT_EQ(store->log().size(), before);
    EXPECT_EQ(starts[0].load(), 1);
}

// The portal opens its port only once the catalog is loaded: while the store's
// load is held, StartServices is refused, and once it returns the first
// request sees every loaded name.
TEST_P(VisorCatalog, LoadCompletesBeforePortsOpen)
{
    ASSERT_EQ(store->inner.insertChronicle(chronicleId("old"), "old"), CL_SUCCESS);
    store->holdLoad = true;
    VisorClientPortal portal(store);
    RPCProviderConf conf;
    conf.PROTO_CONF = protocol;
    conf.IP = "127.0.0.1";
    conf.BASE_PORT = 0;
    conf.SERVICE_PROVIDER_ID = 40;
    CatalogSnapshot loaded;
    auto loading = std::async(std::launch::async, [&] { return portal.LoadCatalog(loaded); });
    store->loadEntered.get_future().wait();
    EXPECT_NE(portal.StartServices(conf, registry), CL_SUCCESS);
    EXPECT_EQ(portal.core()->engine, nullptr);
    store->loadReleased.set_value();
    ASSERT_EQ(loading.get(), CL_SUCCESS);
    ASSERT_EQ(portal.StartServices(conf, registry), CL_SUCCESS);
    ClientId client = 100;
    uint64_t offset = 0;
    ASSERT_EQ(portal.ClientConnect(0, client, offset), CL_SUCCESS);
    EXPECT_EQ(names(portal), (std::vector<std::string>{"old"}));
}

// The flag is committed before the destroy is sent. A destroy that every
// grapher confirms leaves no row; one that a grapher does not confirm leaves
// the row with the flag.
TEST_P(VisorCatalog, DestroyFlagSetBeforeSendAndClearedWithTheRow)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);
    for(std::string s: {"confirmed", "unconfirmed"})
    {
        ASSERT_EQ(portal.AcquireStory(100, "chronicle", s).getErrorCode(), CL_SUCCESS);
        ASSERT_EQ(portal.ReleaseStory(100, "chronicle", s), CL_SUCCESS);
    }
    int destroysAtFlag = -1;
    store->onChange = [&](std::string const& entry)
    {
        if(entry.rfind("set", 0) == 0)
            destroysAtFlag = storyDestroys[0].load() + chronicleDestroys[0].load();
    };

    EXPECT_EQ(portal.DestroyStory(100, "chronicle", "confirmed"), CL_SUCCESS);
    EXPECT_EQ(destroysAtFlag, 0);
    EXPECT_EQ(findStory(store->rows(), "chronicle", "confirmed"), nullptr);
    EXPECT_EQ(store->log().back(), "deleteStory " + std::to_string(storyIdOf("chronicle", "confirmed")));

    destroysAtFlag = -1;
    destroyDelays[0] = 300;
    EXPECT_EQ(portal.DestroyStory(100, "chronicle", "unconfirmed"), CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_EQ(destroysAtFlag, 1);
    auto const rows = store->rows();
    auto row = findStory(rows, "chronicle", "unconfirmed");
    ASSERT_NE(row, nullptr);
    EXPECT_TRUE(row->destroyPending);
    EXPECT_EQ(stories(portal, "chronicle"), (std::vector<std::string>{"unconfirmed"}));
    std::this_thread::sleep_for(400ms);

    destroysAtFlag = -1;
    EXPECT_EQ(portal.DestroyChronicle(100, "chronicle"), CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_EQ(destroysAtFlag, 2);
    ASSERT_NE(findChronicle(store->rows(), "chronicle"), nullptr);
    EXPECT_TRUE(findChronicle(store->rows(), "chronicle")->destroyPending);
    std::this_thread::sleep_for(400ms);

    destroyDelays[0] = 0;
    EXPECT_EQ(portal.DestroyChronicle(100, "chronicle"), CL_SUCCESS);
    EXPECT_TRUE(store->rows().chronicles.empty());
    EXPECT_TRUE(store->rows().stories.empty());
}

// Review Focus 8: a destroy left unconfirmed when the visor stopped is sent
// again by the next create or acquire of the name, before anything else, as
// a destroy that ended unconfirmed in the same run would be.
TEST_P(VisorCatalog, PendingDestroyFlagSeedsTheMarker)
{
    delays[1] = delays[2] = 0;
    ASSERT_EQ(store->inner.insertChronicle(chronicleId("gone"), "gone"), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertStory(storyIdOf("gone", "s"), chronicleId("gone"), "gone", "s"), CL_SUCCESS);
    ASSERT_EQ(store->inner.setChronicleDestroyPending(chronicleId("gone"), true), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertChronicle(chronicleId("kept"), "kept"), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertStory(storyIdOf("kept", "s"), chronicleId("kept"), "kept", "s"), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertStory(storyIdOf("kept", "t"), chronicleId("kept"), "kept", "t"), CL_SUCCESS);
    ASSERT_EQ(store->inner.setStoryDestroyPending(storyIdOf("kept", "s"), true), CL_SUCCESS);
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);

    // The other story of the chronicle is not touched.
    ASSERT_EQ(portal.AcquireStory(100, "kept", "t").getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(storyDestroys[0].load(), 0);
    ASSERT_EQ(portal.ReleaseStory(100, "kept", "t"), CL_SUCCESS);

    auto startsBefore = starts[0].load();
    auto acquired = portal.AcquireStory(100, "kept", "s");
    EXPECT_EQ(acquired.getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(storyDestroys[0].load(), 1);
    EXPECT_EQ(starts[0].load(), startsBefore + 1);
    auto log = store->log();
    auto removed = indexOf(log, "deleteStory " + std::to_string(storyIdOf("kept", "s")));
    auto inserted = indexOf(log, "insertStory kept/s");
    ASSERT_LT(removed, log.size());
    ASSERT_LT(inserted, log.size());
    EXPECT_LT(removed, inserted);
    ASSERT_NE(findStory(store->rows(), "kept", "s"), nullptr);
    EXPECT_FALSE(findStory(store->rows(), "kept", "s")->destroyPending);

    EXPECT_EQ(portal.CreateChronicle(100, "gone"), CL_SUCCESS);
    EXPECT_EQ(chronicleDestroys[0].load(), 1);
    log = store->log();
    EXPECT_LT(indexOf(log, "deleteChronicle " + std::to_string(chronicleId("gone"))),
              indexOf(log, "insertChronicle gone"));
    ASSERT_NE(findChronicle(store->rows(), "gone"), nullptr);
    EXPECT_FALSE(findChronicle(store->rows(), "gone")->destroyPending);
    EXPECT_EQ(findStory(store->rows(), "gone", "s"), nullptr);
    EXPECT_TRUE(stories(portal, "gone").empty());
}

// The restored destroy is sent again but no grapher answers in time: the
// acquire gets CL_ERR_OUTCOME_UNCONFIRMED with no start sent, the create gets
// CL_ERR_CHRONICLE_EXISTS, and both rows stay flagged.
TEST_P(VisorCatalog, PendingDestroyFlagResendTimesOut)
{
    delays[1] = delays[2] = 0;
    ASSERT_EQ(store->inner.insertChronicle(chronicleId("gone"), "gone"), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertStory(storyIdOf("gone", "s"), chronicleId("gone"), "gone", "s"), CL_SUCCESS);
    ASSERT_EQ(store->inner.setChronicleDestroyPending(chronicleId("gone"), true), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertChronicle(chronicleId("kept"), "kept"), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertStory(storyIdOf("kept", "s"), chronicleId("kept"), "kept", "s"), CL_SUCCESS);
    ASSERT_EQ(store->inner.setStoryDestroyPending(storyIdOf("kept", "s"), true), CL_SUCCESS);
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);
    destroyDelays[0] = 300;

    EXPECT_EQ(portal.AcquireStory(100, "kept", "s").getErrorCode(), CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_EQ(storyDestroys[0].load(), 1);
    for(auto const& c: starts) EXPECT_EQ(c.load(), 0);
    ASSERT_NE(findStory(store->rows(), "kept", "s"), nullptr);
    EXPECT_TRUE(findStory(store->rows(), "kept", "s")->destroyPending);
    std::this_thread::sleep_for(400ms);

    EXPECT_EQ(portal.CreateChronicle(100, "gone"), CL_ERR_CHRONICLE_EXISTS);
    EXPECT_EQ(chronicleDestroys[0].load(), 1);
    ASSERT_NE(findChronicle(store->rows(), "gone"), nullptr);
    EXPECT_TRUE(findChronicle(store->rows(), "gone")->destroyPending);
    EXPECT_NE(findStory(store->rows(), "gone", "s"), nullptr);
    EXPECT_EQ(count(store->log(), "delete"), 0u);
    std::this_thread::sleep_for(400ms);
}

// A destroy of a name whose restored destroy is pending writes the flag again
// (the registry's placeholder or marker already exists) and, once the grapher
// answers in time, removes the rows.
TEST_P(VisorCatalog, DestroyAfterARestoredMarkerWritesTheFlagAgain)
{
    delays[1] = delays[2] = 0;
    ASSERT_EQ(store->inner.insertChronicle(chronicleId("gone"), "gone"), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertStory(storyIdOf("gone", "s"), chronicleId("gone"), "gone", "s"), CL_SUCCESS);
    ASSERT_EQ(store->inner.setChronicleDestroyPending(chronicleId("gone"), true), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertChronicle(chronicleId("kept"), "kept"), CL_SUCCESS);
    ASSERT_EQ(store->inner.insertStory(storyIdOf("kept", "s"), chronicleId("kept"), "kept", "s"), CL_SUCCESS);
    ASSERT_EQ(store->inner.setStoryDestroyPending(storyIdOf("kept", "s"), true), CL_SUCCESS);
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);
    int destroysAtFlag = -1;
    store->onChange = [&](std::string const& entry)
    {
        if(entry.rfind("set", 0) == 0)
            destroysAtFlag = storyDestroys[0].load() + chronicleDestroys[0].load();
    };

    EXPECT_EQ(portal.DestroyStory(100, "kept", "s"), CL_SUCCESS);
    EXPECT_EQ(count(store->log(), "setStoryDestroyPending " + std::to_string(storyIdOf("kept", "s")) + " 1"), 1u);
    EXPECT_EQ(destroysAtFlag, 0);
    EXPECT_EQ(storyDestroys[0].load(), 1);
    EXPECT_EQ(findStory(store->rows(), "kept", "s"), nullptr);
    EXPECT_TRUE(stories(portal, "kept").empty());

    destroysAtFlag = -1;
    EXPECT_EQ(portal.DestroyChronicle(100, "gone"), CL_SUCCESS);
    EXPECT_EQ(count(store->log(), "setChronicleDestroyPending " + std::to_string(chronicleId("gone")) + " 1"), 1u);
    EXPECT_EQ(destroysAtFlag, 1);
    EXPECT_EQ(chronicleDestroys[0].load(), 1);
    EXPECT_EQ(findChronicle(store->rows(), "gone"), nullptr);
    EXPECT_EQ(findStory(store->rows(), "gone", "s"), nullptr);
    EXPECT_EQ(names(portal), (std::vector<std::string>{"chronicle", "kept"}));
}

// A destroy that ended unconfirmed and that the next acquire or create
// finishes removes the name from the store as well as from the directory.
TEST_P(VisorCatalog, FinishedDestroyRemovesTheNameFromTheStore)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);
    ASSERT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    ASSERT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
    destroyDelays[0] = 300;
    EXPECT_NE(portal.DestroyStory(100, "chronicle", "story"), CL_SUCCESS);
    std::this_thread::sleep_for(400ms);
    destroyDelays[0] = 0;
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(storyDestroys[0].load(), 2);
    auto log = store->log();
    EXPECT_EQ(count(log, "deleteStory"), 1u);
    EXPECT_EQ(count(log, "insertStory chronicle/story"), 2u);
    ASSERT_NE(findStory(store->rows(), "chronicle", "story"), nullptr);
    EXPECT_FALSE(findStory(store->rows(), "chronicle", "story")->destroyPending);
    ASSERT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);

    destroyDelays[0] = 300;
    EXPECT_NE(portal.DestroyChronicle(100, "chronicle"), CL_SUCCESS);
    std::this_thread::sleep_for(400ms);
    destroyDelays[0] = 0;
    EXPECT_EQ(portal.CreateChronicle(100, "chronicle"), CL_SUCCESS);
    EXPECT_EQ(count(store->log(), "deleteChronicle"), 1u);
    ASSERT_NE(findChronicle(store->rows(), "chronicle"), nullptr);
    EXPECT_FALSE(findChronicle(store->rows(), "chronicle")->destroyPending);
    EXPECT_TRUE(store->rows().stories.empty());
}

// Every grapher confirmed the destroy but the row's delete failed: the name
// stays, flagged, and the next acquire sends the destroy again and removes it.
TEST_P(VisorCatalog, FailedDeleteAfterAConfirmedDestroyIsFinishedLater)
{
    delays[1] = delays[2] = 0;
    VisorClientPortal portal(store);
    load(portal);
    attach(portal);
    ASSERT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    ASSERT_EQ(portal.ReleaseStory(100, "chronicle", "story"), CL_SUCCESS);
    store->onChange = [&](std::string const& entry)
    {
        if(entry.rfind("deleteStory", 0) == 0 && storyDestroys[0].load() == 1)
            store->failNext = true;
    };
    EXPECT_EQ(portal.DestroyStory(100, "chronicle", "story"), CL_ERR_OUTCOME_UNCONFIRMED);
    EXPECT_EQ(storyDestroys[0].load(), 1);
    ASSERT_NE(findStory(store->rows(), "chronicle", "story"), nullptr);
    EXPECT_TRUE(findStory(store->rows(), "chronicle", "story")->destroyPending);
    EXPECT_EQ(portal.AcquireStory(100, "chronicle", "story").getErrorCode(), CL_SUCCESS);
    EXPECT_EQ(storyDestroys[0].load(), 2);
    ASSERT_NE(findStory(store->rows(), "chronicle", "story"), nullptr);
    EXPECT_FALSE(findStory(store->rows(), "chronicle", "story")->destroyPending);
}

INSTANTIATE_TEST_SUITE_P(Native, VisorCatalog, ::testing::Values("ofi+sockets", "ofi+tcp"));
