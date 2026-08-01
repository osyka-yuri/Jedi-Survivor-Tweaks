#include "core/config.hpp"
#include "test_check.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

size_t CountSaveArtifacts(const std::filesystem::path& path) {
    const std::wstring prefix = path.filename().wstring() + L".";
    size_t count = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(path.parent_path())) {
        const auto name = entry.path().filename().wstring();
        if (name.starts_with(prefix) &&
            (name.ends_with(L".tmp") || name.ends_with(L".tmp.backup"))) {
            ++count;
        }
    }
    return count;
}

} // namespace

void TestConfig() {
    wchar_t tempRoot[MAX_PATH]{};
    const DWORD rootLength = GetTempPathW(MAX_PATH, tempRoot);
    Check(rootLength != 0 && rootLength < MAX_PATH,
          "temporary root is available for config tests");
    const auto directory = std::filesystem::path(tempRoot) /
        (L"JST.ConfigTests." + std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(directory);
    const auto path = directory / L"JediSurvivorTweaks.ini";

    const auto newPath = directory / L"NewConfig.ini";
    jst::core::Config newConfig;
    Check(!newConfig.Load(
              newPath, jst::core::Config::SaveMode::Deterministic),
          "missing config initializes a writable target without fake content");
    newConfig.SetInt("Values", "Integer", 5);
    Check(newConfig.Save() &&
              ReadText(newPath).find("Integer = 5") != std::string::npos &&
              CountSaveArtifacts(newPath) == 0,
          "first save atomically publishes a flushed sibling temp");

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "[Values]\n"
                  "Integer = 12junk\n"
                  "Float = 1.5oops\n"
                  "NonFinite = nan\n"
                  "Exponent = 1e2\n"
                  "Range = 301\n"
                  "Boolean = perhaps\n";
    }

    jst::core::Config config;
    Check(config.Load(path, jst::core::Config::SaveMode::Deterministic),
          "config test fixture loads");
    Check(config.GetInt("Values", "Integer", 7) == 7,
          "integer parsing rejects a partially consumed value");
    Check(config.GetFloat("Values", "Float", 2.0f) == 2.0f,
          "float parsing rejects a partially consumed value");
    Check(config.GetFloat("Values", "NonFinite", 3.0f) == 3.0f,
          "float parsing rejects non-finite input");
    Check(config.GetFloat("Values", "Exponent", 0.0f) == 100.0f,
          "float parsing accepts fully consumed exponent notation");
    Check(config.GetFloatInRange(
              "Values", "Range", 60.0f, 0.0f, 300.0f) == 60.0f,
          "range validation uses the documented fallback");
    Check(config.GetBool("Values", "Boolean", true),
          "invalid boolean uses its explicit fallback");

    const auto staleLegacyTemp = path.parent_path() /
        (path.filename().wstring() + L"." +
         std::to_wstring(GetCurrentProcessId()) + L".tmp");
    {
        std::ofstream stale(staleLegacyTemp, std::ios::binary);
        stale << "do not overwrite";
    }
    config.SetInt("Values", "Integer", 42);
    Check(config.Save(), "config saves through sibling temp and atomic replace");
    const std::string saved = ReadText(path);
    Check(saved.find("Integer = 42") != std::string::npos,
          "atomic save publishes the complete updated file");
    Check(ReadText(staleLegacyTemp) == "do not overwrite",
          "unique temp allocation never truncates a stale sibling file");
    std::filesystem::remove(staleLegacyTemp);
    Check(CountSaveArtifacts(path) == 0,
          "successful replacement removes its backup and temp artifacts");

    // Deny FILE_SHARE_DELETE so ReplaceFileW must fail. The original target
    // remains byte-for-byte intact and the sibling temp is cleaned up.
    const std::string beforeFailure = ReadText(path);
    const HANDLE lock = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    Check(lock != INVALID_HANDLE_VALUE,
          "config target lock is established for failure-path test");
    config.SetInt("Values", "Integer", 99);
    Check(!config.Save(), "atomic replacement reports sharing failure");
    Check(CountSaveArtifacts(path) == 0,
          "classified replacement failure removes its temporary artifacts");
    if (lock != INVALID_HANDLE_VALUE) {
        CloseHandle(lock);
    }
    Check(ReadText(path) == beforeFailure,
          "failed replacement preserves the original config file");

    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
}
