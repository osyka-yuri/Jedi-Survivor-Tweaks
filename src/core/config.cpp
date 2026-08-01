#include "config.hpp"
#include "ini_helpers.hpp"
#include "logging.hpp"

#include <windows.h>

#include <atomic>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <set>

namespace jst::core {

namespace {

class TempFileGuard final {
public:
    explicit TempFileGuard(std::filesystem::path path)
        : m_path(std::move(path)) {}
    ~TempFileGuard() {
        if (!m_path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
        }
    }

    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
    TempFileGuard(TempFileGuard&& other) noexcept
        : m_path(std::move(other.m_path)) {
        other.m_path.clear();
    }
    TempFileGuard& operator=(TempFileGuard&&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return m_path;
    }
    void Release() noexcept { m_path.clear(); }

private:
    std::filesystem::path m_path;
};

[[nodiscard]] std::optional<TempFileGuard> CreateSiblingTemp(
    const std::filesystem::path& target) {
    static std::atomic<uint64_t> sequence{0};
    for (size_t attempt = 0; attempt < 32; ++attempt) {
        const uint64_t id = sequence.fetch_add(1, std::memory_order_relaxed);
        auto candidate = target.parent_path() /
            (target.filename().wstring() + L"." +
             std::to_wstring(GetCurrentProcessId()) + L"." +
             std::to_wstring(GetTickCount64()) + L"." +
             std::to_wstring(id) + L".tmp");
        const HANDLE file = CreateFileW(
            candidate.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            return TempFileGuard(std::move(candidate));
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            break;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool FlushFileToDisk(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        JST_LOG_ERROR(
            "Config::Save: cannot reopen temp file '{}' for durable flush "
            "(Win32 error {}).",
            path.string(),
            GetLastError());
        return false;
    }

    const BOOL flushed = FlushFileBuffers(file);
    const DWORD flushError = flushed ? ERROR_SUCCESS : GetLastError();
    const BOOL closed = CloseHandle(file);
    const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
    if (!flushed || !closed) {
        JST_LOG_ERROR(
            "Config::Save: durable flush failed for '{}' "
            "(flush error {}, close error {}).",
            path.string(),
            flushError,
            closeError);
        return false;
    }
    return true;
}

[[nodiscard]] std::optional<bool> PathExists(
    const std::filesystem::path& path,
    DWORD& error) noexcept {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        error = ERROR_SUCCESS;
        return true;
    }
    error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] bool MoveReplacing(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    DWORD& error) noexcept {
    if (MoveFileExW(
            source.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = ERROR_SUCCESS;
        return true;
    }
    error = GetLastError();
    return false;
}

} // namespace

using jst::core::detail::Trim;
using jst::core::detail::MatchesAny;
using jst::core::detail::kFalseLiterals;
using jst::core::detail::kTrueLiterals;

bool Config::Load(const std::filesystem::path& path, SaveMode mode) {
    m_path = path;
    m_saveMode = mode;
    m_cache.clear();
    m_rawLines.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        JST_LOG_WARNING("Failed to open config file: '{}'.", path.string());
        return false;
    }

    const bool captureRaw = (mode == SaveMode::PreserveComments);

    std::string currentSection;
    std::string line;
    while (std::getline(file, line)) {
        // Strip trailing CR so rawLines (if captured) store clean text
        // regardless of line-ending style.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (captureRaw) m_rawLines.push_back(line);

        const std::string_view sv = Trim(line);
        if (sv.empty() || sv[0] == ';' || sv[0] == '#') continue;

        if (sv.front() == '[' && sv.back() == ']') {
            currentSection = std::string(Trim(sv.substr(1, sv.size() - 2)));
            continue;
        }

        const auto eqPos = sv.find('=');
        if (eqPos == std::string_view::npos) continue;

        std::string key(Trim(sv.substr(0, eqPos)));
        std::string val(Trim(sv.substr(eqPos + 1)));
        if (key.empty()) continue;
        if (currentSection.empty()) {
            // The .ini schema doesn't promise meaningful behavior for keys
            // outside a [section]. We still accept them (so legacy configs
            // don't break) but the user should know they likely won't be read
            // back: GetString("", key, ...) is the only path that reaches them.
            JST_LOG_WARNING("Config: orphan key '{}' before any [section]; assigning to default section.", key);
        }
        m_cache[currentSection][std::move(key)] = std::move(val);
    }

    JST_LOG_INFO("Config loaded: '{}'.", path.string());
    return true;
}

bool Config::Reload() {
    if (m_path.empty()) return false;
    const bool res = Load(m_path, m_saveMode);
    if (res) JST_LOG_INFO("Config reloaded.");
    return res;
}

std::optional<std::string_view> Config::GetRawOpt(std::string_view section, std::string_view key) const {
    const auto sit = m_cache.find(section);
    if (sit == m_cache.end()) return std::nullopt;
    const auto kit = sit->second.find(key);
    if (kit == sit->second.end()) return std::nullopt;
    return std::string_view(kit->second);
}

std::string Config::GetString(std::string_view section, std::string_view key, std::string_view defaultValue) const {
    const auto raw = GetRawOpt(section, key);
    return raw ? std::string(*raw) : std::string(defaultValue);
}

int Config::GetInt(std::string_view section, std::string_view key, int defaultValue) const {
    const auto raw = GetRawOpt(section, key);
    if (!raw) return defaultValue;
    const auto valueText = Trim(*raw);
    int value = 0;
    const auto [end, error] = std::from_chars(
        valueText.data(), valueText.data() + valueText.size(), value);
    if (error == std::errc{} && end == valueText.data() + valueText.size()) {
        return value;
    }
    JST_LOG_WARNING(
        "Invalid integer [{}] {}='{}'; using default {}.",
        section, key, *raw, defaultValue);
    return defaultValue;
}

float Config::GetFloat(std::string_view section, std::string_view key, float defaultValue) const {
    const auto raw = GetRawOpt(section, key);
    if (!raw) return defaultValue;
    const auto valueText = Trim(*raw);
    float value = 0.0f;
    const auto [end, error] = std::from_chars(
        valueText.data(), valueText.data() + valueText.size(), value);
    if (error == std::errc{} &&
        end == valueText.data() + valueText.size() &&
        std::isfinite(value)) {
        return value;
    }
    JST_LOG_WARNING(
        "Invalid finite number [{}] {}='{}'; using default {}.",
        section, key, *raw, defaultValue);
    return defaultValue;
}

float Config::GetFloatInRange(
    std::string_view section,
    std::string_view key,
    float defaultValue,
    float minimum,
    float maximum) const {
    const float value = GetFloat(section, key, defaultValue);
    if (value >= minimum && value <= maximum) {
        return value;
    }
    JST_LOG_WARNING(
        "Out-of-range number [{}] {}={} (expected {}..{}); using default {}.",
        section, key, value, minimum, maximum, defaultValue);
    return defaultValue;
}

bool Config::GetBool(std::string_view section, std::string_view key, bool defaultValue) const {
    const auto raw = GetRawOpt(section, key);
    if (!raw) return defaultValue;
    const auto valueText = Trim(*raw);
    if (detail::MatchesAny(valueText, detail::kFalseLiterals)) return false;
    if (detail::MatchesAny(valueText, detail::kTrueLiterals))  return true;
    JST_LOG_WARNING(
        "Invalid boolean [{}] {}='{}'; using default {}.",
        section, key, *raw, defaultValue);
    return defaultValue;
}

bool Config::HasSection(std::string_view section) const {
    return m_cache.find(section) != m_cache.end();
}

const Config::Section* Config::GetSection(std::string_view section) const {
    const auto it = m_cache.find(section);
    return it == m_cache.end() ? nullptr : &it->second;
}

void Config::SetString(std::string_view section, std::string_view key, std::string value) {
    m_cache[std::string(section)][std::string(key)] = std::move(value);
}

void Config::SetFloat(std::string_view section, std::string_view key, float value) {
    SetString(section, key, std::format("{:.6g}", value));
}

void Config::SetInt(std::string_view section, std::string_view key, int value) {
    SetString(section, key, std::to_string(value));
}

void Config::SetBool(std::string_view section, std::string_view key, bool value) {
    SetString(section, key, value ? "true" : "false");
}

bool Config::Save() {
    if (m_path.empty()) return false;

    auto temp = CreateSiblingTemp(m_path);
    if (!temp) {
        JST_LOG_ERROR(
            "Config::Save: cannot create a unique sibling temp for '{}'.",
            m_path.string());
        return false;
    }
    const auto tmpPath = temp->Path();
    const bool wrote = (m_saveMode == SaveMode::PreserveComments)
                           ? SavePreserveComments(tmpPath)
                           : SaveDeterministic(tmpPath);
    if (!wrote) {
        return false;
    }
    if (!FlushFileToDisk(tmpPath)) {
        return false;
    }

    DWORD targetInspectError = ERROR_SUCCESS;
    const auto targetExists = PathExists(m_path, targetInspectError);
    if (!targetExists) {
        temp->Release();
        JST_LOG_ERROR(
            "Config::Save: cannot inspect '{}' (Win32 error {}). The flushed "
            "replacement was preserved at '{}'.",
            m_path.string(),
            targetInspectError,
            tmpPath.string());
        return false;
    }

    if (!*targetExists) {
        DWORD moveError = ERROR_SUCCESS;
        if (!MoveReplacing(tmpPath, m_path, moveError)) {
            temp->Release();
            JST_LOG_ERROR(
                "Config::Save: atomic MoveFileExW failed for '{}' "
                "(Win32 error {}). The flushed replacement was preserved "
                "at '{}'.",
                m_path.string(),
                moveError,
                tmpPath.string());
            return false;
        }
        temp->Release();
        JST_LOG_INFO("Config saved: '{}'.", m_path.string());
        return true;
    }

    // ReplaceFileW may partially rename its operands even when it reports an
    // error. Supplying a known backup path gives us a deterministic copy of
    // the original that can be restored before any recovery artifact is
    // removed. The replacement itself was flushed above; the documented
    // REPLACEFILE_WRITE_THROUGH flag is unsupported and is deliberately not
    // used.
    const auto backupPath = std::filesystem::path(
        tmpPath.wstring() + L".backup");
    DWORD backupInspectError = ERROR_SUCCESS;
    const auto backupExistsBefore = PathExists(backupPath, backupInspectError);
    if (!backupExistsBefore || *backupExistsBefore) {
        JST_LOG_ERROR(
            "Config::Save: backup path '{}' is not available "
            "(Win32 error {}).",
            backupPath.string(),
            backupInspectError);
        return false;
    }
    TempFileGuard backup(backupPath);

    if (ReplaceFileW(
            m_path.c_str(),
            tmpPath.c_str(),
            backupPath.c_str(),
            0,
            nullptr,
            nullptr)) {
        temp->Release();
        JST_LOG_INFO("Config saved: '{}'.", m_path.string());
        return true;
    }

    const DWORD replaceError = GetLastError();
    DWORD backupError = ERROR_SUCCESS;
    DWORD targetError = ERROR_SUCCESS;
    DWORD tempError = ERROR_SUCCESS;
    const auto backupExists = PathExists(backupPath, backupError);
    const auto targetExistsAfter = PathExists(m_path, targetError);
    const auto tempExistsAfter = PathExists(tmpPath, tempError);
    if (!backupExists || !targetExistsAfter || !tempExistsAfter) {
        // The filesystem outcome cannot be classified safely. Retain every
        // possible recovery artifact instead of letting RAII erase evidence or
        // the only surviving copy.
        backup.Release();
        temp->Release();
        JST_LOG_ERROR(
            "Config::Save: ReplaceFileW failed for '{}' (Win32 error {}) and "
            "recovery state is ambiguous (backup {}, target {}, temp {}). "
            "Artifacts were preserved at '{}' and '{}'.",
            m_path.string(),
            replaceError,
            backupError,
            targetError,
            tempError,
            backupPath.string(),
            tmpPath.string());
        return false;
    }

    if (*backupExists) {
        DWORD restoreError = ERROR_SUCCESS;
        if (MoveReplacing(backupPath, m_path, restoreError)) {
            JST_LOG_ERROR(
                "Config::Save: ReplaceFileW failed for '{}' (Win32 error {}); "
                "the original file was restored from backup.",
                m_path.string(),
                replaceError);
            return false;
        }

        backup.Release();
        temp->Release();
        JST_LOG_ERROR(
            "Config::Save: ReplaceFileW failed for '{}' (Win32 error {}) and "
            "the original backup could not be restored (Win32 error {}). "
            "Recovery artifacts were preserved at '{}' and '{}'.",
            m_path.string(),
            replaceError,
            restoreError,
            backupPath.string(),
            tmpPath.string());
        return false;
    }

    if (*targetExistsAfter) {
        JST_LOG_ERROR(
            "Config::Save: ReplaceFileW failed for '{}' (Win32 error {}); "
            "the existing target remains intact.",
            m_path.string(),
            replaceError);
        return false;
    }

    if (*tempExistsAfter) {
        DWORD recoveryError = ERROR_SUCCESS;
        if (MoveReplacing(tmpPath, m_path, recoveryError)) {
            temp->Release();
            JST_LOG_WARNING(
                "Config::Save: ReplaceFileW failed for '{}' (Win32 error {}), "
                "but the flushed replacement was recovered successfully.",
                m_path.string(),
                replaceError);
            return true;
        }

        temp->Release();
        JST_LOG_ERROR(
            "Config::Save: ReplaceFileW failed for '{}' (Win32 error {}) and "
            "the replacement could not be recovered (Win32 error {}). "
            "The flushed temp file was preserved at '{}'.",
            m_path.string(),
            replaceError,
            recoveryError,
            tmpPath.string());
        return false;
    }

    JST_LOG_ERROR(
        "Config::Save: ReplaceFileW failed for '{}' (Win32 error {}) and no "
        "recoverable filesystem object remains.",
        m_path.string(),
        replaceError);
    return false;
}

bool Config::SavePreserveComments(const std::filesystem::path& tmpPath) const {
    // Pre-scan rawLines to build the set of (section, key) pairs already
    // present, so we know what is new and needs to be appended.
    std::map<std::string, std::set<std::string>> inRaw;
    {
        std::string sec;
        for (const auto& rawLine : m_rawLines) {
            const auto sv = Trim(rawLine);
            if (sv.size() >= 2 && sv.front() == '[' && sv.back() == ']') {
                sec = std::string(Trim(sv.substr(1, sv.size() - 2)));
            } else if (!sv.empty() && sv[0] != ';' && sv[0] != '#') {
                const auto eq = sv.find('=');
                if (eq != std::string_view::npos) {
                    auto k = std::string(Trim(sv.substr(0, eq)));
                    if (!k.empty()) inRaw[sec].insert(k);
                }
            }
        }
    }

    std::vector<std::string> output;
    output.reserve(m_rawLines.size() + 16);
    std::string currentSection;

    // Append any cache keys for `section` not yet emitted to output.
    auto appendPendingKeys = [&](std::string_view section) {
        auto sit = m_cache.find(section);
        if (sit == m_cache.end()) return;
        for (const auto& [k, v] : sit->second) {
            const std::string secStr(section);
            if (!inRaw[secStr].count(k)) {
                output.push_back(k + " = " + v);
                inRaw[secStr].insert(k);
            }
        }
    };

    for (const auto& rawLine : m_rawLines) {
        const auto sv = Trim(rawLine);

        // Section header: flush pending keys for the outgoing section first.
        if (sv.size() >= 2 && sv.front() == '[' && sv.back() == ']') {
            appendPendingKeys(currentSection);
            currentSection = std::string(Trim(sv.substr(1, sv.size() - 2)));
            output.push_back(rawLine);
            continue;
        }

        // Key = value line: replace with cached value if present.
        if (!sv.empty() && sv[0] != ';' && sv[0] != '#') {
            const auto eq = sv.find('=');
            if (eq != std::string_view::npos) {
                auto key = std::string(Trim(sv.substr(0, eq)));
                if (!key.empty()) {
                    auto sit = m_cache.find(currentSection);
                    if (sit != m_cache.end()) {
                        auto kit = sit->second.find(key);
                        if (kit != sit->second.end()) {
                            // Build the updated line, preserving any inline
                            // comment that appeared after the value in the
                            // original file (e.g. "; 0.0-10.0 (default)").
                            std::string newLine = key + " = " + kit->second;
                            const auto rawEqOfs = rawLine.find('=');
                            if (rawEqOfs != std::string::npos) {
                                const auto cmtOfs =
                                    rawLine.find_first_of(";#", rawEqOfs + 1);
                                if (cmtOfs != std::string::npos) {
                                    newLine += "  ";
                                    newLine += rawLine.substr(cmtOfs);
                                }
                            }
                            output.push_back(std::move(newLine));
                            continue;
                        }
                    }
                }
            }
        }

        // Comment, blank line, or unrecognised entry: preserve as-is.
        output.push_back(rawLine);
    }

    // Flush any pending keys for the final section in the file.
    appendPendingKeys(currentSection);

    // Append entirely new sections (not present anywhere in rawLines).
    for (const auto& [sec, secMap] : m_cache) {
        bool hasNew = false;
        for (const auto& [k, v] : secMap) {
            if (!inRaw[sec].count(k)) { hasNew = true; break; }
        }
        if (!hasNew) continue;
        output.push_back("");
        output.push_back("[" + sec + "]");
        for (const auto& [k, v] : secMap) {
            if (!inRaw[sec].count(k)) {
                output.push_back(k + " = " + v);
                inRaw[sec].insert(k);
            }
        }
    }

    std::ofstream out(tmpPath, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        JST_LOG_ERROR("Config::Save: cannot open temp file '{}'.", tmpPath.string());
        return false;
    }
    for (const auto& ln : output) {
        out << ln << '\n';
    }
    out.flush();
    if (!out) {
        JST_LOG_ERROR("Config::Save: write error on '{}'.", tmpPath.string());
        return false;
    }
    out.close();
    if (!out) {
        JST_LOG_ERROR("Config::Save: close error on '{}'.", tmpPath.string());
        return false;
    }
    return true;
}

bool Config::SaveDeterministic(const std::filesystem::path& tmpPath) const {
    std::ofstream out(tmpPath, std::ios::out | std::ios::trunc);
    if (!out.is_open()) {
        JST_LOG_ERROR("Config::Save: cannot open temp file '{}'.", tmpPath.string());
        return false;
    }
    for (const auto& [sec, secMap] : m_cache) {
        out << '[' << sec << ']' << '\n';
        for (const auto& [k, v] : secMap) {
            out << k << " = " << v << '\n';
        }
        out << '\n';
    }
    out.flush();
    if (!out) {
        JST_LOG_ERROR("Config::Save: write error on '{}'.", tmpPath.string());
        return false;
    }
    out.close();
    if (!out) {
        JST_LOG_ERROR("Config::Save: close error on '{}'.", tmpPath.string());
        return false;
    }
    return true;
}

} // namespace jst::core
