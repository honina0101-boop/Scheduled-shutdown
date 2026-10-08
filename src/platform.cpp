#include "platform.hpp"
#include "../third_party/json.hpp"
#include <bcrypt.h>
#include <ctime>
#include <fstream>
#include <set>
#include <shlobj.h>
#include <sstream>
#include <stdexcept>

namespace evening {
using json = nlohmann::json;
std::wstring wide(const std::string &s) {
    if (s.empty())
        return {};
    int n =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n)
        return L"";
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    int n =
        WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
// Resolve newer DPI APIs at runtime so earlier Windows 10 releases can also start.
UINT systemDpi() {
    using Fn = UINT(WINAPI *)();
    static auto query =
        reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
    if (query)
        return query();
    HDC dc = GetDC(nullptr);
    UINT dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc)
        ReleaseDC(nullptr, dc);
    return dpi ? dpi : 96;
}
UINT windowDpi(HWND window) {
    using Fn = UINT(WINAPI *)(HWND);
    static auto query =
        reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (query && window) {
        UINT dpi = query(window);
        if (dpi)
            return dpi;
    }
    return systemDpi();
}
void configureDpi() {
    using Fn = BOOL(WINAPI *)(HANDLE);
    static auto set = reinterpret_cast<Fn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set)
        set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    else
        SetProcessDPIAware();
}
Snapshot snapshot() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER n;
    n.LowPart = ft.dwLowDateTime;
    n.HighPart = ft.dwHighDateTime;
    return {static_cast<Time>(n.QuadPart / 10000ULL) - 11644473600000LL, static_cast<Time>(GetTickCount64())};
}
DYNAMIC_TIME_ZONE_INFORMATION WindowsCalendar::zone() const {
    if (zoneOverride)
        return *zoneOverride;
    DYNAMIC_TIME_ZONE_INFORMATION result{};
    GetDynamicTimeZoneInformation(&result);
    return result;
}
std::optional<SYSTEMTIME> WindowsCalendar::localSystemTime(Time utc) const {
    if (utc < -11644473600000LL || utc > 253402300799999LL)
        return {};
    ULARGE_INTEGER raw;
    raw.QuadPart = static_cast<uint64_t>(utc + 11644473600000LL) * 10000ULL;
    FILETIME file{raw.LowPart, raw.HighPart};
    SYSTEMTIME universal{}, result{};
    auto information = zone();
    if (!FileTimeToSystemTime(&file, &universal) ||
        !SystemTimeToTzSpecificLocalTimeEx(&information, &universal, &result))
        return {};
    return result;
}
LocalStamp WindowsCalendar::local(Time utc) const {
    auto value = localSystemTime(utc);
    if (!value)
        return {};
    return {civilDay(value->wYear, value->wMonth, value->wDay), value->wHour * 60 + value->wMinute,
            (value->wDayOfWeek + 6) % 7};
}
std::optional<Time> WindowsCalendar::resolve(int day, int minute) const {
    using namespace std::chrono;
    year_month_day date{sys_days{days{day}}};
    if (int(date.year()) < 1601 || int(date.year()) > 9999 || minute < 0 || minute >= 1440)
        return {};
    SYSTEMTIME civil{};
    civil.wYear = int(date.year());
    civil.wMonth = unsigned(date.month());
    civil.wDay = unsigned(date.day());
    civil.wHour = minute / 60;
    civil.wMinute = minute % 60;
    FILETIME file{};
    if (!SystemTimeToFileTime(&civil, &file))
        return {};
    ULARGE_INTEGER raw;
    raw.LowPart = file.dwLowDateTime;
    raw.HighPart = file.dwHighDateTime;
    Time naive = static_cast<Time>(raw.QuadPart / 10000ULL) - 11644473600000LL;
    auto information = zone();
    TIME_ZONE_INFORMATION rules{};
    if (!GetTimeZoneInformationForYear(civil.wYear, &information, &rules))
        return {};
    std::optional<Time> earliest;
    // Validate both offsets through Windows' forward conversion. A gap has no valid
    // candidate; a repeated hour uses the earlier candidate.
    for (LONG adjustment : {rules.StandardBias, rules.DaylightBias}) {
        Time candidate = naive + (rules.Bias + adjustment) * Minute;
        auto check = localSystemTime(candidate);
        if (check && check->wYear == civil.wYear && check->wMonth == civil.wMonth &&
            check->wDay == civil.wDay && check->wHour == civil.wHour && check->wMinute == civil.wMinute &&
            (!earliest || candidate < *earliest))
            earliest = candidate;
    }
    return earliest;
}
std::string newId() {
    unsigned char random[16]{};
    if (BCryptGenRandom(nullptr, random, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        auto n = snapshot();
        return std::to_string(n.wall) + "-" + std::to_string(n.steady);
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (auto c : random) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
std::wstring formatTime(Time value, bool date) {
    WindowsCalendar calendar;
    auto local = calendar.localSystemTime(value);
    if (!local)
        return L"--:--";
    wchar_t buffer[80];
    if (date)
        swprintf(buffer, 80, L"%04u-%02u-%02u  %02u:%02u", local->wYear, local->wMonth, local->wDay,
                 local->wHour, local->wMinute);
    else
        swprintf(buffer, 80, L"%02u:%02u", local->wHour, local->wMinute);
    return buffer;
}
std::wstring describe(const Task &t) {
    if (t.kind == Kind::Once)
        return L"仅执行一次 · " + formatTime(t.onceDue, true);
    if (t.kind == Kind::Countdown)
        return L"倒计时 · " + std::to_wstring(t.minutes) + L" 分钟";
    std::wstring days;
    if (t.weekdays == 127)
        days = L"每天";
    else if (t.weekdays == 31)
        days = L"工作日";
    else if (t.weekdays == 96)
        days = L"周末";
    else {
        constexpr const wchar_t *names[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
        days = L"周";
        for (int i = 0; i < 7; i++)
            if (t.weekdays & (1 << i)) {
                if (days.size() > 1)
                    days += L"、";
                days += names[i];
            }
    }
    wchar_t time[16];
    swprintf(time, 16, L"%02d:%02d", t.minute / 60, t.minute % 60);
    return days + L" · " + time;
}
std::wstring describeStatus(Status s) {
    switch (s) {
    case Status::Cancelled:
        return L"已取消";
    case Status::Snoozed:
        return L"已推迟";
    case Status::Missed:
        return L"已错过";
    case Status::Requesting:
        return L"结果待确认";
    case Status::Requested:
        return L"已请求关机";
    default:
        return L"请求失败";
    }
}
std::filesystem::path defaultDataDirectory() {
    PWSTR value = nullptr;
    std::filesystem::path path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value))) {
        path = value;
        CoTaskMemFree(value);
    } else {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        path = temp;
    }
    return path / L"Evenfall";
}
json encode(const State &s) {
    json j = {{"schemaVersion", 1},
              {"settings",
               {{"paused", s.settings.paused},
                {"autostart", s.settings.autostart},
                {"sound", s.settings.sound},
                {"force", s.settings.force},
                {"theme", s.settings.theme}}}};
    j["tasks"] = json::array();
    j["pending"] = json::array();
    j["logs"] = json::array();
    for (const auto &t : s.tasks)
        j["tasks"].push_back({{"id", t.id},
                              {"name", t.name},
                              {"revision", t.revision},
                              {"kind", kindName(t.kind)},
                              {"enabled", t.enabled},
                              {"completed", t.completed},
                              {"day", t.day},
                              {"minute", t.minute},
                              {"weekdays", t.weekdays},
                              {"minutes", t.minutes},
                              {"lastHandledDay", t.lastHandledDay},
                              {"onceDue", t.onceDue}});
    for (const auto &p : s.pending)
        j["pending"].push_back({{"key", p.key},
                                {"taskId", p.taskId},
                                {"revision", p.revision},
                                {"day", p.day},
                                {"originalDue", p.originalDue},
                                {"due", p.due},
                                {"detached", p.detached}});
    for (const auto &l : s.logs)
        j["logs"].push_back({{"key", l.key},
                             {"name", l.name},
                             {"detail", l.detail},
                             {"at", l.at},
                             {"due", l.due},
                             {"status", statusName(l.status)}});
    return j;
}
State decode(const json &j) {
    if (j.at("schemaVersion").get<int>() != 1)
        throw std::runtime_error("unsupported schema");
    State s;
    const auto &a = j.at("settings");
    s.settings = {a.at("paused").get<bool>(), a.at("autostart").get<bool>(), a.at("sound").get<bool>(),
                  a.at("force").get<bool>(), a.at("theme").get<std::string>()};
    if (s.settings.theme != "system" && s.settings.theme != "light" && s.settings.theme != "dark")
        throw std::runtime_error("invalid theme");
    const auto &tasks = j.at("tasks");
    const auto &pending = j.at("pending");
    const auto &logs = j.at("logs");
    if (!tasks.is_array() || !pending.is_array() || !logs.is_array() || tasks.size() > 512 ||
        pending.size() > 2048 || logs.size() > 100)
        throw std::runtime_error("invalid array size");
    std::set<std::string> ids, keys;
    for (const auto &x : tasks) {
        Task t;
        t.id = x.at("id");
        t.name = x.at("name");
        t.revision = x.at("revision");
        auto k = x.at("kind").get<std::string>();
        if (k != "once" && k != "weekly" && k != "countdown")
            throw std::runtime_error("invalid task kind");
        t.kind = k == "once" ? Kind::Once : k == "weekly" ? Kind::Weekly : Kind::Countdown;
        t.enabled = x.at("enabled");
        t.completed = x.at("completed");
        t.day = x.at("day");
        t.minute = x.at("minute");
        t.weekdays = x.at("weekdays");
        t.minutes = x.at("minutes");
        t.lastHandledDay = x.at("lastHandledDay");
        t.onceDue = x.at("onceDue");
        if (t.id.empty() || t.id.size() > 80 || !ids.insert(t.id).second || t.name.empty() ||
            wide(t.name).size() > 80 || t.revision < 1 || t.revision > 1000000 ||
            t.lastHandledDay < -2000000 || t.lastHandledDay > 3000000 || t.minute < 0 || t.minute >= 1440 ||
            t.weekdays < 1 || t.weekdays > 127 || t.minutes < 1 || t.minutes > 525600 || t.day < -200000 ||
            t.day > 3000000 || t.onceDue < 0 || t.onceDue > 253402300799999LL)
            throw std::runtime_error("invalid task");
        s.tasks.push_back(t);
    }
    for (const auto &x : pending) {
        Occurrence p;
        p.key = x.at("key");
        p.taskId = x.at("taskId");
        p.revision = x.at("revision");
        p.day = x.at("day");
        p.originalDue = x.at("originalDue");
        p.due = x.at("due");
        p.detached = x.at("detached");
        auto t =
            std::find_if(s.tasks.begin(), s.tasks.end(), [&](const auto &v) { return v.id == p.taskId; });
        if (t == s.tasks.end() || !t->enabled || t->completed || t->revision != p.revision || p.key.empty() ||
            p.key.size() > 180 || !keys.insert(p.key).second || p.due < 0 || p.due > 253402300799999LL ||
            p.originalDue < 0 || p.day < -200000 || p.day > 3000000)
            throw std::runtime_error("invalid occurrence");
        s.pending.push_back(p);
    }
    for (const auto &x : logs) {
        Log l;
        l.key = x.at("key");
        l.name = x.at("name");
        l.detail = x.at("detail");
        l.at = x.at("at");
        l.due = x.at("due");
        auto st = x.at("status").get<std::string>();
        if (st == "cancelled")
            l.status = Status::Cancelled;
        else if (st == "snoozed")
            l.status = Status::Snoozed;
        else if (st == "missed")
            l.status = Status::Missed;
        else if (st == "requesting")
            l.status = Status::Requesting;
        else if (st == "requested")
            l.status = Status::Requested;
        else if (st == "failed")
            l.status = Status::Failed;
        else
            throw std::runtime_error("invalid status");
        s.logs.push_back(l);
    }
    return s;
}
State readState(const std::filesystem::path &path) {
    if (std::filesystem::file_size(path) > 4 * 1024 * 1024)
        throw std::runtime_error("configuration too large");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read configuration");
    return decode(json::parse(input));
}
LoadResult Storage::load() const {
    LoadResult out;
    std::error_code ec;
    auto path = directory / L"state.json", backup = directory / L"state.backup.json";
    if (!std::filesystem::exists(path, ec) && !std::filesystem::exists(backup, ec))
        return out;
    try {
        out.state = readState(path);
        return out;
    } catch (...) {
    }
    try {
        out.state = readState(backup);
        out.warning = L"配置异常，已恢复备份。任务已暂停，请检查后恢复。";
        out.recovered = true;
    } catch (...) {
        out.warning = L"配置无法读取，任务已暂停。原文件保留在数据目录。";
        out.recovered = true;
    }
    out.state.settings.paused = true;
    try {
        if (std::filesystem::exists(path))
            std::filesystem::copy_file(
                path, directory / (L"state.corrupt." + std::to_wstring(snapshot().wall) + L".json"),
                std::filesystem::copy_options::overwrite_existing);
    } catch (...) {
    }
    return out;
}
bool Storage::save(const State &state, std::wstring &error) const {
    auto fail = [&]() {
        error = L"设置无法保存（错误 " + std::to_wstring(GetLastError()) +
                L"）。自动任务已暂停。请检查数据目录权限和可用空间。";
        return false;
    };
    try {
        std::filesystem::create_directories(directory);
        auto target = directory / L"state.json", temp = directory / L"state.tmp.json",
             backup = directory / L"state.backup.json";
        std::string bytes = encode(state).dump(2);
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return fail();
        DWORD written = 0;
        bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                  written == bytes.size() && FlushFileBuffers(file);
        DWORD code = GetLastError();
        CloseHandle(file);
        SetLastError(code);
        if (!ok)
            return fail();
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!ReplaceFileW(target.c_str(), temp.c_str(), backup.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS,
                              nullptr, nullptr))
                return fail();
        } else if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
            return fail();
        return true;
    } catch (...) {
        SetLastError(ERROR_WRITE_FAULT);
        return fail();
    }
}
PowerResult requestShutdown(bool force, bool simulate) {
    if (simulate)
        return {true, L"模拟模式：没有调用系统关机"};
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return {false, L"无法获取关机权限"};
    TOKEN_PRIVILEGES privilege{};
    privilege.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privilege.Privileges[0].Luid)) {
        CloseHandle(token);
        return {false, L"系统不支持关机权限"};
    }
    privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    TOKEN_PRIVILEGES previous{};
    DWORD previousSize = sizeof(previous);
    SetLastError(ERROR_SUCCESS);
    BOOL changed =
        AdjustTokenPrivileges(token, FALSE, &privilege, sizeof(previous), &previous, &previousSize);
    DWORD code = GetLastError();
    if (!changed || code == ERROR_NOT_ALL_ASSIGNED) {
        CloseHandle(token);
        return {false, L"当前账户没有关机权限，请检查系统策略"};
    }
    BOOL ok =
        ExitWindowsEx(EWX_POWEROFF | (force ? EWX_FORCE : 0),
                      SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
    code = GetLastError();
    AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
    CloseHandle(token);
    return {ok != FALSE,
            ok ? L"Windows 已接受关机请求" : L"Windows 拒绝关机请求（错误 " + std::to_wstring(code) + L"）"};
}
std::wstring executablePath() {
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    return std::wstring(path, n);
}
static constexpr const wchar_t *RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
bool hasAutostart() {
    HKEY key;
    bool found = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RunKey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD size = 0;
        found = RegQueryValueExW(key, L"Evenfall", nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS;
        RegCloseKey(key);
    }
    return found;
}
bool setAutostart(bool enabled, std::wstring &error) {
    HKEY key;
    LSTATUS result =
        RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result == ERROR_SUCCESS) {
        if (enabled) {
            std::wstring command = L"\"" + executablePath() + L"\" --tray";
            if (command.size() > 259) {
                RegCloseKey(key);
                error = L"程序路径过长，请移到较短的固定目录。";
                return false;
            }
            result =
                RegSetValueExW(key, L"Evenfall", 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                               static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        } else {
            result = RegDeleteValueW(key, L"Evenfall");
            if (result == ERROR_FILE_NOT_FOUND)
                result = ERROR_SUCCESS;
        }
        RegCloseKey(key);
    }
    if (result != ERROR_SUCCESS) {
        error = L"自启动设置失败（错误 " + std::to_wstring(result) + L"）";
        return false;
    }
    return true;
}
} // namespace evening
