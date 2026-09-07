#pragma once

#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace jst::core {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

[[nodiscard]] LogLevel ParseLogLevel(std::string_view text, LogLevel fallback) noexcept;

class Logger final {
public:
    static constexpr size_t kMaxRingBufferEntries = 512;

    [[nodiscard]] static Logger& Instance();

    bool Initialize(const std::filesystem::path& path);
    void Shutdown();

    void Log(LogLevel level, std::string_view message,
             std::source_location location = std::source_location::current());
             
    void LogV(LogLevel level, std::string_view fmt, std::format_args args,
              std::source_location location = std::source_location::current());

    template <typename... Args>
    void LogFmt(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
        LogV(level, fmt.get(), std::make_format_args(args...));
    }

    void               SetMinLevel(LogLevel level) noexcept { m_minLevel = level; }
    [[nodiscard]] LogLevel GetMinLevel() const noexcept    { return m_minLevel; }

    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] bool HasFileSink() const;
    [[nodiscard]] std::filesystem::path GetLogPath() const;
    [[nodiscard]] std::string DumpRecentEntries() const;
    [[nodiscard]] std::vector<std::string> GetRecentEntries() const;

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::ofstream           m_file;
    mutable std::mutex      m_mutex;
    bool                    m_active = false;
    bool                    m_hasFileSink = false;
    std::filesystem::path   m_path;
    std::deque<std::string> m_ringBuffer;
    LogLevel                m_minLevel = LogLevel::Info;
};

// Macros pre-filter on the configured min-level to avoid constructing format
// arguments when the level is disabled (zero-cost fast path). LogV provides
// a second authoritative check so direct calls are also correctly filtered.
// Source location is captured implicitly via LogV's default argument.
#define JST_LOG_IMPL(level, fmt, ...)                                         \
    do {                                                                      \
        auto& _jst_logger = ::jst::core::Logger::Instance();                  \
        if (_jst_logger.GetMinLevel() <= (level)) {                           \
            _jst_logger.LogFmt((level), fmt __VA_OPT__(,) __VA_ARGS__);       \
        }                                                                     \
    } while (0)

#define JST_LOG_DEBUG(fmt, ...)   JST_LOG_IMPL(::jst::core::LogLevel::Debug,   fmt __VA_OPT__(,) __VA_ARGS__)
#define JST_LOG_INFO(fmt, ...)    JST_LOG_IMPL(::jst::core::LogLevel::Info,    fmt __VA_OPT__(,) __VA_ARGS__)
#define JST_LOG_WARNING(fmt, ...) JST_LOG_IMPL(::jst::core::LogLevel::Warning, fmt __VA_OPT__(,) __VA_ARGS__)
#define JST_LOG_ERROR(fmt, ...)   JST_LOG_IMPL(::jst::core::LogLevel::Error,   fmt __VA_OPT__(,) __VA_ARGS__)

} // namespace jst::core
