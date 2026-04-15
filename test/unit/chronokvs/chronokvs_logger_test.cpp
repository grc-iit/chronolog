#include <gtest/gtest.h>
#include <sstream>
#include <string>

#include <chronokvs_logger.h>

namespace ckvs = chronokvs;

/* ----------------------------------
  Tests for LogLevel enum
  ---------------------------------- */

TEST(ChronoKVSLogger_LogLevel, TestLevelValues)
{
    // Verify level ordering (most verbose to least verbose)
    EXPECT_LT(static_cast<int>(ckvs::LogLevel::LvlTrace), static_cast<int>(ckvs::LogLevel::LvlDebug));
    EXPECT_LT(static_cast<int>(ckvs::LogLevel::LvlDebug), static_cast<int>(ckvs::LogLevel::LvlInfo));
    EXPECT_LT(static_cast<int>(ckvs::LogLevel::LvlInfo), static_cast<int>(ckvs::LogLevel::LvlWarning));
    EXPECT_LT(static_cast<int>(ckvs::LogLevel::LvlWarning), static_cast<int>(ckvs::LogLevel::LvlError));
    EXPECT_LT(static_cast<int>(ckvs::LogLevel::LvlError), static_cast<int>(ckvs::LogLevel::LvlCritical));
    EXPECT_LT(static_cast<int>(ckvs::LogLevel::LvlCritical), static_cast<int>(ckvs::LogLevel::LvlOff));
}

TEST(ChronoKVSLogger_LogLevel, TestExactValues)
{
    // Verify exact numeric values as documented
    EXPECT_EQ(static_cast<int>(ckvs::LogLevel::LvlTrace), 0);
    EXPECT_EQ(static_cast<int>(ckvs::LogLevel::LvlDebug), 1);
    EXPECT_EQ(static_cast<int>(ckvs::LogLevel::LvlInfo), 2);
    EXPECT_EQ(static_cast<int>(ckvs::LogLevel::LvlWarning), 3);
    EXPECT_EQ(static_cast<int>(ckvs::LogLevel::LvlError), 4);
    EXPECT_EQ(static_cast<int>(ckvs::LogLevel::LvlCritical), 5);
    EXPECT_EQ(static_cast<int>(ckvs::LogLevel::LvlOff), 6);
}

/* ----------------------------------
  Tests for getDefaultLogLevel()
  ---------------------------------- */

TEST(ChronoKVSLogger_DefaultLevel, TestBuildTypeDefault)
{
#ifdef NDEBUG
    // Release build: default should be ERROR
    EXPECT_EQ(ckvs::getDefaultLogLevel(), ckvs::LogLevel::LvlError);
#else
    // Debug build: default should be DEBUG
    EXPECT_EQ(ckvs::getDefaultLogLevel(), ckvs::LogLevel::LvlDebug);
#endif
}

/* ----------------------------------
  Tests for logLevelToString()
  ---------------------------------- */

TEST(ChronoKVSLogger_LogLevelToString, TestAllLevels)
{
    EXPECT_STREQ(ckvs::logLevelToString(ckvs::LogLevel::LvlTrace), "TRACE");
    EXPECT_STREQ(ckvs::logLevelToString(ckvs::LogLevel::LvlDebug), "DEBUG");
    EXPECT_STREQ(ckvs::logLevelToString(ckvs::LogLevel::LvlInfo), "INFO");
    EXPECT_STREQ(ckvs::logLevelToString(ckvs::LogLevel::LvlWarning), "WARNING");
    EXPECT_STREQ(ckvs::logLevelToString(ckvs::LogLevel::LvlError), "ERROR");
    EXPECT_STREQ(ckvs::logLevelToString(ckvs::LogLevel::LvlCritical), "CRITICAL");
    EXPECT_STREQ(ckvs::logLevelToString(ckvs::LogLevel::LvlOff), "OFF");
}

