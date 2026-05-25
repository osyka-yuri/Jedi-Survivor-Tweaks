#pragma once

#include <filesystem>
#include <flat_map>
#include <optional>
#include <string>
#include <string_view>

namespace jst::core {

/**
 * Lightweight INI parser/cache.
 *
 *   - Sections and keys are stored in `flat_map`s with transparent comparators
 *     so callers may query with `string_view` without allocating.
 *   - String values are returned by value; integers/floats/bools are parsed on
 *     demand via `from_chars`.
 *   - `GetSection` returns a borrowed pointer (or nullptr) to avoid copying the
 *     full sub-map when callers only need to enumerate.
 */
class Config final {
public:
    using Section = std::flat_map<std::string, std::string, std::less<>>;

    Config() = default;
    ~Config() = default;

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    Config(Config&&) = delete;
    Config& operator=(Config&&) = delete;

    [[nodiscard]] bool Load(const std::filesystem::path& path);
    [[nodiscard]] bool Reload();

    [[nodiscard]] std::string GetString(std::string_view section, std::string_view key, std::string_view defaultValue) const;
    [[nodiscard]] int         GetInt(std::string_view section, std::string_view key, int defaultValue) const;
    [[nodiscard]] float       GetFloat(std::string_view section, std::string_view key, float defaultValue) const;
    [[nodiscard]] bool        GetBool(std::string_view section, std::string_view key, bool defaultValue) const;

    [[nodiscard]] bool HasSection(std::string_view section) const;
    [[nodiscard]] const Section* GetSection(std::string_view section) const;

    [[nodiscard]] const std::filesystem::path& GetPath() const { return m_path; }

private:
    [[nodiscard]] std::optional<std::string_view> GetRawOpt(std::string_view section, std::string_view key) const;

    std::filesystem::path m_path;
    std::flat_map<std::string, Section, std::less<>> m_cache;
};

} // namespace jst::core
