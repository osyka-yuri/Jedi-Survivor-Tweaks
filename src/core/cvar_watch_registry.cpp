#include "cvar_watch_registry.hpp"

#include "logging.hpp"
#include "string_utils.hpp"

#include <exception>
#include <utility>
#include <vector>

namespace jst::core {

namespace {

void LogWatchException(uint64_t id, std::string_view phase, const std::exception& error) {
    JST_LOG_ERROR("CVar watch #{} {} threw: {}. Watch cancelled.",
                  id, phase, error.what());
}

void LogUnknownWatchException(uint64_t id, std::string_view phase) {
    JST_LOG_ERROR("CVar watch #{} {} threw an unknown exception. Watch cancelled.",
                  id, phase);
}

} // namespace

uint64_t CVarWatchRegistry::Register(IntWatchRequest request) {
    std::lock_guard lock(m_mutex);
    uint64_t id = m_nextId++;
    if (id == 0) {
        id = m_nextId++;
    }

    auto entry = std::make_shared<WatchEntry>();
    entry->id = id;
    entry->deadline = std::chrono::steady_clock::now() + request.timeout;
    entry->request = std::move(request);
    m_entries.emplace(id, std::move(entry));
    return id;
}

void CVarWatchRegistry::Cancel(uint64_t id) {
    if (id == 0) {
        return;
    }

    std::unique_lock lock(m_mutex);
    const auto found = m_entries.find(id);
    if (found == m_entries.end()) {
        return;
    }

    const auto entry = found->second;
    entry->active = false;
    m_entries.erase(found);
    m_cv.wait(lock, [&] { return entry->inFlight == 0; });
}

void CVarWatchRegistry::Clear() {
    std::unique_lock lock(m_mutex);
    std::vector<std::shared_ptr<WatchEntry>> entries;
    entries.reserve(m_entries.size());
    for (auto& [id, entry] : m_entries) {
        (void)id;
        entry->active = false;
        entries.push_back(entry);
    }
    m_entries.clear();
    m_cv.wait(lock, [&] {
        for (const auto& entry : entries) {
            if (entry->inFlight != 0) {
                return false;
            }
        }
        return true;
    });
}

bool CVarWatchRegistry::HasAny() const {
    std::lock_guard lock(m_mutex);
    return !m_entries.empty();
}

bool CVarWatchRegistry::HasFor(std::wstring_view name) const {
    std::lock_guard lock(m_mutex);
    for (const auto& [id, entry] : m_entries) {
        (void)id;
        if (entry->active && entry->request.name == name) {
            return true;
        }
    }
    return false;
}

void CVarWatchRegistry::Evaluate(const ValueReader& readValue) {
    std::vector<std::shared_ptr<WatchEntry>> entries;
    {
        std::lock_guard lock(m_mutex);
        entries.reserve(m_entries.size());
        for (const auto& [id, entry] : m_entries) {
            (void)id;
            if (!entry->active) {
                continue;
            }
            entries.push_back(entry);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    for (const auto& entry : entries) {
        {
            std::lock_guard lock(m_mutex);
            if (!entry->active) {
                continue;
            }
            ++entry->inFlight;
        }

        bool complete = false;
        bool timedOut = false;

        try {
            complete = entry->request.shouldAbort && entry->request.shouldAbort();
        } catch (const std::exception& error) {
            LogWatchException(entry->id, "abort predicate", error);
            complete = true;
        } catch (...) {
            LogUnknownWatchException(entry->id, "abort predicate");
            complete = true;
        }

        if (!complete && now >= entry->deadline) {
            complete = true;
            timedOut = true;
            try {
                if (entry->request.onTimeout) {
                    entry->request.onTimeout();
                }
            } catch (const std::exception& error) {
                LogWatchException(entry->id, "timeout callback", error);
            } catch (...) {
                LogUnknownWatchException(entry->id, "timeout callback");
            }
        }

        if (!complete) {
            const auto value = readValue(entry->request.name);
            if (value) {
                try {
                    complete = entry->request.onValue(*value) == CVarWatchDecision::Complete;
                } catch (const std::exception& error) {
                    LogWatchException(entry->id, "value callback", error);
                    complete = true;
                } catch (...) {
                    LogUnknownWatchException(entry->id, "value callback");
                    complete = true;
                }
            }
        }

        {
            std::lock_guard lock(m_mutex);
            if (complete) {
                const auto found = m_entries.find(entry->id);
                if (found != m_entries.end() && found->second == entry) {
                    entry->active = false;
                    m_entries.erase(found);
                }
            }
            --entry->inFlight;
            m_cv.notify_all();
        }

        if (timedOut) {
            JST_LOG_DEBUG("CVar watch #{} timed out for '{}'.",
                          entry->id, utils::WideToUtf8(entry->request.name));
        }
    }
}

} // namespace jst::core
