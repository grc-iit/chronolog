// The shipped template's timings are not independent. A keeper holds every
// sealed chunk until the grapher writes it, so the grapher's window plus its
// acceptance window sets how long data lives only in memory, and three keeper
// knobs have to sit above that figure. These read the template the deploy
// scripts copy, so a change to one value that breaks a relationship fails here
// rather than in a deployment.

#include <gtest/gtest.h>

#include <json-c/json.h>

#include <string>

namespace
{
json_object* conf()
{
    static json_object* parsed = json_object_from_file(CHRONOLOG_CONF_TEMPLATE);
    return parsed;
}

int knob(char const* component, char const* key)
{
    json_object* component_json = json_object_object_get(conf(), component);
    EXPECT_NE(component_json, nullptr) << component;
    json_object* internals = json_object_object_get(component_json, "DataStoreInternals");
    EXPECT_NE(internals, nullptr) << component;
    json_object* value = json_object_object_get(internals, key);
    EXPECT_NE(value, nullptr) << component << "." << key;
    return value == nullptr ? -1 : json_object_get_int(value);
}

// How long a story's events live only in memory: the grapher opens a window,
// waits out its acceptance window, then writes it.
int writeWindowSecs()
{
    return knob("chrono_grapher", "story_chunk_duration_secs") + knob("chrono_grapher", "acceptance_window_secs");
}
} // namespace

TEST(ConfTemplate, TheTemplateParses) { ASSERT_NE(conf(), nullptr) << CHRONOLOG_CONF_TEMPLATE; }

TEST(ConfTemplate, EventsReachTheArchiveWithinNinetySeconds)
{
    // every keeper and the grapher hold that much data in memory, and a
    // stopping keeper waits that long for the grapher
    EXPECT_LE(writeWindowSecs(), 90);
}

TEST(ConfTemplate, KeepersWaitForTheGrapherBeforeSendingAgain)
{
    // below twice the write window, healthy chunks are sent and written twice
    EXPECT_GE(knob("chrono_keeper", "watermark_resend_timeout_secs"), 2 * writeWindowSecs());
}

TEST(ConfTemplate, AStoppingKeeperWaitsOutTheWriteWindow)
{
    // a shorter wait frees chunks the grapher has acknowledged but not written
    EXPECT_GE(knob("chrono_keeper", "shutdown_confirm_timeout_secs"), writeWindowSecs());
}

TEST(ConfTemplate, KeeperSealsWellInsideTheGrapherAcceptanceWindow)
{
    // a keeper ships a chunk story_chunk_duration + acceptance_window after its
    // first event; arriving after the grapher's window closed makes it a late
    // chunk, written to a second file for that window
    int const keeper_seal_secs =
            knob("chrono_keeper", "story_chunk_duration_secs") + knob("chrono_keeper", "acceptance_window_secs");
    EXPECT_LT(keeper_seal_secs, knob("chrono_grapher", "acceptance_window_secs"));
}

TEST(ConfTemplate, TheRetentionWarningIsAboveWhatAHealthyKeeperHolds)
{
    // A keeper holds a chunk from the moment it seals until the grapher writes
    // it, reports it, and the archive file is visible. That is the steady state
    // with nothing wrong, so a cap below it warns in normal operation and the
    // warning stops meaning anything. Sized here at the 10 MB/s per-keeper
    // reference rate the configuration docs use, with 3x headroom for a short
    // grapher hiccup.
    int const held_secs = knob("chrono_keeper", "story_chunk_duration_secs") +
                          knob("chrono_keeper", "acceptance_window_secs") + writeWindowSecs() +
                          knob("chrono_grapher", "watermark_report_interval_secs") +
                          knob("chrono_keeper", "archive_visibility_delay_secs");
    EXPECT_GE(knob("chrono_keeper", "retention_cap_mb"), 3 * held_secs * 10);
}
