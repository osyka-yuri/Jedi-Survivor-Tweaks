#include "cvar_watch_registry.hpp"

#include "cvar_name.hpp"
#include "logging.hpp"
#include "string_utils.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <utility>
#include <vector>

namespace jst::core {

struct CVarWatchRegistryState {
    mutable std::mutex mutex;
#if defined(JST_UNIT_TESTS)
    mutable std::condition_variable entriesCv;
#endif
    // Registration order makes callback behavior deterministic and lets one
    // callback safely cancel a later subscription before it starts. Weak
    // ownership ensures the index never extends a subscription's lifetime.
    std::map<uint64_t, std::weak_ptr<CVarWatchControl>> entries;
    uint64_t nextId = 1;
};

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

void CVarWatchControl::CancelAndWait() {
    Cancel();
    Wait();
    DetachFromRegistry();
}

void CVarWatchControl::Cancel() {
    std::lock_guard lock(mutex);
    active = false;
}

void CVarWatchControl::Wait() {
    std::unique_lock lock(mutex);
    cv.wait(lock, [this] { return inFlight == 0; });
}

void CVarWatchControl::DetachFromRegistry() {
    const auto state = registryState.lock();
    if (!state) {
        return;
    }

    std::lock_guard lock(state->mutex);
    const auto found = state->entries.find(id);
    if (found == state->entries.end()) {
        return;
    }
    const auto indexed = found->second.lock();
    if (!indexed || indexed.get() == this) {
        state->entries.erase(found);
#if defined(JST_UNIT_TESTS)
        state->entriesCv.notify_all();
#endif
    }
}

bool CVarWatchControl::IsActive() const {
    std::lock_guard lock(mutex);
    return active;
}

CVarWatchRegistry::CVarWatchRegistry()
    : m_state(std::make_shared<CVarWatchRegistryState>()) {}

std::shared_ptr<CVarWatchControl> CVarWatchRegistry::Register(
    IntWatchRequest request) {
    const auto state = m_state;
    std::lock_guard lock(state->mutex);
    uint64_t id = state->nextId++;
    if (id == 0) {
        id = state->nextId++;
    }

    auto entry = std::make_shared<CVarWatchControl>();
    entry->id = id;
    entry->deadline = std::chrono::steady_clock::now() + request.timeout;
    entry->request = std::move(request);
    entry->registryState = state;
    state->entries.emplace(id, entry);
    return entry;
}

void CVarWatchRegistry::Clear() {
    std::vector<std::shared_ptr<CVarWatchControl>> entries;
    const auto state = m_state;
    {
        std::lock_guard lock(state->mutex);
        entries.reserve(state->entries.size());
        for (auto& [id, entry] : state->entries) {
            (void)id;
            if (auto retained = entry.lock()) {
                entries.push_back(std::move(retained));
            }
        }
        state->entries.clear();
#if defined(JST_UNIT_TESTS)
        state->entriesCv.notify_all();
#endif
    }
    // Close admission for the complete snapshot before waiting for any one
    // callback. This prevents a later entry from starting while Clear is
    // blocked behind an earlier callback.
    for (const auto& entry : entries) {
        entry->Cancel();
    }
    for (const auto& entry : entries) {
        entry->Wait();
    }
}

void CVarWatchRegistry::OpenStartupBarrier(
    std::chrono::steady_clock::time_point now) {
    std::vector<std::shared_ptr<CVarWatchControl>> entries;
    const auto state = m_state;
    {
        std::lock_guard lock(state->mutex);
        for (const auto& [id, entry] : state->entries) {
            (void)id;
            if (auto retained = entry.lock()) {
                entries.push_back(std::move(retained));
            }
        }
    }
    for (const auto& entry : entries) {
        std::lock_guard lock(entry->mutex);
        if (entry->active) {
            entry->deadline = now + entry->request.timeout;
        }
    }
}

bool CVarWatchRegistry::HasFor(std::wstring_view name) const {
    std::vector<std::shared_ptr<CVarWatchControl>> entries;
    const auto state = m_state;
    {
        std::lock_guard lock(state->mutex);
        for (const auto& [id, entry] : state->entries) {
            (void)id;
            if (auto retained = entry.lock()) {
                entries.push_back(std::move(retained));
            }
        }
    }
    const CVarNameEqual equal;
    for (const auto& entry : entries) {
        std::lock_guard lock(entry->mutex);
        if (entry->active && equal(entry->request.name, name)) {
            return true;
        }
    }
    return false;
}

#if defined(JST_UNIT_TESTS)
bool CVarWatchRegistry::WaitUntilAbsent(
    std::wstring_view name,
    std::chrono::milliseconds timeout) const {
    const CVarNameEqual equal;
    const auto state = m_state;
    std::unique_lock lock(state->mutex);
    return state->entriesCv.wait_for(lock, timeout, [&] {
        for (const auto& [id, weakEntry] : state->entries) {
            (void)id;
            const auto entry = weakEntry.lock();
            if (!entry) {
                continue;
            }
            std::lock_guard entryLock(entry->mutex);
            if (entry->active && equal(entry->request.name, name)) {
                return false;
            }
        }
        return true;
    });
}

size_t CVarWatchRegistry::EntryCount() const {
    const auto state = m_state;
    std::lock_guard lock(state->mutex);
    return static_cast<size_t>(std::count_if(
        state->entries.begin(),
        state->entries.end(),
        [](const auto& item) { return !item.second.expired(); }));
}
#endif

void CVarWatchRegistry::Evaluate(const ValueReader& readValue) {
    std::vector<std::shared_ptr<CVarWatchControl>> entries;
    const auto state = m_state;
    {
        std::lock_guard lock(state->mutex);
        entries.reserve(state->entries.size());
        for (const auto& [id, entry] : state->entries) {
            (void)id;
            if (auto retained = entry.lock()) {
                entries.push_back(std::move(retained));
            }
        }
    }

    const auto now = std::chrono::steady_clock::now();
    for (const auto& entry : entries) {
        std::chrono::steady_clock::time_point deadline;
        {
            std::lock_guard lock(entry->mutex);
            if (!entry->active) {
                continue;
            }
            ++entry->inFlight;
            deadline = entry->deadline;
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

        if (!complete && now >= deadline) {
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
            std::lock_guard lock(entry->mutex);
            if (complete) {
                entry->active = false;
            }
            --entry->inFlight;
            entry->cv.notify_all();
        }

        if (timedOut) {
            JST_LOG_DEBUG("CVar watch #{} timed out for '{}'.",
                          entry->id, utils::WideToUtf8(entry->request.name));
        }
    }

    std::lock_guard lock(state->mutex);
    const auto removed = std::erase_if(state->entries, [](const auto& item) {
        const auto entry = item.second.lock();
        return !entry || !entry->IsActive();
    });
#if defined(JST_UNIT_TESTS)
    if (removed != 0) {
        state->entriesCv.notify_all();
    }
#else
    (void)removed;
#endif
}

} // namespace jst::core
