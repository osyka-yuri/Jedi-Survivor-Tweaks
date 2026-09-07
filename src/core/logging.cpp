#include "logging.hpp"

#include <windows.h>

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
    if (m_active && m_hasFileSink && m_path == path) {
        return true;
    }

    if (m_file.is_open()) {
        m_file.close();
    }

    m_path = path;
    m_hasFileSink = false;

    if (!path.empty()) {
        try {
            std::error_code ec;
            if (!path.parent_path().empty()) {
                std::filesystem::create_directories(path.parent_path(), ec);
            }
            if (!ec) {
                m_file.open(path, std::ios::out | std::ios::trunc);
                if (m_file.is_open()) {
                    m_hasFileSink = true;
                }
            }
        } catch (...) {
            m_hasFileSink = false;
        }
    }

    const auto startBanner = std::format("Log started: {}\n", FormatTimestamp());
    const auto sepBanner = "==================================================\n";

    ::OutputDebugStringA(startBanner.c_str());
    ::OutputDebugStringA(sepBanner);

    if (!m_active) {
        m_ringBuffer.clear();
        m_ringBuffer.push_back(startBanner);
        m_ringBuffer.push_back(sepBanner);
    }

    if (m_hasFileSink) {
        try {
            m_file << startBanner;
            m_file << sepBanner;
            m_file.flush();
            if (!m_file) {
                m_hasFileSink = false;
                m_file.close();
            }
        } catch (...) {
            m_hasFileSink = false;
        }
    }

    m_active = true;
    return m_hasFileSink;
}

void Logger::Shutdown() {
    std::lock_guard lock(m_mutex);
    if (m_file.is_open()) {
        m_file << "Log ended.\n";
        m_file.flush();
        m_file.close();
    }
    ::OutputDebugStringA("Log ended.\n");
    if (m_ringBuffer.size() >= kMaxRingBufferEntries) {
        m_ringBuffer.pop_front();
    }
    m_ringBuffer.push_back("Log ended.\n");
    m_hasFileSink = false;
    m_active = false;
}

void Logger::Log(LogLevel level, std::string_view message, std::source_location location) {
    std::lock_guard lock(m_mutex);
    if (!m_active) return;

    const auto line = std::format("[{}] {:>5} | [{}] {}\n",
                                  FormatTimestamp(),
                                  LevelToString(level),
                                  FormatLocation(location),
                                  message);

    ::OutputDebugStringA(line.c_str());

    if (m_hasFileSink && m_file.is_open()) {
        try {
            m_file << line;
            m_file.flush();
            if (!m_file) {
                m_hasFileSink = false;
                m_file.close();
            }
        } catch (...) {
            m_hasFileSink = false;
        }
    }

    if (m_ringBuffer.size() >= kMaxRingBufferEntries) {
        m_ringBuffer.pop_front();
    }
    m_ringBuffer.push_back(line);
}

void Logger::LogV(LogLevel level, std::string_view fmt, std::format_args args, std::source_location location) {
    if (m_minLevel > level) return;
    Log(level, std::vformat(fmt, args), location);
}

bool Logger::IsActive() const {
    std::lock_guard lock(m_mutex);
    return m_active;
}

bool Logger::HasFileSink() const {
    std::lock_guard lock(m_mutex);
    return m_hasFileSink;
}

std::filesystem::path Logger::GetLogPath() const {
    std::lock_guard lock(m_mutex);
    return m_path;
}

std::string Logger::DumpRecentEntries() const {
    std::lock_guard lock(m_mutex);
    size_t totalLen = 0;
    for (const auto& entry : m_ringBuffer) {
        totalLen += entry.size();
    }
    std::string dump;
    dump.reserve(totalLen);
    for (const auto& entry : m_ringBuffer) {
        dump.append(entry);
    }
    return dump;
}

std::vector<std::string> Logger::GetRecentEntries() const {
    std::lock_guard lock(m_mutex);
    return {m_ringBuffer.begin(), m_ringBuffer.end()};
}

} // namespace jst::core
