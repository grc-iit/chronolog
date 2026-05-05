#include <gtest/gtest.h>
#include <sstream>
#include <string>

#include <chronopubsub_logger.h>

namespace cps = chronopubsub;

/* ----------------------------------
  Tests for LogLevel enum
  ---------------------------------- */

TEST(ChronoPubSubLogger_LogLevel, TestLevelValues)
{
    // Verify level ordering (most verbose to least verbose)
    EXPECT_LT(static_cast<int>(cps::LogLevel::TRACE), static_cast<int>(cps::LogLevel::DEBUG));
    EXPECT_LT(static_cast<int>(cps::LogLevel::DEBUG), static_cast<int>(cps::LogLevel::INFO));
    EXPECT_LT(static_cast<int>(cps::LogLevel::INFO), static_cast<int>(cps::LogLevel::WARNING));
    EXPECT_LT(static_cast<int>(cps::LogLevel::WARNING), static_cast<int>(cps::LogLevel::ERROR));
    EXPECT_LT(static_cast<int>(cps::LogLevel::ERROR), static_cast<int>(cps::LogLevel::CRITICAL));
    EXPECT_LT(static_cast<int>(cps::LogLevel::CRITICAL), static_cast<int>(cps::LogLevel::OFF));
}

TEST(ChronoPubSubLogger_LogLevel, TestExactValues)
{
    // Verify exact numeric values as documented
    EXPECT_EQ(static_cast<int>(cps::LogLevel::TRACE), 0);
    EXPECT_EQ(static_cast<int>(cps::LogLevel::DEBUG), 1);
    EXPECT_EQ(static_cast<int>(cps::LogLevel::INFO), 2);
    EXPECT_EQ(static_cast<int>(cps::LogLevel::WARNING), 3);
    EXPECT_EQ(static_cast<int>(cps::LogLevel::ERROR), 4);
    EXPECT_EQ(static_cast<int>(cps::LogLevel::CRITICAL), 5);
    EXPECT_EQ(static_cast<int>(cps::LogLevel::OFF), 6);
}

/* ----------------------------------
  Tests for getDefaultLogLevel()
  ---------------------------------- */

TEST(ChronoPubSubLogger_DefaultLevel, TestBuildTypeDefault)
{
#ifdef NDEBUG
    // Release build: default should be ERROR
    EXPECT_EQ(cps::getDefaultLogLevel(), cps::LogLevel::ERROR);
#else
    // Debug build: default should be DEBUG
    EXPECT_EQ(cps::getDefaultLogLevel(), cps::LogLevel::DEBUG);
#endif
}

/* ----------------------------------
  Tests for logLevelToString()
  ---------------------------------- */

TEST(ChronoPubSubLogger_LogLevelToString, TestAllLevels)
{
    EXPECT_STREQ(cps::logLevelToString(cps::LogLevel::TRACE), "TRACE");
    EXPECT_STREQ(cps::logLevelToString(cps::LogLevel::DEBUG), "DEBUG");
    EXPECT_STREQ(cps::logLevelToString(cps::LogLevel::INFO), "INFO");
    EXPECT_STREQ(cps::logLevelToString(cps::LogLevel::WARNING), "WARNING");
    EXPECT_STREQ(cps::logLevelToString(cps::LogLevel::ERROR), "ERROR");
    EXPECT_STREQ(cps::logLevelToString(cps::LogLevel::CRITICAL), "CRITICAL");
    EXPECT_STREQ(cps::logLevelToString(cps::LogLevel::OFF), "OFF");
}

TEST(ChronoPubSubLogger_LogLevelToString, TestUnknownLevel)
{
    // Cast an invalid value to test default case
    auto invalidLevel = static_cast<cps::LogLevel>(99);
    EXPECT_STREQ(cps::logLevelToString(invalidLevel), "UNKNOWN");
}

/* ----------------------------------
  Tests for format_log_message()
  ---------------------------------- */

TEST(ChronoPubSubLogger_FormatMessage, TestSingleString)
{
    std::string result = cps::format_log_message("Simple message");
    EXPECT_EQ(result, "Simple message");
}

