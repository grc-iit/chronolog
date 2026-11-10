// Unit tests for answerChunkSender, the answer the grapher's and the player's
// receive_story_chunk give the keeper that sent a StoryChunk. Over ofi+tcp, a
// respond to a keeper that was killed can fail with HG_TIMEOUT; thrown out of
// the handler, that thallium error ended the grapher. A real engine cannot be
// made to fail a respond on demand, so the helper takes a fake request here,
// and a second test checks that both handlers answer only through it.

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <chrono_monitor.h>
#include <ChunkReceipt.h>
#include <ChunkSenderAnswer.h>
#include <RpcAnswer.h>

namespace chl = chronolog;

namespace
{
void ensureLogger()
{
    static bool done = false;
    if(!done)
    {
        chl::chrono_monitor::initialize("console", "", chl::LogLevel::err, "chunk_sender_answer_test_logger");
        done = true;
    }
}

// A request answered with a return code, as the keeper's write handlers
// answer; respond throws a thallium error when told to fail.
struct FakeIntRequest
{
    bool fails = false;
    mutable int responds = 0;
    mutable int answered = 0;

    void respond(int code) const
    {
        ++responds;
        if(fails)
        {
            throw thallium::exception("[margo_respond] Function returned HG_TIMEOUT");
        }
        answered = code;
    }
};

// Records what it was answered with, or throws what it was told to.
struct FakeRequest
{
    enum class Fails
    {
        No,
        WithThalliumError,
        WithOtherError
    };
    Fails fails = Fails::No;
    mutable int responds = 0;
    mutable chl::ChunkReceipt answered{};

    void respond(chl::ChunkReceipt const& receipt) const
    {
        ++responds;
        if(fails == Fails::WithThalliumError)
        {
            throw thallium::exception("[margo_respond] Function returned HG_TIMEOUT");
        }
        if(fails == Fails::WithOtherError)
        {
            throw std::logic_error("not a transport error");
        }
        answered = receipt;
    }
};

chl::ChunkReceipt receiptOf(uint64_t bytes)
{
    chl::ChunkReceipt receipt;
    receipt.bytes = bytes;
    receipt.grapher_instance = 3;
    receipt.receipt = 11;
    return receipt;
}

std::string fileText(char const* path)
{
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// A handler's text, from its signature to `until` (the class's private
// section by default).
std::string handlerText(std::string const& file,
                        std::string const& from = "void receive_story_chunk(",
                        std::string const& until = "\nprivate:")
{
    std::size_t const start = file.find(from);
    if(start == std::string::npos)
    {
        return {};
    }
    std::size_t const end = file.find(until, start);
    return file.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::size_t occurrences(std::string const& text, std::string const& what)
{
    std::size_t count = 0;
    for(std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) { ++count; }
    return count;
}
} // namespace

TEST(ChunkSenderAnswer, AnswersAWorkingSenderOnce)
{
    ensureLogger();
    FakeRequest request;
    EXPECT_TRUE(chl::answerChunkSender(request, receiptOf(4096), "test"));
    EXPECT_EQ(request.responds, 1);
    EXPECT_EQ(request.answered.bytes, 4096u);
    EXPECT_EQ(request.answered.grapher_instance, 3u);
    EXPECT_EQ(request.answered.receipt, 11u);
}

// The failure that ended the grapher: the respond throws a thallium error,
// which the helper logs and swallows.
TEST(ChunkSenderAnswer, ThalliumErrorFromRespondDoesNotLeaveTheHandler)
{
    ensureLogger();
    FakeRequest request;
    request.fails = FakeRequest::Fails::WithThalliumError;
    bool answered = true;
    EXPECT_NO_THROW(answered = chl::answerChunkSender(request, receiptOf(4096), "test"));
    EXPECT_FALSE(answered);
    EXPECT_EQ(request.responds, 1);
}

// Only transport errors are swallowed; anything else is a bug and still shows.
TEST(ChunkSenderAnswer, OtherErrorsAreNotSwallowed)
{
    ensureLogger();
    FakeRequest request;
    request.fails = FakeRequest::Fails::WithOtherError;
    EXPECT_THROW(chl::answerChunkSender(request, receiptOf(4096), "test"), std::logic_error);
}

// Every answer in both handlers goes through BulkReceiveContext::answer, which
// catches a failed reply as answerChunkSender does: a failed pull, the
// checksum or deserialization failure, the received chunk and the failure
// path, and in the grapher also a duplicate of a written chunk. A request that
// finds no free slot is refused through BulkReceives::refuse, which catches
// too. Only the failure path keeps its slot, when a piece may be in flight.
TEST(ChunkSenderAnswer, BothReceiversAnswerOnlyThroughTheHelper)
{
    for(auto const& [path, answers]: {std::pair<char const*, std::size_t>{CHRONOLOG_GRAPHER_RECORDING_SERVICE, 5},
                                      std::pair<char const*, std::size_t>{CHRONOLOG_CHUNK_CONSUMER_SERVICE, 4}})
    {
        std::string const file = fileText(path);
        ASSERT_FALSE(file.empty()) << path;
        std::string const handler = handlerText(file);
        ASSERT_FALSE(handler.empty()) << "no receive_story_chunk in " << path;
        EXPECT_EQ(occurrences(handler, "request.respond("), 0u) << path;
        EXPECT_EQ(occurrences(handler, "context->answer("), answers) << path;
        EXPECT_EQ(occurrences(handler, "incoming.refuse(request"), 1u) << path;
        EXPECT_EQ(occurrences(handler, "lease->quarantine()"), 1u) << path;
    }
}

// The keeper's write handlers answer with a return code. The same failure
// would end the keeper with every client's writes in flight: a durable write
// whose client gave up during the group's sync, or died, cannot be answered.
TEST(ChunkSenderAnswer, ThalliumErrorFromAnIntAnswerDoesNotLeaveTheHandler)
{
    ensureLogger();
    FakeIntRequest request;
    EXPECT_TRUE(chl::answerRpcCaller(request, -19, "test"));
    EXPECT_EQ(request.answered, -19);
    request.fails = true;
    bool answered = true;
    EXPECT_NO_THROW(answered = chl::answerRpcCaller(request, 0, "test"));
    EXPECT_FALSE(answered);
    EXPECT_EQ(request.responds, 2);
}

// The keeper's handlers answer only through rpc::respond, which catches a
// failed reply: record_event (one answer), record_event_durable (two), then
// tail_get_sequences, tail_get_events and story_range_fetch (one each).
TEST(ChunkSenderAnswer, KeeperHandlersAnswerOnlyThroughTheHelper)
{
    std::string const file = fileText(CHRONOLOG_KEEPER_RECORDING_SERVICE);
    ASSERT_FALSE(file.empty());
    std::string const handlers = handlerText(file, "void record_event(");
    ASSERT_FALSE(handlers.empty()) << "no record_event in the keeper's recording service";
    for(char const* name:
        {"void record_event_durable(", "void tail_get_sequences(", "void tail_get_events(", "void story_range_fetch("})
    {
        EXPECT_NE(handlers.find(name), std::string::npos) << name;
    }
    EXPECT_EQ(occurrences(handlers, "request.respond("), 0u);
    EXPECT_EQ(occurrences(handlers, "rpc::respond(request, "), 6u);
}
