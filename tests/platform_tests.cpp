#include "platform.hpp"
#include <fstream>
#include <iostream>
#include <shellapi.h>
#include <stdexcept>
using namespace evening;
int checks = 0;
void check(bool v, const char *m) {
    ++checks;
    if (!v)
        throw std::runtime_error(m);
}
int main() {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    try {
        if (argc < 2)
            throw std::runtime_error("isolated storage path required");
        auto path = std::filesystem::path(argv[1]);
        LocalFree(argv);
        Storage storage(path);
        WindowsCalendar calendar;
        auto now = snapshot();
        Engine e(calendar);
        Task t;
        t.id = "utf8";
        t.name = "周末 · 阅读结束";
        t.kind = Kind::Once;
        t.onceDue = now.wall + 120000;
        auto l = calendar.local(t.onceDue);
        t.day = l.day;
        t.minute = l.minute;
        e.upsert(t, now);
        std::wstring error;
        check(storage.save(e.state, error), "initial save");
        auto loaded = storage.load();
        check(loaded.state.tasks.size() == 1 && loaded.state.tasks[0].name == t.name, "Chinese JSON");
        e.tick({now.wall + 60000, now.steady + 60000});
        e.cancel({now.wall + 61000, now.steady + 61000});
        check(storage.save(e.state, error), "cancel save");
        Engine restore(calendar);
        restore.state = storage.load().state;
        restore.tick({now.wall + 62000, now.steady + 62000}, Reconcile::Startup);
        check(!restore.active && restore.state.tasks[0].completed, "actual persisted cancellation");
        check(storage.save(e.state, error), "backup cancelled");
        {
            std::ofstream f(path / L"state.json", std::ios::binary);
            f << "{broken";
        }
        auto recovery = storage.load();
        check(recovery.recovered && recovery.state.settings.paused, "corruption pauses");
        check(recovery.state.tasks[0].completed, "backup keeps cancellation");
        auto invalid = e.state;
        invalid.tasks[0].minute = 2000;
        check(storage.save(invalid, error), "invalid fixture");
        check(storage.load().recovered, "schema rejects");
        Storage denied(path / L"blocker");
        {
            std::ofstream f(path / L"blocker");
            f << "blocked";
        }
        check(!denied.save(e.state, error), "write failure");
        auto simulated = requestShutdown(true, true);
        check(simulated.accepted && simulated.message.find(L"模拟") != std::wstring::npos, "safe simulation");
        check(wide(utf8(L"中文输入 · 你好")) == L"中文输入 · 你好", "Unicode");
        DYNAMIC_TIME_ZONE_INFORMATION zone{};
        bool found = false;
        for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &zone) == ERROR_SUCCESS; ++i)
            if (std::wstring(zone.TimeZoneKeyName) == L"Pacific Standard Time") {
                found = true;
                break;
            }
        check(found, "Windows Pacific zone available");
        WindowsCalendar pacific(zone);
        check(!pacific.resolve(civilDay(2026, 3, 8), 150), "Windows daylight-saving gap");
        check(pacific.resolve(civilDay(2026, 11, 1), 90) == civilDay(2026, 11, 1) * Day + 510 * Minute,
              "Windows daylight-saving fold chooses first");
        check(calendar.resolve(civilDay(9999, 12, 31), 1380).has_value(), "supported upper date bound");
        State farFuture;
        Task distant;
        distant.id = "far-future";
        distant.name = "日期上限";
        distant.kind = Kind::Once;
        distant.day = civilDay(9999, 12, 31);
        distant.onceDue = *calendar.resolve(distant.day, 1380);
        farFuture.tasks.push_back(distant);
        Storage upperDate(path / L"upper-date");
        check(upperDate.save(farFuture, error), "upper date save");
        auto upperLoaded = upperDate.load();
        check(!upperLoaded.recovered && upperLoaded.state.tasks.size() == 1 &&
                  upperLoaded.state.tasks.front().day == distant.day,
              "upper date round-trip");
        std::cout << "PASS: " << checks << " persistence/platform assertions\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
