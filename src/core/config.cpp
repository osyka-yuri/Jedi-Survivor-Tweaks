#include "config.hpp"
#include "logging.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <ranges>
#include <string_view>

namespace jst::core {

namespace {
    constexpr std::array<std::string_view, 6> kFalseLiterals{
        "0", "false", "False", "FALSE", "no", "No"
    };
    constexpr std::array<std::string_view, 6> kTrueLiterals{
        "1", "true", "True", "TRUE", "yes", "Yes"
    };

    template <std::ranges::input_range R>
    constexpr bool MatchesAny(std::string_view sv, const R& set) {
        return std::ranges::find(set, sv) != std::ranges::end(set);
    }

    constexpr std::string_view Trim(std::string_view sv) {
        const auto start = sv.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) return {};
        const auto end = sv.find_last_not_of(" \t\r\n");
        return sv.substr(start, end - start + 1);
    }
} // anonymous namespace

bool Config::Load(const std::filesystem::path& path) {
    m_path = path;
    m_cache.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        JST_LOG_WARNING("Failed to open config file: '{}'.", path.string());
        return false;
    }

    std::string currentSection;
    std::string line;
    while (std::getline(file, line)) {
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
    const bool res = Load(m_path);
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
    if (MatchesAny(*raw, kFalseLiterals)) return false;
    if (MatchesAny(*raw, kTrueLiterals))  return true;
    return defaultValue;
}

bool Config::HasSection(std::string_view section) const {
    return m_cache.find(section) != m_cache.end();
}

const Config::Section* Config::GetSection(std::string_view section) const {
    const auto it = m_cache.find(section);
    return it == m_cache.end() ? nullptr : &it->second;
}

} // namespace jst::core
