#include "core/logging.hpp"
#include "test_check.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string ReadAllText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

} // anonymous namespace

void TestLogging() {
    auto& logger = jst::core::Logger::Instance();
    logger.Shutdown();

    wchar_t tempRoot[MAX_PATH]{};
    const DWORD rootLength = GetTempPathW(MAX_PATH, tempRoot);
    Check(rootLength != 0 && rootLength < MAX_PATH,
          "temporary root is available for logging tests");

    const auto tempDir = std::filesystem::path(tempRoot) /
        (L"JST.LoggingTests." + std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(tempDir);

    // Test 1: Valid writable path
    {
        const auto logFile = tempDir / L"ValidLog.log";
        const bool attached = logger.Initialize(logFile);
        Check(attached, "Initialize returns true when file sink is successfully attached");
        Check(logger.HasFileSink(), "HasFileSink returns true for valid path");
        Check(logger.GetLogPath() == logFile, "GetLogPath returns configured path");

        logger.SetMinLevel(jst::core::LogLevel::Debug);
        JST_LOG_INFO("Valid sink entry: {}", 42);

        const auto dump = logger.DumpRecentEntries();
        Check(dump.find("Valid sink entry: 42") != std::string::npos,
              "DumpRecentEntries contains the logged text");

        const auto diskContent = ReadAllText(logFile);
        Check(diskContent.find("Valid sink entry: 42") != std::string::npos,
              "Log file on disk contains the logged text");

        logger.Shutdown();
        Check(!logger.HasFileSink(), "HasFileSink is false after Shutdown");
    }

    // Test 2: Unwritable / invalid path - graceful degradation to in-memory ring buffer
    {
        // Path with invalid filename characters on Windows cannot be created as a file
        const auto unwritableFile = std::filesystem::path(L"Z:\\NonExistentDrive_JST_Test\\<?>:invalid/test.log");
        const bool attached = logger.Initialize(unwritableFile);
        Check(!attached, "Initialize returns false for unwritable path");
        Check(!logger.HasFileSink(), "HasFileSink is false for unwritable path");
        Check(logger.GetLogPath() == unwritableFile, "GetLogPath returns requested path even on sink failure");

        JST_LOG_WARNING("Memory fallback message: {}", 99);

        const auto dump = logger.DumpRecentEntries();
        Check(dump.find("Memory fallback message: 99") != std::string::npos,
              "In-memory ring buffer captures log entries even when file sink is unavailable");

        const auto recent = logger.GetRecentEntries();
        Check(!recent.empty(), "GetRecentEntries returns non-empty list");

        logger.Shutdown();
    }

    // Test 3: Ring buffer capacity capping
    {
        const auto memOnlyFile = std::filesystem::path(L"");
        (void)logger.Initialize(memOnlyFile);

        for (size_t i = 0; i < jst::core::Logger::kMaxRingBufferEntries + 50; ++i) {
            JST_LOG_INFO("Sequence message: {}", i);
        }

        const auto recent = logger.GetRecentEntries();
        Check(recent.size() == jst::core::Logger::kMaxRingBufferEntries,
              "Ring buffer size does not exceed kMaxRingBufferEntries");

        const auto dump = logger.DumpRecentEntries();
        Check(dump.find("Sequence message: 0\n") == std::string::npos,
              "Oldest messages are evicted once ring buffer capacity is reached");
        Check(dump.find(std::format("Sequence message: {}",
                                    jst::core::Logger::kMaxRingBufferEntries + 49)) != std::string::npos,
              "Newest messages are retained at the end of the ring buffer");

        logger.Shutdown();
    }

    // Test 4: Re-attempting Initialize with valid path attaches sink even after prior failure
    {
        const auto unwritableFile = std::filesystem::path(L"Z:\\NonExistentDrive_JST_Test\\<?>:invalid/test.log");
        Check(!logger.Initialize(unwritableFile), "first init to unwritable path returns false");
        Check(!logger.HasFileSink(), "no file sink attached");
        Check(logger.IsActive(), "logger subsystem is active in memory-only mode");

        const auto validFile = tempDir / L"RecoveredLog.log";
        Check(logger.Initialize(validFile), "second init to valid path successfully attaches file sink");
        Check(logger.HasFileSink(), "file sink is now attached");
        Check(logger.GetLogPath() == validFile, "active path updated to valid file");

        JST_LOG_INFO("Recovered message: {}", 777);
        const auto diskContent = ReadAllText(validFile);
        Check(diskContent.find("Recovered message: 777") != std::string::npos,
              "Recovered log file contains message written after sink re-attachment");

        logger.Shutdown();
        Check(!logger.IsActive(), "logger is inactive after Shutdown");
    }

    std::error_code ignored;
    std::filesystem::remove_all(tempDir, ignored);
}