TEST(ChronoKVSLogger_LogLevelToString, TestUnknownLevel)
{
    // Cast an invalid value to test default case
    auto invalidLevel = static_cast<ckvs::LogLevel>(99);
    EXPECT_STREQ(ckvs::logLevelToString(invalidLevel), "UNKNOWN");
}

/* ----------------------------------
  Tests for format_log_message()
  ---------------------------------- */

TEST(ChronoKVSLogger_FormatMessage, TestSingleString)
{
    std::string result = ckvs::format_log_message("Simple message");
    EXPECT_EQ(result, "Simple message");
}

TEST(ChronoKVSLogger_FormatMessage, TestWithOneArg)
{
    std::string result = ckvs::format_log_message("Value: ", 42);
    EXPECT_EQ(result, "Value: 42");
}

TEST(ChronoKVSLogger_FormatMessage, TestWithStringArg)
{
    std::string result = ckvs::format_log_message("Key: ", "mykey");
    EXPECT_EQ(result, "Key: mykey");
}

TEST(ChronoKVSLogger_FormatMessage, TestWithMultipleArgs)
{
    std::string result = ckvs::format_log_message("Test", 1, "two", 3.14);
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

TEST(ChronoKVSLogger_Macros, TestInfoMacroEnabled)
{
    StderrCapture capture;

    // With level INFO, INFO messages should appear
    ckvs::LogLevel level = ckvs::LogLevel::LvlInfo;
    CHRONOKVS_INFO(level, "Info message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoKVS][INFO]") != std::string::npos);
    EXPECT_TRUE(output.find("Info message") != std::string::npos);
}

TEST(ChronoKVSLogger_Macros, TestInfoMacroDisabled)
{
    StderrCapture capture;

    // With level WARNING, INFO messages should NOT appear
    ckvs::LogLevel level = ckvs::LogLevel::LvlWarning;
    CHRONOKVS_INFO(level, "Info message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoKVSLogger_Macros, TestWarningMacroEnabled)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlWarning;
    CHRONOKVS_WARNING(level, "Warning message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoKVS][WARNING]") != std::string::npos);
    EXPECT_TRUE(output.find("Warning message") != std::string::npos);
}

TEST(ChronoKVSLogger_Macros, TestWarningMacroDisabled)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlError;
    CHRONOKVS_WARNING(level, "Warning message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoKVSLogger_Macros, TestErrorMacroEnabled)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlError;
    CHRONOKVS_ERROR(level, "Error message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoKVS][ERROR]") != std::string::npos);
    EXPECT_TRUE(output.find("Error message") != std::string::npos);
}

TEST(ChronoKVSLogger_Macros, TestErrorMacroDisabled)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlCritical;
    CHRONOKVS_ERROR(level, "Error message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoKVSLogger_Macros, TestCriticalMacroEnabled)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlCritical;
    CHRONOKVS_CRITICAL(level, "Critical message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoKVS][CRITICAL]") != std::string::npos);
    EXPECT_TRUE(output.find("Critical message") != std::string::npos);
}

TEST(ChronoKVSLogger_Macros, TestCriticalMacroDisabledByOff)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlOff;
    CHRONOKVS_CRITICAL(level, "Critical message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoKVSLogger_Macros, TestOffLevelDisablesAll)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlOff;
    CHRONOKVS_INFO(level, "Should not appear");
    CHRONOKVS_WARNING(level, "Should not appear");
    CHRONOKVS_ERROR(level, "Should not appear");
    CHRONOKVS_CRITICAL(level, "Should not appear");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

// Debug-only tests - these verify TRACE and DEBUG macros
#ifndef NDEBUG

TEST(ChronoKVSLogger_DebugMacros, TestTraceMacroEnabled)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlTrace;
    CHRONOKVS_TRACE(level, "Trace message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoKVS][TRACE]") != std::string::npos);
    EXPECT_TRUE(output.find("Trace message") != std::string::npos);
}