TEST(ChronoPubSubLogger_FormatMessage, TestWithOneArg)
{
    std::string result = cps::format_log_message("Value: ", 42);
    EXPECT_EQ(result, "Value: 42");
}

TEST(ChronoPubSubLogger_FormatMessage, TestWithStringArg)
{
    std::string result = cps::format_log_message("Key: ", "mykey");
    EXPECT_EQ(result, "Key: mykey");
}

TEST(ChronoPubSubLogger_FormatMessage, TestWithMultipleArgs)
{
    std::string result = cps::format_log_message("Test", 1, "two", 3.14);
    // Variadic template concatenates with spaces
    EXPECT_TRUE(result.find("Test") != std::string::npos);
    EXPECT_TRUE(result.find("1") != std::string::npos);
    EXPECT_TRUE(result.find("two") != std::string::npos);
    EXPECT_TRUE(result.find("3.14") != std::string::npos);
}

/* ----------------------------------
  Tests for logging macros
  ---------------------------------- */

// Helper to capture stderr output
class StderrCapture
{
public:
    StderrCapture()
    {
        old_buf_ = std::cerr.rdbuf();
        std::cerr.rdbuf(buffer_.rdbuf());
    }

    ~StderrCapture() { std::cerr.rdbuf(old_buf_); }

    std::string getOutput()
    {
        std::string output = buffer_.str();
        buffer_.str("");
        buffer_.clear();
        return output;
    }

private:
    std::stringstream buffer_;
    std::streambuf* old_buf_;
};

TEST(ChronoPubSubLogger_Macros, TestInfoMacroEnabled)
{
    StderrCapture capture;

    // With level INFO, INFO messages should appear
    cps::LogLevel level = cps::LogLevel::INFO;
    CHRONOPUBSUB_INFO(level, "Info message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoPubSub][INFO]") != std::string::npos);
    EXPECT_TRUE(output.find("Info message") != std::string::npos);
}

TEST(ChronoPubSubLogger_Macros, TestInfoMacroDisabled)
{
    StderrCapture capture;

    // With level WARNING, INFO messages should NOT appear
    cps::LogLevel level = cps::LogLevel::WARNING;
    CHRONOPUBSUB_INFO(level, "Info message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoPubSubLogger_Macros, TestWarningMacroEnabled)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::WARNING;
    CHRONOPUBSUB_WARNING(level, "Warning message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoPubSub][WARNING]") != std::string::npos);
    EXPECT_TRUE(output.find("Warning message") != std::string::npos);
}

TEST(ChronoPubSubLogger_Macros, TestWarningMacroDisabled)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::ERROR;
    CHRONOPUBSUB_WARNING(level, "Warning message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoPubSubLogger_Macros, TestErrorMacroEnabled)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::ERROR;
    CHRONOPUBSUB_ERROR(level, "Error message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoPubSub][ERROR]") != std::string::npos);
    EXPECT_TRUE(output.find("Error message") != std::string::npos);
}

TEST(ChronoPubSubLogger_Macros, TestErrorMacroDisabled)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::CRITICAL;
    CHRONOPUBSUB_ERROR(level, "Error message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoPubSubLogger_Macros, TestCriticalMacroEnabled)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::CRITICAL;
    CHRONOPUBSUB_CRITICAL(level, "Critical message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoPubSub][CRITICAL]") != std::string::npos);
    EXPECT_TRUE(output.find("Critical message") != std::string::npos);
}

TEST(ChronoPubSubLogger_Macros, TestCriticalMacroDisabledByOff)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::OFF;
    CHRONOPUBSUB_CRITICAL(level, "Critical message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoPubSubLogger_Macros, TestOffLevelDisablesAll)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::OFF;
    CHRONOPUBSUB_INFO(level, "Should not appear");
    CHRONOPUBSUB_WARNING(level, "Should not appear");
    CHRONOPUBSUB_ERROR(level, "Should not appear");
    CHRONOPUBSUB_CRITICAL(level, "Should not appear");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

// Debug-only tests - these verify TRACE and DEBUG macros
#ifndef NDEBUG

TEST(ChronoPubSubLogger_DebugMacros, TestTraceMacroEnabled)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::TRACE;
    CHRONOPUBSUB_TRACE(level, "Trace message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoPubSub][TRACE]") != std::string::npos);
    EXPECT_TRUE(output.find("Trace message") != std::string::npos);
}

