#pragma once

#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <source_location>
#include <string_view>

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
    [[nodiscard]] static Logger& Instance();

    bool Initialize(const std::filesystem::path& path);
    void Shutdown();

    void Log(LogLevel level, std::string_view message,
             std::source_location location = std::source_location::current());

    void               SetMinLevel(LogLevel level) noexcept { m_minLevel = level; }
    [[nodiscard]] LogLevel GetMinLevel() const noexcept    { return m_minLevel; }

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::ofstream m_file;
    std::mutex    m_mutex;
    bool          m_initialized = false;
    LogLevel      m_minLevel = LogLevel::Info;
};

// Macros gate formatting cost on the configured min-level. Source location is
// captured implicitly through the default argument of `Log()`.
#define JST_LOG_IMPL(level, fmt, ...)                                         \
    do {                                                                      \
        auto& _jst_logger = ::jst::core::Logger::Instance();                  \
        if (_jst_logger.GetMinLevel() <= (level)) {                           \
            _jst_logger.Log((level), std::format(fmt __VA_OPT__(,) __VA_ARGS__)); \
        }                                                                     \
    } while (0)

#define JST_LOG_DEBUG(fmt, ...)   JST_LOG_IMPL(::jst::core::LogLevel::Debug,   fmt __VA_OPT__(,) __VA_ARGS__)
#define JST_LOG_INFO(fmt, ...)    JST_LOG_IMPL(::jst::core::LogLevel::Info,    fmt __VA_OPT__(,) __VA_ARGS__)
#define JST_LOG_WARNING(fmt, ...) JST_LOG_IMPL(::jst::core::LogLevel::Warning, fmt __VA_OPT__(,) __VA_ARGS__)
#define JST_LOG_ERROR(fmt, ...)   JST_LOG_IMPL(::jst::core::LogLevel::Error,   fmt __VA_OPT__(,) __VA_ARGS__)

} // namespace jst::core
