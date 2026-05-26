#pragma once

#include <filesystem>
#include <flat_map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jst::core {

/**
 * Lightweight INI parser/cache with two save policies.
 *
 *   - Sections and keys are stored in `flat_map`s with transparent comparators
 *     so callers may query with `string_view` without allocating.
 *   - String values are returned by value; integers/floats/bools are parsed on
 *     demand via `from_chars`.
 *   - `GetSection` returns a borrowed pointer (or nullptr) to avoid copying the
 *     full sub-map when callers only need to enumerate.
 *
 * `SaveMode` is fixed at `Load` time:
 *   - `PreserveComments` (default) — `Save()` preserves comments, blank lines,
 *     and original key ordering by replaying `m_rawLines` captured at Load.
 *     Suitable for human-edited files (ASI variant).
 *   - `Deterministic` — `Save()` writes the cache straight out without any
 *     raw-line replay. Faster, comment-free; suitable for machine-managed
 *     files (ReShade addon variant, autosave hot path).
 */
class Config final {
public:
    using Section = std::flat_map<std::string, std::string, std::less<>>;

    enum class SaveMode { PreserveComments, Deterministic };

    Config() = default;
    ~Config() = default;

    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    Config(Config&&) = delete;
    Config& operator=(Config&&) = delete;

    [[nodiscard]] bool Load(const std::filesystem::path& path, SaveMode mode = SaveMode::PreserveComments);
    [[nodiscard]] bool Reload();

    [[nodiscard]] std::string GetString(std::string_view section, std::string_view key, std::string_view defaultValue) const;
    [[nodiscard]] int         GetInt(std::string_view section, std::string_view key, int defaultValue) const;
    [[nodiscard]] float       GetFloat(std::string_view section, std::string_view key, float defaultValue) const;
    [[nodiscard]] bool        GetBool(std::string_view section, std::string_view key, bool defaultValue) const;

    [[nodiscard]] bool HasSection(std::string_view section) const;
    [[nodiscard]] const Section* GetSection(std::string_view section) const;

    // For seeding / cross-config copying.
    [[nodiscard]] const auto& GetCache() const { return m_cache; }

    [[nodiscard]] const std::filesystem::path& GetPath() const { return m_path; }

    // Mutators: write to m_cache. Call Save() to persist to disk.
    void SetString(std::string_view section, std::string_view key, std::string value);
    void SetFloat (std::string_view section, std::string_view key, float value);
    void SetInt   (std::string_view section, std::string_view key, int value);
    void SetBool  (std::string_view section, std::string_view key, bool value);

    /// Write m_cache back to GetPath() atomically (temp file + rename). The
    /// strategy is fixed at Load time via SaveMode:
    ///   - `PreserveComments` replays m_rawLines, substituting cached values
    ///     and appending keys/sections not present in the original file.
    ///   - `Deterministic` ignores m_rawLines and writes the cache directly,
    ///     suitable for files the runtime owns end-to-end.
    /// Returns false on I/O error (and cleans up the orphan temp file).
    [[nodiscard]] bool Save();

    [[nodiscard]] SaveMode GetSaveMode() const noexcept { return m_saveMode; }

private:
    [[nodiscard]] std::optional<std::string_view> GetRawOpt(std::string_view section, std::string_view key) const;

    [[nodiscard]] bool SavePreserveComments(const std::filesystem::path& tmpPath) const;
    [[nodiscard]] bool SaveDeterministic(const std::filesystem::path& tmpPath) const;

    std::filesystem::path m_path;
    std::flat_map<std::string, Section, std::less<>> m_cache;
    std::vector<std::string> m_rawLines;    // populated only when m_saveMode == PreserveComments
    SaveMode m_saveMode = SaveMode::PreserveComments;
};

} // namespace jst::core
