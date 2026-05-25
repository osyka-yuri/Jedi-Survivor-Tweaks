#include "logging.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <ranges>
#include <utility>

namespace jst::core {

namespace {

std::string_view LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
    }
    std::unreachable();
}

std::string FormatTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
    std::tm timeinfo{};
    if (localtime_s(&timeinfo, &time_t_now) != 0) {
        // Fallback: self-diagnosing string so 00:00:00.000 never appears as a
        // silent failure mode. Reports raw ms-since-epoch instead.
        const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now.time_since_epoch()).count();
        return std::format("epoch:{}", epochMs);
    }
    return std::format("{:02}:{:02}:{:02}.{:03}",
                       timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, ms.count());
}

std::string FormatLocation(std::source_location loc) {
    const std::filesystem::path p(loc.file_name());
    return std::format("{}:{}", p.stem().string(), loc.line());
}

// Case-insensitive ASCII equality. Both sides are lowercased on the fly so
// "INFO" and "info" compare equal. Same idiom used in custom_cvars.cpp.
bool IEqual(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

} // anonymous namespace

LogLevel ParseLogLevel(std::string_view text, LogLevel fallback) noexcept {
    if (IEqual(text, "debug")) return LogLevel::Debug;
    if (IEqual(text, "info"))  return LogLevel::Info;
    if (IEqual(text, "warn") || IEqual(text, "warning")) return LogLevel::Warning;
    if (IEqual(text, "error") || IEqual(text, "err"))    return LogLevel::Error;
    return fallback;
}

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

bool Logger::Initialize(const std::filesystem::path& path) {
    std::lock_guard lock(m_mutex);
    if (m_initialized) return true;

    try {
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        m_file.open(path, std::ios::out | std::ios::trunc);
        if (!m_file.is_open()) return false;

        m_initialized = true;
        m_file << "Log started: " << FormatTimestamp() << "\n";
        m_file << "==================================================\n";
        m_file.flush();
        return true;
    } catch (...) {
        return false;
    }
}

void Logger::Shutdown() {
    std::lock_guard lock(m_mutex);
    if (m_file.is_open()) {
        m_file << "Log ended.\n";
        m_file.flush();
        m_file.close();
    }
    m_initialized = false;
}

void Logger::Log(LogLevel level, std::string_view message, std::source_location location) {
    std::lock_guard lock(m_mutex);
    if (!m_initialized) return;

    try {
        m_file << '[' << FormatTimestamp() << "] "
               << std::setw(5) << std::setfill(' ') << LevelToString(level) << " | "
               << '[' << FormatLocation(location) << "] "
               << message << '\n';
        m_file.flush();
    } catch (...) {
        // Silently ignore write errors to avoid crashes due to logging issues.
    }
}

} // namespace jst::core
