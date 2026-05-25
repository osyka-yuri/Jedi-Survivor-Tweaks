#pragma once

#include "logging.hpp"

#include <string>
#include <string_view>
#include <windows.h>

namespace jst::core::utils {

    /// Convert a wide (UTF-16) string to UTF-8.
    /// Returns an empty string on failure; logs an ERROR explaining what went wrong.
    [[nodiscard]] inline std::string WideToUtf8(std::wstring_view wstr) {
        if (wstr.empty()) return {};
        const int sizeNeeded = WideCharToMultiByte(
            CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
            nullptr, 0, nullptr, nullptr);
        if (sizeNeeded <= 0) {
            JST_LOG_ERROR("WideCharToMultiByte sizing failed (size={}, err={}).",
                          wstr.size(), GetLastError());
            return {};
        }
        std::string str(static_cast<size_t>(sizeNeeded), '\0');
        const int written = WideCharToMultiByte(
            CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()),
            str.data(), sizeNeeded, nullptr, nullptr);
        if (written != sizeNeeded) {
            JST_LOG_ERROR("WideCharToMultiByte conversion failed (err={}).", GetLastError());
            return {};
        }
        return str;
    }

    /// Convert a UTF-8 string to wide (UTF-16).
    /// Returns an empty wstring on failure; logs an ERROR explaining what went wrong.
    [[nodiscard]] inline std::wstring Utf8ToWide(std::string_view str) {
        if (str.empty()) return {};
        const int sizeNeeded = MultiByteToWideChar(
            CP_UTF8, 0, str.data(), static_cast<int>(str.size()), nullptr, 0);
        if (sizeNeeded <= 0) {
            JST_LOG_ERROR("MultiByteToWideChar sizing failed (size={}, err={}).",
                          str.size(), GetLastError());
            return {};
        }
        std::wstring wstr(static_cast<size_t>(sizeNeeded), L'\0');
        const int written = MultiByteToWideChar(
            CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
            wstr.data(), sizeNeeded);
        if (written != sizeNeeded) {
            JST_LOG_ERROR("MultiByteToWideChar conversion failed (err={}).", GetLastError());
            return {};
        }
        return wstr;
    }

} // namespace jst::core::utils
