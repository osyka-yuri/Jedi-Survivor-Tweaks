#pragma once

#include <windows.h>
#include <tlhelp32.h>

#include <cstddef>
#include <limits>
#include <vector>

namespace jst::core {

class ThreadSuspender final {
public:
    ThreadSuspender() {
        const DWORD currentThreadId = GetCurrentThreadId();
        const DWORD currentProcessId = GetCurrentProcessId();

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return;
        }

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != currentProcessId ||
                    entry.th32ThreadID == currentThreadId) {
                    entry.dwSize = sizeof(entry);
                    continue;
                }

                HANDLE thread =
                    OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
                if (thread && SuspendThread(thread) != std::numeric_limits<DWORD>::max()) {
                    m_threads.push_back(thread);
                } else if (thread) {
                    CloseHandle(thread);
                }
                entry.dwSize = sizeof(entry);
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    ~ThreadSuspender() {
        for (HANDLE thread : m_threads) {
            ResumeThread(thread);
            CloseHandle(thread);
        }
    }

    ThreadSuspender(const ThreadSuspender&) = delete;
    ThreadSuspender& operator=(const ThreadSuspender&) = delete;

    [[nodiscard]] size_t SuspendedCount() const noexcept { return m_threads.size(); }

private:
    std::vector<HANDLE> m_threads;
};

} // namespace jst::core