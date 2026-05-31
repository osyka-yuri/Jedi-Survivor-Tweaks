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

    // Build a list of (wide-name, raw-value) entries from the [CVars] config section.
    struct Entry {
        std::wstring name;
        std::string_view value;
    };
    std::vector<Entry> entries;
    entries.reserve(section->size());
    for (const auto& [name, valStr] : *section) {
        if (name == "Enabled") continue;
        if (name.size() < 2) {
            JST_LOG_WARNING("Skipping invalid CVar name '{}'", name);
            continue;
        }
        entries.push_back({jst::core::utils::Utf8ToWide(name), valStr});
    }

    size_t applied = 0;
    size_t queued = 0;
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
                continue;
            }
        } else {
            int32_t ival = 0;
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), ival);
            if (ec == std::errc{}) {
                ok = cvarSys.SetInt(e.name, ival);
            } else {
                JST_LOG_ERROR("Failed to parse integer CVar '{}': '{}'.",
                              jst::core::utils::WideToUtf8(e.name), v);
                continue;
            }
        }
        if (ok) ++applied;
        else ++queued;
    }

    JST_LOG_INFO("CustomCVars: {} applied synchronously, {} queued for async initialization.", applied, queued);

    m_initialized = true;
    return {};
}

void CustomCVarsTweak::Shutdown() {
    m_initialized = false;
}

} // namespace jst::tweaks