TEST(ChronoKVSLogger_DebugMacros, TestTraceMacroDisabled)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlDebug;
    CHRONOKVS_TRACE(level, "Trace message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoKVSLogger_DebugMacros, TestDebugMacroEnabled)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlDebug;
    CHRONOKVS_DEBUG(level, "Debug message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoKVS][DEBUG]") != std::string::npos);
    EXPECT_TRUE(output.find("Debug message") != std::string::npos);
}

TEST(ChronoKVSLogger_DebugMacros, TestDebugMacroDisabled)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlInfo;
    CHRONOKVS_DEBUG(level, "Debug message");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoKVSLogger_DebugMacros, TestAllLevelsWithTraceEnabled)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlTrace;
    CHRONOKVS_TRACE(level, "1");
    CHRONOKVS_DEBUG(level, "2");
    CHRONOKVS_INFO(level, "3");
    CHRONOKVS_WARNING(level, "4");
    CHRONOKVS_ERROR(level, "5");
    CHRONOKVS_CRITICAL(level, "6");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[TRACE]") != std::string::npos);
    EXPECT_TRUE(output.find("[DEBUG]") != std::string::npos);
    EXPECT_TRUE(output.find("[INFO]") != std::string::npos);
    EXPECT_TRUE(output.find("[WARNING]") != std::string::npos);
    EXPECT_TRUE(output.find("[ERROR]") != std::string::npos);
    EXPECT_TRUE(output.find("[CRITICAL]") != std::string::npos);
}

#else // NDEBUG defined (Release build)

TEST(ChronoKVSLogger_ReleaseMacros, TestTraceAndDebugAreNoOp)
{
    StderrCapture capture;

    // In Release builds, TRACE and DEBUG macros expand to nothing
    ckvs::LogLevel level = ckvs::LogLevel::LvlTrace;
    CHRONOKVS_TRACE(level, "Should not compile to anything");
    CHRONOKVS_DEBUG(level, "Should not compile to anything");

    // These should produce no output because macros are empty
    std::string output = capture.getOutput();
    EXPECT_TRUE(output.empty());
}

TEST(ChronoKVSLogger_ReleaseMacros, TestInfoStillWorks)
{
    StderrCapture capture;

    ckvs::LogLevel level = ckvs::LogLevel::LvlInfo;
    CHRONOKVS_INFO(level, "Info in release");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[INFO]") != std::string::npos);
}

#endif // NDEBUG

/* ----------------------------------
  Tests for log_message() directly
  ---------------------------------- */

TEST(ChronoKVSLogger_LogMessage, TestDirectLogMessage)
{
    StderrCapture capture;

    ckvs::log_message(ckvs::LogLevel::LvlInfo, "Direct log test");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[ChronoKVS][INFO]") != std::string::npos);
    EXPECT_TRUE(output.find("Direct log test") != std::string::npos);
}

TEST(ChronoKVSLogger_LogMessage, TestAllLevelsDirect)
{
    StderrCapture capture;

    ckvs::log_message(ckvs::LogLevel::LvlTrace, "trace");
    ckvs::log_message(ckvs::LogLevel::LvlDebug, "debug");
    ckvs::log_message(ckvs::LogLevel::LvlInfo, "info");
    ckvs::log_message(ckvs::LogLevel::LvlWarning, "warning");
    ckvs::log_message(ckvs::LogLevel::LvlError, "error");
    ckvs::log_message(ckvs::LogLevel::LvlCritical, "critical");

    std::string output = capture.getOutput();
    EXPECT_TRUE(output.find("[TRACE] trace") != std::string::npos);
    EXPECT_TRUE(output.find("[DEBUG] debug") != std::string::npos);
    EXPECT_TRUE(output.find("[INFO] info") != std::string::npos);
    EXPECT_TRUE(output.find("[WARNING] warning") != std::string::npos);
    EXPECT_TRUE(output.find("[ERROR] error") != std::string::npos);
    EXPECT_TRUE(output.find("[CRITICAL] critical") != std::string::npos);
}
