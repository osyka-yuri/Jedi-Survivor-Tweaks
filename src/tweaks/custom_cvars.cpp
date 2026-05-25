#include "custom_cvars.hpp"
#include "core/cvar_system.hpp"
#include "core/config.hpp"
#include "core/logging.hpp"
#include "core/string_utils.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <ranges>
#include <vector>

namespace jst::tweaks {

namespace {
    bool IsTrueLiteral(std::string_view sv) {
        constexpr std::string_view target = "true";
        if (sv.size() != target.size()) return false;
        return std::ranges::equal(sv, target, [](unsigned char a, unsigned char b) {
            return std::tolower(a) == b;
        });
    }
    bool IsFalseLiteral(std::string_view sv) {
        constexpr std::string_view target = "false";
        if (sv.size() != target.size()) return false;
        return std::ranges::equal(sv, target, [](unsigned char a, unsigned char b) {
            return std::tolower(a) == b;
        });
    }
} // anonymous namespace

std::expected<void, std::string> CustomCVarsTweak::Initialize(
    [[maybe_unused]] jst::core::HookEngine& hooks, jst::core::Config& config) {
    auto& cvarSys = jst::core::CVarSystem::Instance();

    const auto* section = config.GetSection("CVars");
    if (!section) {
        m_initialized = true;
        JST_LOG_INFO("No [CVars] section found.");
        return {};
    }

    // Build a list of (wide-name, raw-value) entries up-front so we can batch
    // the .text scan once across all custom cvars.
    struct Entry {
        std::wstring name;
        std::string_view value;
    };
    std::vector<Entry> entries;
    entries.reserve(section->size());
    for (const auto& [name, valStr] : *section) {
        if (name == "Enabled") continue;
        entries.push_back({jst::core::utils::Utf8ToWide(name), valStr});
    }

    if (!entries.empty()) {
        std::vector<std::wstring_view> names;
        names.reserve(entries.size());
        for (const auto& e : entries) names.emplace_back(e.name);
        cvarSys.ResolveBatch(names);
    }

    size_t applied = 0;
    for (const auto& e : entries) {
        const std::string_view v = e.value;
        bool ok = false;

        if (IsTrueLiteral(v))            ok = cvarSys.SetInt(e.name, 1);
        else if (IsFalseLiteral(v))      ok = cvarSys.SetInt(e.name, 0);
        else if (v.find('.') != std::string_view::npos) {
            float fval = 0.0f;
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), fval);
            if (ec == std::errc{}) {
                ok = cvarSys.SetFloat(e.name, fval);
            } else {
                JST_LOG_ERROR("Failed to parse float CVar '{}': '{}'.",
                              jst::core::utils::WideToUtf8(e.name), v);
            }
        } else {
            int32_t ival = 0;
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), ival);
            if (ec == std::errc{}) {
                ok = cvarSys.SetInt(e.name, ival);
            } else {
                JST_LOG_ERROR("Failed to parse integer CVar '{}': '{}'.",
                              jst::core::utils::WideToUtf8(e.name), v);
            }
        }
        if (ok) ++applied;
    }

    m_initialized = true;
    JST_LOG_INFO("Initialized: {} of {} custom CVars accepted.",
                 applied, entries.size());
    return {};
}

void CustomCVarsTweak::Shutdown() {
    m_initialized = false;
}

} // namespace jst::tweaks