TEST(ChronoPubSubLogger_DebugMacros, TestTraceMacroDisabled)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::DEBUG;
    CHRONOPUBSUB_TRACE(level, "Trace message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoPubSubLogger_DebugMacros, TestDebugMacroEnabled)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::DEBUG;
    CHRONOPUBSUB_DEBUG(level, "Debug message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoPubSub][DEBUG]") != std::string::npos);
    EXPECT_TRUE(output.find("Debug message") != std::string::npos);
}

TEST(ChronoPubSubLogger_DebugMacros, TestDebugMacroDisabled)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::INFO;
    CHRONOPUBSUB_DEBUG(level, "Debug message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoPubSubLogger_DebugMacros, TestAllLevelsWithTraceEnabled)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::TRACE;
    CHRONOPUBSUB_TRACE(level, "1");
    CHRONOPUBSUB_DEBUG(level, "2");
    CHRONOPUBSUB_INFO(level, "3");
    CHRONOPUBSUB_WARNING(level, "4");
    CHRONOPUBSUB_ERROR(level, "5");
    CHRONOPUBSUB_CRITICAL(level, "6");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[TRACE]") != std::string::npos);
    EXPECT_TRUE(output.find("[DEBUG]") != std::string::npos);
    EXPECT_TRUE(output.find("[INFO]") != std::string::npos);
    EXPECT_TRUE(output.find("[WARNING]") != std::string::npos);
    EXPECT_TRUE(output.find("[ERROR]") != std::string::npos);
    EXPECT_TRUE(output.find("[CRITICAL]") != std::string::npos);
}

#else // NDEBUG defined (Release build)

TEST(ChronoPubSubLogger_ReleaseMacros, TestTraceAndDebugAreNoOp)
{
    StderrCapture capture;

    // In Release builds, TRACE and DEBUG macros expand to nothing
    cps::LogLevel level = cps::LogLevel::TRACE;
    CHRONOPUBSUB_TRACE(level, "Should not compile to anything");
    CHRONOPUBSUB_DEBUG(level, "Should not compile to anything");

    // These should produce no output because macros are empty
    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoPubSubLogger_ReleaseMacros, TestInfoStillWorks)
{
    StderrCapture capture;

    cps::LogLevel level = cps::LogLevel::INFO;
    CHRONOPUBSUB_INFO(level, "Info in release");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[INFO]") != std::string::npos);
}

#endif // NDEBUG

/* ----------------------------------
  Tests for log_message() directly
  ---------------------------------- */

TEST(ChronoPubSubLogger_LogMessage, TestDirectLogMessage)
{
    StderrCapture capture;

    cps::log_message(cps::LogLevel::INFO, "Direct log test");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoPubSub][INFO]") != std::string::npos);
    EXPECT_TRUE(output.find("Direct log test") != std::string::npos);
}

TEST(ChronoPubSubLogger_LogMessage, TestAllLevelsDirect)
{
    StderrCapture capture;

    cps::log_message(cps::LogLevel::TRACE, "trace");
    cps::log_message(cps::LogLevel::DEBUG, "debug");
    cps::log_message(cps::LogLevel::INFO, "info");
    cps::log_message(cps::LogLevel::WARNING, "warning");
    cps::log_message(cps::LogLevel::ERROR, "error");
    cps::log_message(cps::LogLevel::CRITICAL, "critical");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[TRACE] trace") != std::string::npos);
    EXPECT_TRUE(output.find("[DEBUG] debug") != std::string::npos);
    EXPECT_TRUE(output.find("[INFO] info") != std::string::npos);
    EXPECT_TRUE(output.find("[WARNING] warning") != std::string::npos);
    EXPECT_TRUE(output.find("[ERROR] error") != std::string::npos);
    EXPECT_TRUE(output.find("[CRITICAL] critical") != std::string::npos);
}
