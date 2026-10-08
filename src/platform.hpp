#pragma once
#include "core.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <filesystem>
#include <windows.h>

namespace evening {
struct WindowsCalendar : Calendar {
    // Optional injected zone for deterministic tests; the app uses the current Windows zone.
    std::optional<DYNAMIC_TIME_ZONE_INFORMATION> zoneOverride;
    WindowsCalendar() = default;
    explicit WindowsCalendar(const DYNAMIC_TIME_ZONE_INFORMATION &zone) : zoneOverride(zone) {}
    DYNAMIC_TIME_ZONE_INFORMATION zone() const;
    std::optional<SYSTEMTIME> localSystemTime(Time utc) const;
    LocalStamp local(Time utc) const override;
    std::optional<Time> resolve(int day, int minute) const override;
};
Snapshot snapshot();
UINT windowDpi(HWND window);
UINT systemDpi();
void configureDpi();
std::wstring wide(const std::string &utf8);
std::string utf8(const std::wstring &text);
std::string newId();
std::wstring formatTime(Time time, bool date = false);
std::wstring describe(const Task &task);
std::wstring describeStatus(Status status);
std::filesystem::path defaultDataDirectory();
struct LoadResult {
    State state;
    std::wstring warning;
    bool recovered = false;
};
struct Storage {
    std::filesystem::path directory;
    explicit Storage(std::filesystem::path path) : directory(std::move(path)) {}
    LoadResult load() const;
    bool save(const State &state, std::wstring &error) const;
};
struct PowerResult {
    bool accepted = false;
    std::wstring message;
};
PowerResult requestShutdown(bool force, bool simulate);
bool setAutostart(bool enabled, std::wstring &error);
bool hasAutostart();
std::wstring executablePath();
} // namespace evening
