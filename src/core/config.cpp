#include "config.hpp"
#include "ini_helpers.hpp"
#include "logging.hpp"

#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <set>

namespace jst::core {

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
    int value = 0;
    auto [ptr, ec] = std::from_chars(raw->data(), raw->data() + raw->size(), value);
    return ec == std::errc{} ? value : defaultValue;
}

float Config::GetFloat(std::string_view section, std::string_view key, float defaultValue) const {
    const auto raw = GetRawOpt(section, key);
    if (!raw) return defaultValue;
    float value = 0.0f;
    auto [ptr, ec] = std::from_chars(raw->data(), raw->data() + raw->size(), value);
    return ec == std::errc{} ? value : defaultValue;
}

bool Config::GetBool(std::string_view section, std::string_view key, bool defaultValue) const {
    const auto raw = GetRawOpt(section, key);
    if (!raw) return defaultValue;
    if (detail::MatchesAny(*raw, detail::kFalseLiterals)) return false;
    if (detail::MatchesAny(*raw, detail::kTrueLiterals))  return true;
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

    const auto tmpPath = m_path.parent_path() / (m_path.filename().string() + ".tmp");
    const bool wrote = (m_saveMode == SaveMode::PreserveComments)
                           ? SavePreserveComments(tmpPath)
                           : SaveDeterministic(tmpPath);
    if (!wrote) {
        std::error_code rmEc;
        std::filesystem::remove(tmpPath, rmEc);  // best-effort orphan cleanup
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(tmpPath, m_path, ec);
    if (ec) {
        JST_LOG_ERROR("Config::Save: rename failed: {}.", ec.message());
        std::error_code rmEc;
        std::filesystem::remove(tmpPath, rmEc);
        return false;
    }
    JST_LOG_INFO("Config saved: '{}'.", m_path.string());
    return true;
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
    if (!out) {
        JST_LOG_ERROR("Config::Save: write error on '{}'.", tmpPath.string());
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
    if (!out) {
        JST_LOG_ERROR("Config::Save: write error on '{}'.", tmpPath.string());
        return false;
    }
    return true;
}

} // namespace jst::core
