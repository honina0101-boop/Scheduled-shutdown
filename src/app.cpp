#include "ui.hpp"
#include <array>
#include <commctrl.h>
#include <ctime>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <shellapi.h>
#include <sstream>
#include <wincodec.h>
#include <windowsx.h>
using namespace evening;
enum Command {
    Home = 10,
    Preferences,
    History,
    NewTask,
    QuickCustom,
    Quick30,
    Quick60,
    Quick120,
    PauseAll,
    AutoStart,
    Sound,
    Force,
    ThemeSystem,
    ThemeLight,
    ThemeDark,
    Preview,
    DataFolder,
    RetrySave,
    Quit,
    CancelCurrent,
    Snooze10,
    Snooze30,
    Snooze60,
    SnoozeMenu,
    CycleAlert,
    EditorOnce = 100,
    EditorWeekly,
    EditorCountdown,
    EveryDay,
    WorkDays,
    Weekend,
    EditorCancel,
    EditorSave,
    Monday = 200,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
    Sunday
};
constexpr UINT TrayMessage = WM_APP + 1, ShowMessage = WM_APP + 42;
class App;
static App *application = nullptr;
static LRESULT CALLBACK windowProcedure(HWND, UINT, WPARAM, LPARAM);
static std::wstring controlText(HWND w) {
    int n = GetWindowTextLengthW(w);
    std::wstring o(n + 1, L'\0');
    GetWindowTextW(w, o.data(), n + 1);
    o.resize(n);
    return o;
}
static std::wstring dateText(int day) {
    using namespace std::chrono;
    year_month_day d{sys_days{days{day}}};
    wchar_t b[32];
    swprintf(b, 32, L"%04d-%02d-%02d", int(d.year()), unsigned(d.month()), unsigned(d.day()));
    return b;
}
static std::wstring minuteText(int m) {
    wchar_t b[12];
    swprintf(b, 12, L"%02d:%02d", m / 60, m % 60);
    return b;
}
static bool digits(const std::wstring &s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}
static bool parseDate(const std::wstring &s, int &result) {
    if (s.size() != 10 || s[4] != L'-' || s[7] != L'-' || !digits(s.substr(0, 4)) ||
        !digits(s.substr(5, 2)) || !digits(s.substr(8, 2)))
        return false;
    using namespace std::chrono;
    int y = std::stoi(s.substr(0, 4)), m = std::stoi(s.substr(5, 2)), d = std::stoi(s.substr(8, 2));
    year_month_day date{year{y}, month{unsigned(m)}, day{unsigned(d)}};
    if (y < 1970 || y > 9999 || !date.ok())
        return false;
    result = int(sys_days{date}.time_since_epoch().count());
    return true;
}
static bool parseMinute(const std::wstring &s, int &r) {
    if (s.size() != 5 || s[2] != L':' || !digits(s.substr(0, 2)) || !digits(s.substr(3, 2)))
        return false;
    int h = std::stoi(s.substr(0, 2)), m = std::stoi(s.substr(3, 2));
    if (h > 23 || m > 59)
        return false;
    r = h * 60 + m;
    return true;
}
static std::wstring duration(Time v) {
    auto s = std::max(Time{0}, (v + 999) / 1000);
    if (s < 60)
        return std::to_wstring(s) + L" 秒";
    if (s < 3600)
        return std::to_wstring((s + 59) / 60) + L" 分钟";
    auto h = s / 3600, m = (s % 3600) / 60;
    if (h >= 24)
        return std::to_wstring(h / 24) + L" 天 " + std::to_wstring(h % 24) + L" 小时";
    return std::to_wstring(h) + L" 小时 " + std::to_wstring(m) + L" 分钟";
}
class App {
  public:
    HINSTANCE instance;
    WindowsCalendar calendar;
    Engine engine{calendar};
    Storage storage;
    Surface main, editor, warning;
    ID2D1Factory *graphics = nullptr;
    IDWriteFactory *writer = nullptr;
    HFONT editorFont = nullptr;
    HBRUSH fieldBrush = nullptr;
    HANDLE mutex = nullptr;
    UINT taskbarCreated = 0;
    std::wstring mainClass, editorClass, warningClass;
    bool simulate = false, smoke = false, trayStart = false, preview = false, blocked = false,
         benchmark = false;
    int page = 0, smokeChecks = 0, dispatchCount = 0;
    Time previewDeadline = 0, warningAnimation = 0;
    float warningProgress = 1;
    std::filesystem::path smokeOutput;
    std::wstring banner, editorError;
    std::array<HWND, 4> fields{};
    Task draft;
    App(HINSTANCE h, std::filesystem::path data) : instance(h), storage(std::move(data)) {}
    ~App() {
        release(writer);
        release(graphics);
        if (editorFont)
            DeleteObject(editorFont);
        if (fieldBrush)
            DeleteObject(fieldBrush);
        if (mutex)
            CloseHandle(mutex);
    }
    bool dark() const {
        if (engine.state.settings.theme == "dark")
            return true;
        if (engine.state.settings.theme == "light")
            return false;
        DWORD light = 1, size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        return light == 0;
    }
    Theme theme() const { return dark() ? Theme::dark() : Theme::light(); }
    void updateTheme() {
        BOOL value = dark();
        for (auto *s : {&main, &editor, &warning})
            if (s->window) {
                DwmSetWindowAttribute(s->window, 20, &value, sizeof(value));
                InvalidateRect(s->window, nullptr, FALSE);
            }
        if (fieldBrush)
            DeleteObject(fieldBrush);
        unsigned c = theme().field;
        fieldBrush = CreateSolidBrush(RGB((c >> 16) & 255, (c >> 8) & 255, c & 255));
        if (editor.window)
            for (HWND f : fields)
                if (f)
                    InvalidateRect(f, nullptr, TRUE);
    }
    bool save() {
        std::wstring error;
        if (storage.save(engine.state, error)) {
            blocked = false;
            if (banner.find(L"无法保存") != std::wstring::npos)
                banner.clear();
            return true;
        }
        blocked = true;
        engine.state.settings.paused = true;
        engine.clearWarnings();
        banner = error;
        hideWarning();
        return false;
    }
    bool initialize() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &graphics)) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown **>(&writer))))
            return false;
        std::wstring suffix = std::to_wstring(std::hash<std::wstring>{}(storage.directory.wstring()));
        mainClass = L"Evenfall.Main." + suffix;
        editorClass = L"Evenfall.Editor." + suffix;
        warningClass = L"Evenfall.Warning." + suffix;
        mutex = CreateMutexW(nullptr, FALSE, (L"Local\\Evenfall." + suffix).c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(mainClass.c_str(), nullptr);
            if (existing)
                SendMessageW(existing, ShowMessage, 0, 0);
            return false;
        }
        for (auto cls : {mainClass, editorClass, warningClass}) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = windowProcedure;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
            wc.hIconSm = wc.hIcon;
            wc.lpszClassName = cls.c_str();
            if (!RegisterClassExW(&wc))
                return false;
        }
        auto loaded = storage.load();
        engine.state = std::move(loaded.state);
        banner = loaded.warning;
        if (!simulate)
            engine.state.settings.autostart = hasAutostart();
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        UINT dpi = systemDpi();
        float scale = dpi / 96.f;
        int w = std::min(int(900 * scale), int(work.right - work.left - 32)),
            h = std::min(int(704 * scale), int(work.bottom - work.top - 32));
        main.window = CreateWindowExW(0, mainClass.c_str(), L"晚安 · 定时关机", WS_OVERLAPPEDWINDOW,
                                      work.left + (work.right - work.left - w) / 2,
                                      work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr,
                                      instance, &main);
        if (!main.window)
            return false;
        main.scale = windowDpi(main.window) / 96.f;
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        updateTheme();
        addTray();
        if (smoke) {
            setupSmoke();
            ShowWindow(main.window, SW_SHOWNOACTIVATE);
            SetTimer(main.window, 99, 350, nullptr);
        } else {
            save();
            pump(Reconcile::Startup);
            if (!trayStart)
                ShowWindow(main.window, benchmark ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
            if (benchmark)
                SetTimer(main.window, 98, 2000, nullptr);
        }
        return true;
    }
    void addTray() {
        NOTIFYICONDATAW icon{sizeof(icon)};
        icon.hWnd = main.window;
        icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        icon.uCallbackMessage = TrayMessage;
        icon.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wcscpy_s(icon.szTip, L"晚安 · 定时关机");
        Shell_NotifyIconW(NIM_ADD, &icon);
        icon.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &icon);
    }
    void removeTray() {
        NOTIFYICONDATAW n{sizeof(n)};
        n.hWnd = main.window;
        n.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &n);
    }
    void showMain() {
        ShowWindow(main.window, SW_RESTORE);
        SetForegroundWindow(main.window);
        InvalidateRect(main.window, nullptr, FALSE);
    }
    void trayMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, Home, L"打开晚安");
        AppendMenuW(menu, MF_STRING, PauseAll,
                    engine.state.settings.paused ? L"恢复全部任务" : L"暂停全部任务");
        AppendMenuW(menu, MF_STRING | (engine.active ? 0 : MF_GRAYED), CancelCurrent, L"取消当前倒计时");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, Quit, L"退出");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(main.window);
        int c = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, main.window, nullptr);
        DestroyMenu(menu);
        PostMessageW(main.window, WM_NULL, 0, 0);
        if (c == Home)
            showMain();
        else if (c)
            command(c, main.window);
    }
    void pump(Reconcile reason = Reconcile::Normal) {
        KillTimer(main.window, 1);
        if (blocked) {
            InvalidateRect(main.window, nullptr, FALSE);
            return;
        }
        auto outcome = engine.tick(snapshot(), reason);
        if (outcome.changed && !save()) {
            engine.result(outcome.dispatch, false, "状态未能保存，没有提交关机请求");
            InvalidateRect(main.window, nullptr, FALSE);
            return;
        }
        if (!outcome.dispatch.empty()) {
            hideWarning();
            ++dispatchCount;
            auto result = requestShutdown(engine.state.settings.force, simulate);
            engine.result(outcome.dispatch, result.accepted, utf8(result.message));
            save();
            if (!result.accepted || simulate)
                banner = result.message;
        }
        if (engine.active) {
            if (preview) {
                preview = false;
                KillTimer(main.window, 3);
            }
            showWarning(false);
            if (outcome.warningOpened && engine.state.settings.sound && !smoke)
                MessageBeep(MB_ICONINFORMATION);
        } else if (!preview)
            hideWarning();
        SetTimer(main.window, 1, static_cast<UINT>(engine.nextWait(snapshot())), nullptr);
        for (auto *s : {&main, &editor, &warning})
            if (s->window && IsWindowVisible(s->window))
                InvalidateRect(s->window, nullptr, FALSE);
    }
    void afterMutation() {
        main.focus = 0;
        main.hover = 0;
        if (save())
            pump();
        else
            InvalidateRect(main.window, nullptr, FALSE);
    }
    void hideWarning() {
        if (warning.window)
            ShowWindow(warning.window, SW_HIDE);
        preview = false;
        KillTimer(main.window, 3);
        if (warning.window)
            KillTimer(warning.window, 4);
    }
    void placeWarning() {
        if (!warning.window)
            return;
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
        warning.scale = windowDpi(warning.window) / 96.f;
        float s = warning.scale;
        int w = int(456 * s), h = int((274 + (!preview && !engine.otherWarnings.empty() ? 44 : 0)) * s),
            margin = int(20 * s);
        SetWindowPos(warning.window, HWND_TOPMOST, info.rcWork.right - w - margin,
                     info.rcWork.bottom - h - margin + int((1 - warningProgress) * 16 * s), w, h,
                     SWP_NOACTIVATE);
        SetWindowRgn(warning.window, CreateRoundRectRgn(0, 0, w + 1, h + 1, int(32 * s), int(32 * s)), TRUE);
    }
    void showWarning(bool isPreview) {
        if (isPreview && engine.active) {
            showWarning(false);
            return;
        }
        if (!warning.window) {
            warning.window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                             warningClass.c_str(), L"晚安 · 关机提醒", WS_POPUP, 0, 0, 456,
                                             274, nullptr, nullptr, instance, &warning);
            updateTheme();
        }
        if (isPreview) {
            preview = true;
            previewDeadline = snapshot().steady + Minute;
            SetTimer(main.window, 3, 1000, nullptr);
        }
        bool entering = !IsWindowVisible(warning.window);
        if (entering) {
            BOOL animation = TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
            warningProgress = animation && !smoke ? 0 : 1;
            warningAnimation = snapshot().steady;
            if (warningProgress < 1)
                SetTimer(warning.window, 4, 16, nullptr);
        }
        placeWarning();
        ShowWindow(warning.window, SW_SHOWNOACTIVATE);
        InvalidateRect(warning.window, nullptr, FALSE);
    }
    void quick(int minutes) {
        if (engine.state.tasks.size() >= 512) {
            banner = L"任务数量已达到上限，请删除不需要的任务。";
            return;
        }
        auto now = snapshot();
        Task t;
        t.id = newId();
        t.name = utf8(std::to_wstring(minutes) + L" 分钟后关机");
        t.kind = Kind::Countdown;
        t.minutes = minutes;
        t.onceDue = now.wall + minutes * Minute;
        auto l = calendar.local(t.onceDue);
        t.day = l.day;
        t.minute = l.minute;
        engine.upsert(t, now);
        afterMutation();
    }
    float editorY(float y) const { return y * std::min(1.f, editor.height() / 570.f); }
    void positionFields() {
        if (!editor.window || !fields[0])
            return;
        editor.scale = windowDpi(editor.window) / 96.f;
        float s = editor.scale;
        if (editorFont)
            DeleteObject(editorFont);
        editorFont = CreateFontW(-int(14 * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 L"Microsoft YaHei UI");
        auto move = [&](int i, float x, float y, float w, float h) {
            MoveWindow(fields[i], int(x * s), int(editorY(y) * s), int(w * s), int(editorY(h) * s), TRUE);
            SendMessageW(fields[i], WM_SETFONT, reinterpret_cast<WPARAM>(editorFont), TRUE);
        };
        float w = editor.width();
        move(0, 44, 131, w - 88, 28);
        move(1, 44, 301, (w - 104) / 2, 28);
        move(2, draft.kind == Kind::Once ? w / 2 + 20 : 44, 301,
             draft.kind == Kind::Once ? (w - 104) / 2 : w - 88, 28);
        move(3, 44, 301, w - 88, 28);
        ShowWindow(fields[1], draft.kind == Kind::Once ? SW_SHOW : SW_HIDE);
        ShowWindow(fields[2], draft.kind == Kind::Countdown ? SW_HIDE : SW_SHOW);
        ShowWindow(fields[3], draft.kind == Kind::Countdown ? SW_SHOW : SW_HIDE);
    }
    void openEditor(const Task *existing = nullptr, bool countdown = false) {
        if (editor.window) {
            SetForegroundWindow(editor.window);
            return;
        }
        auto now = snapshot();
        draft = existing ? *existing : Task{};
        if (!existing) {
            draft.id = newId();
            draft.name = countdown ? "倒计时关机" : "每日关机";
            draft.kind = countdown ? Kind::Countdown : Kind::Weekly;
            draft.day = calendar.local(now.wall).day;
            if (calendar.resolve(draft.day, draft.minute).value_or(0) <= now.wall)
                ++draft.day;
        }
        editorError.clear();
        MONITORINFO work{sizeof(work)};
        GetMonitorInfoW(MonitorFromWindow(main.window, MONITOR_DEFAULTTOPRIMARY), &work);
        float s = main.scale;
        int width = int(528 * s),
            height = std::min(int(610 * s), int(work.rcWork.bottom - work.rcWork.top - 24));
        editor.window =
            CreateWindowExW(WS_EX_CONTROLPARENT, editorClass.c_str(),
                            existing ? L"编辑关机任务" : L"新建关机任务", WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            work.rcWork.left + (work.rcWork.right - work.rcWork.left - width) / 2,
                            work.rcWork.top + (work.rcWork.bottom - work.rcWork.top - height) / 2, width,
                            height, main.window, nullptr, instance, &editor);
        if (!editor.window)
            return;
        editor.scale = windowDpi(editor.window) / 96.f;
        for (int i = 0; i < 4; i++)
            fields[i] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                        0, 0, 10, 10, editor.window,
                                        reinterpret_cast<HMENU>(INT_PTR(1000 + i)), instance, nullptr);
        SendMessageW(fields[0], EM_SETLIMITTEXT, 80, 0);
        SendMessageW(fields[1], EM_SETLIMITTEXT, 10, 0);
        SendMessageW(fields[2], EM_SETLIMITTEXT, 5, 0);
        SendMessageW(fields[3], EM_SETLIMITTEXT, 6, 0);
        SetWindowTextW(fields[0], wide(draft.name).c_str());
        SetWindowTextW(fields[1], dateText(draft.day).c_str());
        SetWindowTextW(fields[2], minuteText(draft.minute).c_str());
        SetWindowTextW(fields[3], std::to_wstring(draft.minutes).c_str());
        positionFields();
        updateTheme();
        EnableWindow(main.window, FALSE);
        ShowWindow(editor.window, SW_SHOWNORMAL);
        SetFocus(fields[0]);
        SendMessageW(fields[0], EM_SETSEL, 0, -1);
    }
    void closeEditor() {
        if (!editor.window)
            return;
        HWND w = editor.window;
        editor.window = nullptr;
        DestroyWindow(w);
        editor.discard();
        fields.fill(nullptr);
        EnableWindow(main.window, TRUE);
        if (!smoke)
            SetForegroundWindow(main.window);
        InvalidateRect(main.window, nullptr, FALSE);
    }
    std::optional<Task> edited(bool report) {
        Task t = draft;
        auto fail = [&](const wchar_t *m) -> std::optional<Task> {
            if (report)
                editorError = m;
            return {};
        };
        std::wstring name = controlText(fields[0]);
        auto first = name.find_first_not_of(L" \t\r\n"), last = name.find_last_not_of(L" \t\r\n");
        if (first == std::wstring::npos)
            return fail(L"请填写任务名称。");
        name = name.substr(first, last - first + 1);
        t.name = utf8(name);
        t.completed = false;
        if (t.kind == Kind::Countdown) {
            auto value = controlText(fields[3]);
            if (!digits(value) || value.size() > 6)
                return fail(L"请输入有效的分钟数。");
            t.minutes = std::stoi(value);
            if (t.minutes < 1 || t.minutes > 525600)
                return fail(L"分钟数范围为 1 至 525600。");
            t.onceDue = snapshot().wall + t.minutes * Minute;
            auto l = calendar.local(t.onceDue);
            t.day = l.day;
            t.minute = l.minute;
        } else {
            if (!parseMinute(controlText(fields[2]), t.minute))
                return fail(L"时间格式应为 23:00，使用 24 小时制。");
            if (t.kind == Kind::Weekly) {
                if (!t.weekdays)
                    return fail(L"请至少选择一个星期。");
            } else {
                if (!parseDate(controlText(fields[1]), t.day))
                    return fail(L"日期格式应为 2026-10-08。");
                auto due = calendar.resolve(t.day, t.minute);
                if (!due)
                    return fail(L"这个本地时间不存在，请选择其他时间。");
                if (*due <= snapshot().wall) {
                    auto *existing = engine.task(t.id);
                    if (!existing || !existing->completed || existing->onceDue != *due)
                        return fail(L"执行一次的时间必须在未来。");
                }
                t.onceDue = *due;
            }
        }
        return t;
    }
    void command(int id, HWND source) {
        if (id >= 10000) {
            size_t index = (id - 10000) / 10;
            int action = (id - 10000) % 10;
            if (index >= engine.state.tasks.size())
                return;
            Task t = engine.state.tasks[index];
            if (action == 0)
                openEditor(&t);
            else if (action == 1) {
                engine.enable(t.id, !t.enabled, snapshot());
                afterMutation();
            } else if (action == 2) {
                if (smoke || MessageBoxW(main.window, (L"删除任务“" + wide(t.name) + L"”？").c_str(),
                                         L"删除任务", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    engine.erase(t.id, snapshot());
                    afterMutation();
                }
            }
            return;
        }
        if (id >= Monday && id <= Sunday) {
            draft.weekdays ^= 1 << (id - Monday);
            InvalidateRect(editor.window, nullptr, FALSE);
            return;
        }
        switch (id) {
        case Home:
        case Preferences:
        case History:
            page = id - Home;
            main.scroll = 0;
            main.focus = 0;
            InvalidateRect(main.window, nullptr, FALSE);
            break;
        case NewTask:
            openEditor();
            break;
        case QuickCustom:
            openEditor(nullptr, true);
            break;
        case Quick30:
            quick(30);
            break;
        case Quick60:
            quick(60);
            break;
        case Quick120:
            quick(120);
            break;
        case PauseAll:
            if (blocked) {
                banner = L"请先在设置中重试保存，确认数据可以安全保存。";
                InvalidateRect(main.window, nullptr, FALSE);
                break;
            }
            engine.pause(snapshot(), !engine.state.settings.paused);
            afterMutation();
            break;
        case AutoStart: {
            if (simulate) {
                banner = L"模拟模式不会修改 Windows 自启动设置。";
                InvalidateRect(main.window, nullptr, FALSE);
                break;
            }
            bool value = !engine.state.settings.autostart;
            std::wstring error;
            if (setAutostart(value, error)) {
                engine.state.settings.autostart = value;
                afterMutation();
            } else {
                banner = error;
                InvalidateRect(main.window, nullptr, FALSE);
            }
            break;
        }
        case Sound:
            engine.state.settings.sound = !engine.state.settings.sound;
            afterMutation();
            break;
        case Force:
            if (!engine.state.settings.force && !smoke &&
                MessageBoxW(main.window, L"强制关机会关闭应用，未保存的文档可能丢失。\n\n启用强制关机？",
                            L"强制关机", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                break;
            engine.state.settings.force = !engine.state.settings.force;
            afterMutation();
            break;
        case ThemeSystem:
        case ThemeLight:
        case ThemeDark:
            engine.state.settings.theme = id == ThemeSystem ? "system" : id == ThemeLight ? "light" : "dark";
            updateTheme();
            afterMutation();
            break;
        case Preview:
            showWarning(true);
            break;
        case DataFolder:
            ShellExecuteW(main.window, L"open", storage.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case RetrySave:
            if (save()) {
                banner = L"设置已保存。检查任务后，可以恢复执行。";
                pump(Reconcile::Startup);
            }
            break;
        case CancelCurrent:
            if (preview) {
                hideWarning();
                break;
            }
            if (engine.cancel(snapshot()))
                afterMutation();
            break;
        case CycleAlert:
            if (!preview && engine.cycleWarning())
                pump();
            break;
        case Snooze10:
        case Snooze30:
        case Snooze60:
            if (preview) {
                hideWarning();
                break;
            }
            if (engine.snooze(snapshot(), id == Snooze10 ? 10 : id == Snooze30 ? 30 : 60))
                afterMutation();
            break;
        case SnoozeMenu: {
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, Snooze10, L"推迟 10 分钟");
            AppendMenuW(m, MF_STRING, Snooze30, L"推迟 30 分钟");
            AppendMenuW(m, MF_STRING, Snooze60, L"推迟 60 分钟");
            POINT pt;
            GetCursorPos(&pt);
            int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, source, nullptr);
            DestroyMenu(m);
            if (c)
                command(c, source);
            break;
        }
        case EditorOnce:
        case EditorWeekly:
        case EditorCountdown:
            draft.kind = id == EditorOnce ? Kind::Once : id == EditorWeekly ? Kind::Weekly : Kind::Countdown;
            editorError.clear();
            positionFields();
            InvalidateRect(editor.window, nullptr, FALSE);
            break;
        case EveryDay:
            draft.weekdays = 127;
            InvalidateRect(editor.window, nullptr, FALSE);
            break;
        case WorkDays:
            draft.weekdays = 31;
            InvalidateRect(editor.window, nullptr, FALSE);
            break;
        case Weekend:
            draft.weekdays = 96;
            InvalidateRect(editor.window, nullptr, FALSE);
            break;
        case EditorCancel:
            closeEditor();
            break;
        case EditorSave: {
            auto t = edited(true);
            if (!t) {
                InvalidateRect(editor.window, nullptr, FALSE);
                break;
            }
            if (!engine.task(t->id) && engine.state.tasks.size() >= 512) {
                editorError = L"任务数量已达到上限。";
                InvalidateRect(editor.window, nullptr, FALSE);
                break;
            }
            engine.upsert(*t, snapshot());
            if (save()) {
                closeEditor();
                pump();
            } else {
                editorError = banner;
                InvalidateRect(editor.window, nullptr, FALSE);
            }
            break;
        }
        case Quit:
            closeEditor();
            hideWarning();
            removeTray();
            DestroyWindow(main.window);
            break;
        }
    }

    void header(Painter &p, float w) {
        auto t = p.theme;
        p.gradient(rect(28, 23, 38, 38), 0x8671EE, 0x5548C5, 12);
        p.circle(47, 42, 11, 0xFFFFFF);
        p.circle(52, 37, 10, 0x7360DC);
        p.circle(56, 49, 2, 0x83E9C7);
        p.text(L"晚安", rect(78, 21, 92, 26), 22, t.text, 650);
        p.text(L"EVENFALL", rect(79, 47, 102, 16), 9, t.muted, 500);
        float navX = std::max(185.f, (w - 252) / 2);
        p.fill(rect(navX, 26, 252, 36), t.field, 11);
        for (int i = 0; i < 3; i++)
            p.button(rect(navX + 4 + i * 82, 30, 80, 28), Home + i,
                     i == 0   ? L"任务"
                     : i == 1 ? L"设置"
                              : L"记录",
                     false, page == i);
        bool paused = engine.state.settings.paused || blocked;
        float x = w - 133;
        p.fill(rect(x, 30, 104, 28), paused ? 0xE9C776 : dark() ? 0x183D38 : 0xE2F7EF, 14, paused ? .18f : 1);
        p.circle(x + 17, 44, 3.5f, paused ? 0xC59624 : t.good);
        p.text(paused ? L"已暂停" : L"运行中", rect(x + 28, 30, 66, 28), 12, paused ? 0xBB8A22 : t.good, 500);
        p.hit(rect(x, 30, 104, 28), PauseAll);
        if (main.focus == PauseAll)
            p.border(rect(x - 2, 28, 108, 32), t.accent, 16, 2);
    }
    void drawHome(Painter &p, float w, float h) {
        auto now = snapshot();
        const auto *next = engine.next();
        auto t = p.theme;
        bool paused = engine.state.settings.paused || blocked;
        bool compact = h < 480;
        float heroY = compact ? 76 : 88, heroH = compact ? 116 : std::clamp(h * .27f, 120.f, 184.f),
              heroBottom = heroY + heroH;
        p.fill(rect(32, heroY + 5, w - 64, heroH), 0x4942AA, 20, .10f);
        p.gradient(rect(28, heroY, w - 56, heroH), paused ? 0x525971 : 0x6557DB, paused ? 0x303749 : 0x303574,
                   20);
        float right = w - 143, cy = heroY + heroH / 2, r = std::min(66.f, heroH * .34f);
        p.ring(right, cy, r, 1, 0xFFFFFF, 5, .13f);
        Time left = next ? std::max(Time{0}, next->due - now.wall) : 0;
        float fraction = next ? float(std::clamp(double(left) / Day, 0.0, 1.0)) : .75f;
        if (engine.active) {
            Time deadline = engine.active->monotonicDue;
            for (const auto &group : engine.otherWarnings)
                deadline = std::min(deadline, group.monotonicDue);
            left = std::max(Time{0}, deadline - now.steady);
            fraction = float(left) / Minute;
        }
        p.ring(right, cy, r, fraction, paused ? 0xAFBAD1 : 0x86F2D4, 5);
        if (next) {
            p.text(engine.active ? L"即将关机"
                   : paused      ? L"任务已暂停"
                                 : L"下一次关机",
                   rect(56, heroY + (heroH < 145 ? 12 : 18), w - 310, heroH < 145 ? 18 : 24), 13, 0xE1DFFF,
                   500);
            p.text(formatTime(next->due),
                   rect(53, heroY + (heroH < 145 ? 31 : 43), w - 310, heroH < 145 ? 52 : 75),
                   heroH < 145 ? 44 : 62, 0xFFFFFF, 300, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            std::wstring secondary = formatTime(next->due, true);
            if (auto *task = engine.task(next->taskId))
                secondary += L"  ·  " + wide(task->name);
            p.text(secondary, rect(57, heroBottom - (heroH < 145 ? 25 : 36), w - 308, heroH < 145 ? 18 : 22),
                   12, 0xE4E1FF);
            std::wstring ringText = paused ? L"等待恢复" : duration(left);
            if (compact) {
                auto pos = ringText.find(L" 小时 ");
                if (pos != std::wstring::npos)
                    ringText.replace(pos, 4, L"时");
                pos = ringText.find(L" 分钟");
                if (pos != std::wstring::npos)
                    ringText.replace(pos, 3, L"分");
            }
            p.text(ringText, rect(right - r - 6, cy - 13, 2 * r + 12, 28), compact || left >= Day ? 14 : 16,
                   0xFFFFFF, 550, DWRITE_TEXT_ALIGNMENT_CENTER);
            p.text(engine.active ? L"可取消本次" : L"距离关机", rect(right - r, cy + 15, 2 * r, 20), 10,
                   0xC6C5ED, 400, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            p.text(L"今晚，安心收尾。", rect(56, heroY + (compact ? 11 : 19), w - 300, compact ? 30 : 40),
                   heroH < 145 ? 26 : 30, 0xFFFFFF, 600);
            p.text(L"把关机交给晚安，把时间留给自己。",
                   rect(57, heroY + (compact ? 42 : 61), w - 300, compact ? 18 : 28), 12, 0xD9D5FF);
            p.button(rect(57, heroBottom - (compact ? 37 : 48), 132, compact ? 27 : 32), NewTask,
                     L"＋  创建第一个任务", true);
            p.circle(right, cy, 22, 0xF6F0FF);
            p.circle(right + 10, cy - 8, 20, paused ? 0x444B62 : 0x394084);
            p.circle(right + 28, cy + 18, 3, 0x85E8D1);
        }
        float toolbar = heroBottom + (compact ? 12 : 22);
        p.text(L"关机任务", rect(30, toolbar, 160, 38), 19, t.text, 650);
        p.text(std::to_wstring(engine.state.tasks.size()), rect(126, toolbar + 3, 40, 32), 12, t.muted);
        p.button(rect(w - 286, toolbar + 2, 112, 34), PauseAll, paused ? L"恢复全部" : L"暂停全部");
        p.button(rect(w - 164, toolbar + 2, 136, 34), NewTask, L"＋  新建任务", true);
        float top = toolbar + (compact ? 44 : 54), bottom = h - 79, listHeight = std::max(0.f, bottom - top);
        main.maxScroll = std::max(0.f, float(engine.state.tasks.size()) * 84 - listHeight);
        main.scroll = std::clamp(main.scroll, 0.f, main.maxScroll);
        if (engine.state.tasks.empty()) {
            float y = top + std::max(0.f, (listHeight - 75) / 2);
            p.text(L"还没有关机任务", rect(32, y, w - 64, 28), 16, t.text, 500, DWRITE_TEXT_ALIGNMENT_CENTER);
            p.text(L"设置一次，或选择每周重复。提前一分钟，随时取消。", rect(32, y + 33, w - 64, 24), 12,
                   t.muted, 400, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            p.clip(rect(24, top, w - 48, listHeight));
            for (size_t i = 0; i < engine.state.tasks.size(); i++) {
                auto task = engine.state.tasks[i];
                float y = top + i * 84 - main.scroll;
                if (y + 76 < top || y > bottom)
                    continue;
                p.fill(rect(28, y, w - 56, 76), t.card, 13);
                p.border(rect(28, y, w - 56, 76), t.border, 13);
                bool enabled = task.enabled && !task.completed;
                p.fill(rect(43, y + 17, 42, 42), t.field, 12);
                p.ring(64, y + 38, 11, 1, enabled ? t.accent : t.muted, 1.6f);
                p.line(64, y + 38, 64, y + 30, enabled ? t.accent : t.muted, 1.6f);
                p.line(64, y + 38, 70, y + 41, enabled ? t.accent : t.muted, 1.6f);
                p.text(wide(task.name), rect(101, y + 12, w - 369, 27), 15, enabled ? t.text : t.muted, 550);
                std::wstring detail = describe(task);
                if (task.completed)
                    detail += L"  ·  已结束";
                else if (!task.enabled)
                    detail += L"  ·  已停用";
                else {
                    auto it = std::min_element(engine.state.pending.begin(), engine.state.pending.end(),
                                               [&](const auto &a, const auto &b) {
                                                   Time aa = a.taskId == task.id ? a.due : INT64_MAX,
                                                        bb = b.taskId == task.id ? b.due : INT64_MAX;
                                                   return aa < bb;
                                               });
                    if (it != engine.state.pending.end() && it->taskId == task.id && it->detached)
                        detail += L"  ·  本次已推迟";
                }
                p.text(detail, rect(102, y + 42, w - 362, 21), 11, t.muted);
                int base = 10000 + int(i) * 10;
                p.button(rect(w - 247, y + 21, 56, 32), base, L"编辑");
                p.toggle(rect(w - 168, y + 26, 42, 22), base + 1, enabled);
                p.button(rect(w - 106, y + 21, 58, 32), base + 2, L"删除");
            }
            p.unclip();
            if (main.maxScroll > 0) {
                float track = std::max(20.f, listHeight * listHeight / (main.maxScroll + listHeight));
                p.fill(rect(w - 20, top + (listHeight - track) * main.scroll / main.maxScroll, 3, track),
                       t.border, 2);
            }
        }
        p.fill(rect(28, h - 63, w - 56, 45), t.card, 12);
        p.text(L"快速倒计时", rect(45, h - 59, 125, 36), 12, t.muted, 500);
        float bx = w - 404;
        for (int i = 0; i < 3; i++)
            p.button(rect(bx + i * 91, h - 57, 83, 32), Quick30 + i,
                     i == 0   ? L"30 分钟"
                     : i == 1 ? L"60 分钟"
                              : L"120 分钟");
        p.button(rect(w - 127, h - 57, 82, 32), QuickCustom, L"自定义");
    }
    void drawSettings(Painter &p, float w, float h) {
        auto t = p.theme;
        p.text(L"习惯，由你决定。", rect(30, 94, w - 60, 38), 27, t.text, 600);
        p.text(L"轻一点的工具，恰到好处的提醒。", rect(32, 135, w - 64, 25), 12, t.muted);
        float top = 177, bottom = h - 30;
        main.maxScroll = std::max(0.f, 425 - (bottom - top));
        main.scroll = std::clamp(main.scroll, 0.f, main.maxScroll);
        p.clip(rect(24, top, w - 48, std::max(0.f, bottom - top)));
        float y = top - main.scroll;
        p.fill(rect(28, y, w - 56, 230), t.card, 15);
        p.border(rect(28, y, w - 56, 230), t.border, 15);
        std::array<std::wstring, 3> names{L"登录后启动", L"提醒提示音", L"强制关机"};
        std::array<std::wstring, 3> descriptions{L"开机登录后自动运行到托盘，开始等待任务",
                                                 L"提前一分钟轻声提醒，为你留出取消的时间",
                                                 L"默认关闭。启用后，未保存的文档可能丢失"};
        bool values[] = {engine.state.settings.autostart, engine.state.settings.sound,
                         engine.state.settings.force};
        int commands[] = {AutoStart, Sound, Force};
        for (int i = 0; i < 3; i++) {
            float row = y + 12 + i * 74;
            p.text(names[i], rect(49, row, w - 176, 27), 15, t.text, 550);
            p.text(descriptions[i], rect(50, row + 30, w - 180, 22), 11, t.muted);
            p.toggle(rect(w - 95, row + 18, 42, 22), commands[i], values[i]);
            if (i < 2)
                p.line(49, row + 62, w - 50, row + 62, t.border);
        }
        y += 245;
        p.fill(rect(28, y, w - 56, 75), t.card, 14);
        p.border(rect(28, y, w - 56, 75), t.border, 14);
        p.text(L"外观", rect(49, y + 13, 110, 25), 15, t.text, 550);
        p.text(L"选择舒适的光线", rect(50, y + 41, 180, 20), 11, t.muted);
        float bx = w - 352;
        std::string themes[] = {"system", "light", "dark"};
        for (int i = 0; i < 3; i++)
            p.button(rect(bx + i * 98, y + 22, 90, 32), ThemeSystem + i,
                     i == 0   ? L"跟随系统"
                     : i == 1 ? L"浅色"
                              : L"深色",
                     false, engine.state.settings.theme == themes[i]);
        y += 94;
        p.button(rect(28, y, 145, 35), Preview, L"预览关机提醒", true);
        p.button(rect(185, y, 126, 35), DataFolder, L"打开数据目录");
        if (blocked)
            p.button(rect(w - 171, y, 143, 35), RetrySave, L"重试保存");
        p.text(L"晚安 1.0.0 · 离线运行 · 当前用户的数据独立保存", rect(30, y + 49, w - 60, 25), 11, t.muted);
        p.unclip();
    }
    void drawHistory(Painter &p, float w, float h) {
        auto t = p.theme;
        p.text(L"每一次，都有迹可循。", rect(30, 94, w - 60, 38), 27, t.text, 600);
        p.text(L"最近 100 条记录。关机请求交给 Windows 后，由系统完成关闭。", rect(32, 138, w - 64, 25), 12,
               t.muted);
        float top = 182, bottom = h - 29, height = std::max(0.f, bottom - top);
        main.maxScroll = std::max(0.f, float(engine.state.logs.size()) * 83 - height);
        main.scroll = std::clamp(main.scroll, 0.f, main.maxScroll);
        if (engine.state.logs.empty())
            p.text(L"尚无执行记录，任务发生后会显示在这里。", rect(32, top + 35, w - 64, 45), 14, t.muted,
                   400, DWRITE_TEXT_ALIGNMENT_CENTER);
        p.clip(rect(24, top, w - 48, height));
        for (size_t i = 0; i < engine.state.logs.size(); i++) {
            auto log = engine.state.logs[i];
            float y = top + i * 83 - main.scroll;
            if (y + 75 < top || y > bottom)
                continue;
            p.fill(rect(28, y, w - 56, 74), t.card, 12);
            p.border(rect(28, y, w - 56, 74), t.border, 12);
            unsigned c = log.status == Status::Failed      ? 0xD37070
                         : log.status == Status::Missed    ? 0xBE923D
                         : log.status == Status::Requested ? t.good
                                                           : t.accent;
            p.fill(rect(43, y + 21, 99, 28), c, 8, .12f);
            p.text(describeStatus(log.status), rect(43, y + 21, 99, 28), 12, c, 550,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
            p.text(wide(log.name), rect(159, y + 9, w - 387, 29), 14, t.text, 550);
            p.text(log.detail.empty() ? L"本次触发已处理" : wide(log.detail), rect(160, y + 40, w - 224, 22),
                   11, t.muted);
            p.text(formatTime(log.at, true), rect(w - 250, y + 9, 202, 28), 11, t.muted, 400,
                   DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        p.unclip();
    }
    void field(Painter &p, int index, float x, float y, float w, const std::wstring &label) {
        p.text(label, rect(x, editorY(y - 27), w, editorY(22)), 12, p.theme.muted, 500);
        p.fill(rect(x, editorY(y), w, editorY(44)), p.theme.field, 9);
        p.border(rect(x, editorY(y), w, editorY(44)),
                 GetFocus() == fields[index] ? p.theme.accent : p.theme.border, 9);
        if (p.capture)
            p.text(controlText(fields[index]), rect(x + 12, editorY(y + 8), w - 24, editorY(28)), 14,
                   p.theme.text);
    }
    void drawEditor(Painter &p, float w, float) {
        auto fy = [&](float n) { return editorY(n); };
        auto t = p.theme;
        p.text(L"安排一个好好休息的时间", rect(32, fy(23), w - 64, fy(37)), 22, t.text, 650);
        p.text(L"提前一分钟提醒，取消只影响本次。", rect(33, fy(65), w - 66, fy(25)), 12, t.muted);
        field(p, 0, 32, 123, w - 64, L"任务名称");
        p.text(L"执行方式", rect(33, fy(190), w - 66, fy(22)), 12, t.muted, 500);
        float bw = (w - 80) / 3;
        for (int i = 0; i < 3; i++) {
            Kind kind = i == 0 ? Kind::Once : i == 1 ? Kind::Weekly : Kind::Countdown;
            p.button(rect(32 + i * (bw + 8), fy(219), bw, fy(36)), EditorOnce + i,
                     i == 0   ? L"执行一次"
                     : i == 1 ? L"按星期重复"
                              : L"倒计时",
                     false, draft.kind == kind);
        }
        if (draft.kind == Kind::Once) {
            field(p, 1, 32, 293, (w - 80) / 2, L"日期 · YYYY-MM-DD");
            field(p, 2, w / 2 + 8, 293, (w - 80) / 2, L"时间 · 24 小时制");
        } else if (draft.kind == Kind::Weekly) {
            field(p, 2, 32, 293, w - 64, L"关机时间 · HH:MM");
            p.text(L"选择重复日期", rect(33, fy(353), w - 66, fy(22)), 12, t.muted, 500);
            float dayWidth = (w - 100) / 7;
            constexpr const wchar_t *labels[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
            for (int i = 0; i < 7; i++)
                p.button(rect(32 + i * (dayWidth + 6), fy(381), dayWidth, fy(36)), Monday + i, labels[i],
                         false, (draft.weekdays & (1 << i)) != 0);
            p.button(rect(32, fy(430), 90, fy(28)), EveryDay, L"每天");
            p.button(rect(132, fy(430), 90, fy(28)), WorkDays, L"工作日");
            p.button(rect(232, fy(430), 90, fy(28)), Weekend, L"周末");
        } else {
            field(p, 3, 32, 293, w - 64, L"多少分钟后关机");
            p.text(L"保存后从现在开始计时，提前一分钟提醒。", rect(33, fy(359), w - 66, fy(30)), 12, t.muted);
        }
        std::wstring summary;
        if (!editorError.empty())
            summary = editorError;
        else if (auto task = edited(false)) {
            if (task->kind == Kind::Weekly) {
                Engine probe(calendar);
                probe.upsert(*task, snapshot());
                if (auto *next = probe.next())
                    summary = L"下一次：" + formatTime(next->due, true);
            } else
                summary = L"下一次：" + formatTime(task->onceDue, true);
        } else
            summary = L"填写有效时间后，将显示下一次执行时间。";
        p.text(summary, rect(33, fy(473), w - 66, fy(28)), 11, editorError.empty() ? t.good : 0xD56F76, 500);
        p.line(32, fy(515), w - 32, fy(515), t.border);
        p.button(rect(w - 263, fy(529), 103, fy(35)), EditorCancel, L"取消");
        p.button(rect(w - 148, fy(529), 116, fy(35)), EditorSave, L"保存任务", true);
    }
    void drawWarning(Painter &p, float w, float h) {
        auto t = p.theme;
        auto now = snapshot();
        Time left = preview ? std::max(Time{0}, previewDeadline - now.steady) : engine.remaining(now);
        p.fill(rect(0, 0, w, h), t.card, 16);
        p.border(rect(.5f, .5f, w - 1, h - 1), t.border, 16);
        p.fill(rect(22, 20, 35, 35), t.accent, 11, .14f);
        p.circle(39, 37, 9, t.accent);
        p.circle(44, 33, 8, t.card);
        p.text(L"晚安", rect(68, 18, 96, 25), 15, t.text, 600);
        auto caption = [&](const Group &group) {
            std::wstring name = L"定时关机";
            for (const auto &occurrence : engine.state.pending)
                if (!group.keys.empty() && occurrence.key == group.keys.front())
                    if (const auto *task = engine.task(occurrence.taskId)) {
                        name = wide(task->name);
                        break;
                    }
            if (group.keys.size() > 1)
                name += L" 等 " + std::to_wstring(group.keys.size()) + L" 个任务";
            return name;
        };
        std::wstring warningName = L"提醒预览";
        if (!preview && engine.active)
            warningName = caption(*engine.active) + L" · " + formatTime(engine.active->due);
        p.text(warningName, rect(69, 43, w - 189, 17), 10, t.muted);
        p.fill(rect(w - 109, 24, 85, 26), t.good, 13, .10f);
        p.text(preview ? L"不会关机" : L"即将关机", rect(w - 109, 24, 85, 26), 11, t.good, 500,
               DWRITE_TEXT_ALIGNMENT_CENTER);
        p.text(L"电脑将在", rect(24, 79, 120, 28), 16, t.text, 500);
        p.text(std::to_wstring((left + 999) / 1000), rect(24, 110, 122, 65), 54, t.text, 300,
               DWRITE_TEXT_ALIGNMENT_LEADING, true);
        p.text(L"秒后关机", rect(125, 132, 135, 34), 18, t.text, 500);
        p.ring(w - 62, 131, 33, 1, t.border, 4);
        p.ring(w - 62, 131, 33, float(left) / Minute, t.good, 4);
        std::wstring sub =
            preview ? L"这是提醒预览，不会执行关机或修改任务。" : L"取消只影响这一次，重复任务仍会继续。";
        p.text(sub, rect(25, 180, w - 50, 25), 11, t.muted);
        if (!preview && !engine.otherWarnings.empty()) {
            auto upcoming = std::min_element(
                engine.otherWarnings.begin(), engine.otherWarnings.end(),
                [](const auto &a, const auto &b) { return a.monotonicDue < b.monotonicDue; });
            auto seconds = std::max(Time{0}, (upcoming->monotonicDue - now.steady + 999) / 1000);
            p.fill(rect(23, 212, w - 46, 35), t.good, 8, .07f);
            std::wstring label = L"另有 " + std::to_wstring(engine.otherWarnings.size()) + L" 组 · " +
                                 std::to_wstring(seconds) + L" 秒后 · " + caption(*upcoming);
            p.text(label, rect(33, 214, w - 120, 30), 10, t.good);
            p.button(rect(w - 83, 215, 52, 28), CycleAlert, L"切换");
        }
        p.button(rect(23, h - 53, 190, 35), CancelCurrent, preview ? L"关闭预览" : L"取消本次", true);
        p.button(rect(225, h - 53, w - 281, 35), Snooze10, L"推迟 10 分钟");
        p.button(rect(w - 49, h - 53, 26, 35), SnoozeMenu, L"▾");
    }
    void draw(Surface &s, Painter &p, float w, float h) {
        s.hits.clear();
        p.rt->Clear(color(p.theme.bg));
        if (&s == &main) {
            header(p, w);
            if (page == 0)
                drawHome(p, w, h);
            else if (page == 1)
                drawSettings(p, w, h);
            else
                drawHistory(p, w, h);
            if (!banner.empty()) {
                p.fill(rect(28, h - 104, w - 56, 32), dark() ? 0x473444 : 0xFFF0E8, 9);
                p.text(banner, rect(39, h - 102, w - 78, 28), 11, dark() ? 0xF5C59D : 0x956038);
            }
            if (simulate)
                p.text(L"模拟模式 · 不会真正关机", rect(28, h - 17, w - 56, 15), 9, p.theme.muted, 400,
                       DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else if (&s == &editor)
            drawEditor(p, w, h);
        else
            drawWarning(p, w, h);
    }
    void paint(Surface &s) {
        PAINTSTRUCT ps;
        BeginPaint(s.window, &ps);
        RECT size{};
        GetClientRect(s.window, &size);
        if (!s.target && size.right > 0 && size.bottom > 0) {
            auto prop = D2D1::RenderTargetProperties();
            prop.dpiX = prop.dpiY = 96 * s.scale;
            graphics->CreateHwndRenderTarget(
                prop, D2D1::HwndRenderTargetProperties(s.window, D2D1::SizeU(size.right, size.bottom)),
                &s.target);
        }
        if (s.target) {
            s.target->BeginDraw();
            Painter p(s.target, graphics, writer, s, theme());
            draw(s, p, s.width(), s.height());
            HRESULT status = s.target->EndDraw();
            if (status == D2DERR_RECREATE_TARGET)
                s.discard();
        }
        EndPaint(s.window, &ps);
    }

    bool capture(Surface &s, const std::filesystem::path &file, float scale = 1) {
        UINT width = UINT(std::ceil(s.width() * scale)), height = UINT(std::ceil(s.height() * scale));
        if (!width || !height)
            return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -int(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        HDC dc = CreateCompatibleDC(nullptr);
        void *pixels = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        HGDIOBJ old = SelectObject(dc, bitmap);
        ID2D1DCRenderTarget *target = nullptr;
        auto prop = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96 * scale, 96 * scale);
        HRESULT hr = graphics->CreateDCRenderTarget(&prop, &target);
        RECT bounds{0, 0, LONG(width), LONG(height)};
        if (SUCCEEDED(hr))
            hr = target->BindDC(dc, &bounds);
        if (SUCCEEDED(hr)) {
            target->SetDpi(96 * scale, 96 * scale);
            target->BeginDraw();
            Painter p(target, graphics, writer, s, theme(), true);
            draw(s, p, s.width(), s.height());
            hr = target->EndDraw();
        }
        IWICImagingFactory *imaging = nullptr;
        IWICBitmap *image = nullptr;
        IWICStream *stream = nullptr;
        IWICBitmapEncoder *encoder = nullptr;
        IWICBitmapFrameEncode *frame = nullptr;
        IPropertyBag2 *properties = nullptr;
        if (SUCCEEDED(hr))
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&imaging));
        if (SUCCEEDED(hr))
            hr = imaging->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &image);
        if (SUCCEEDED(hr))
            hr = imaging->CreateStream(&stream);
        if (SUCCEEDED(hr))
            hr = stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr))
            hr = imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr))
            hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr))
            hr = encoder->CreateNewFrame(&frame, &properties);
        if (SUCCEEDED(hr))
            hr = frame->Initialize(properties);
        if (SUCCEEDED(hr))
            hr = frame->SetSize(width, height);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
        if (SUCCEEDED(hr))
            hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr))
            hr = frame->WriteSource(image, nullptr);
        if (SUCCEEDED(hr))
            hr = frame->Commit();
        if (SUCCEEDED(hr))
            hr = encoder->Commit();
        release(properties);
        release(frame);
        release(encoder);
        release(stream);
        release(image);
        release(imaging);
        release(target);
        SelectObject(dc, old);
        DeleteObject(bitmap);
        DeleteDC(dc);
        return SUCCEEDED(hr);
    }
    bool key(MSG &message) {
        if (message.message != WM_KEYDOWN)
            return false;
        Surface *s = editor.window ? &editor : &main;
        if (!IsWindowVisible(s->window))
            return false;
        if (message.wParam == VK_ESCAPE) {
            if (editor.window)
                closeEditor();
            else
                ShowWindow(main.window, SW_HIDE);
            return true;
        }
        if (message.wParam == 'N' && (GetKeyState(VK_CONTROL) & 0x8000) && !editor.window) {
            openEditor();
            return true;
        }
        if (message.wParam == VK_TAB) {
            struct Entry {
                float y, x;
                int id;
                HWND native;
            };
            std::vector<Entry> order;
            for (auto hit : s->hits)
                order.push_back({hit.bounds.top, hit.bounds.left, hit.id, nullptr});
            if (s == &editor)
                for (auto f : fields)
                    if (f && IsWindowVisible(f)) {
                        RECT r;
                        GetWindowRect(f, &r);
                        POINT pt{r.left, r.top};
                        ScreenToClient(editor.window, &pt);
                        order.push_back({pt.y / editor.scale, pt.x / editor.scale, 0, f});
                    }
            std::sort(order.begin(), order.end(),
                      [](const auto &a, const auto &b) { return a.y == b.y ? a.x < b.x : a.y < b.y; });
            if (order.empty())
                return true;
            int current = -1;
            for (size_t i = 0; i < order.size(); i++)
                if ((order[i].native && order[i].native == GetFocus()) ||
                    (!order[i].native && order[i].id == s->focus && GetFocus() == s->window))
                    current = int(i);
            int next = (current + ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1) + int(order.size())) %
                       int(order.size());
            s->focus = order[next].id;
            SetFocus(order[next].native ? order[next].native : s->window);
            InvalidateRect(s->window, nullptr, FALSE);
            return true;
        }
        if (message.wParam == VK_RETURN || message.wParam == VK_SPACE) {
            if (GetFocus() == s->window && s->focus) {
                command(s->focus, s->window);
                return true;
            }
            if (message.wParam == VK_RETURN && s == &editor) {
                command(EditorSave, s->window);
                return true;
            }
        }
        return false;
    }
    void setupSmoke() {
        engine.state = State{};
        engine.state.settings.sound = false;
        engine.state.settings.theme = "light";
        auto now = snapshot();
        auto local = calendar.local(now.wall);
        Task a;
        a.id = "daily-demo";
        a.name = "今晚，早点休息";
        a.minute = 23 * 60;
        engine.upsert(a, now);
        Task b;
        b.id = "weekend-demo";
        b.name = "周末慢一点";
        b.minute = 23 * 60 + 30;
        b.weekdays = 96;
        engine.upsert(b, now);
        Task c;
        c.id = "once-demo";
        c.name = "阅读结束后关机";
        c.kind = Kind::Once;
        c.day = local.day + 1;
        c.minute = 22 * 60 + 30;
        c.onceDue = calendar.resolve(c.day, c.minute).value();
        engine.upsert(c, now);
        engine.state.logs = {
            {"demo1", "今晚，早点休息", "取消只影响本次", now.wall - 600000, now.wall, Status::Cancelled},
            {"demo2", "阅读结束后关机", "推迟 10 分钟", now.wall - 1200000, now.wall, Status::Snoozed},
            {"demo3", "周末慢一点", "错过执行时间，已跳过", now.wall - Day, now.wall - Day, Status::Missed}};
        save();
    }
    void smokeCheck(bool v, const char *m) {
        ++smokeChecks;
        if (!v)
            throw std::runtime_error(m);
    }
    void runSmoke() {
        KillTimer(main.window, 99);
        try {
            smokeCheck(simulate, "smoke must simulate");
            std::filesystem::create_directories(smokeOutput);
            smokeCheck(capture(main, smokeOutput / L"home-light.png"), "light capture");
            smokeCheck(capture(main, smokeOutput / L"home-150.png", 1.5f), "150% capture");
            smokeCheck(capture(main, smokeOutput / L"home-200.png", 2), "200% capture");
            command(ThemeDark, main.window);
            smokeCheck(capture(main, smokeOutput / L"home-dark.png"), "dark capture");
            command(ThemeLight, main.window);
            command(NewTask, main.window);
            SetWindowTextW(fields[0], L"键盘输入 · 测试任务");
            smokeCheck(capture(editor, smokeOutput / L"editor.png"), "editor capture");
            size_t count = engine.state.tasks.size();
            command(EditorSave, editor.window);
            smokeCheck(!editor.window && engine.state.tasks.size() == count + 1, "editor saves");
            command(10001, main.window);
            smokeCheck(!engine.state.tasks[0].enabled, "toggle off");
            command(10001, main.window);
            smokeCheck(engine.state.tasks[0].enabled, "toggle on");
            command(PauseAll, main.window);
            smokeCheck(engine.state.settings.paused, "pause");
            command(PauseAll, main.window);
            smokeCheck(!engine.state.settings.paused, "resume");
            // Exercise the actual resizable window, including the empty compact layout.
            RECT originalBounds{};
            GetWindowRect(main.window, &originalBounds);
            SetWindowPos(main.window, nullptr, 0, 0, int(660 * main.scale), int(440 * main.scale),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            smokeCheck(capture(main, smokeOutput / L"home-small.png"), "small layout capture");
            State savedState = engine.state;
            engine.state = State{};
            smokeCheck(capture(main, smokeOutput / L"welcome-small.png"), "small empty layout capture");
            engine.state = std::move(savedState);
            SetWindowPos(main.window, nullptr, 0, 0, originalBounds.right - originalBounds.left,
                         originalBounds.bottom - originalBounds.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            command(Preferences, main.window);
            smokeCheck(capture(main, smokeOutput / L"settings.png"), "settings capture");
            command(History, main.window);
            smokeCheck(capture(main, smokeOutput / L"history.png"), "history capture");
            size_t pending = engine.state.pending.size();
            command(Preview, main.window);
            smokeCheck(capture(warning, smokeOutput / L"warning-preview.png"), "preview capture");
            command(CancelCurrent, warning.window);
            smokeCheck(!preview && engine.state.pending.size() == pending, "preview cannot mutate");
            auto now = snapshot();
            Task imminent;
            imminent.id = "imminent";
            imminent.name = "一分钟模拟关机";
            imminent.kind = Kind::Once;
            imminent.onceDue = now.wall + 30000;
            auto local = calendar.local(imminent.onceDue);
            imminent.day = local.day;
            imminent.minute = local.minute;
            engine.upsert(imminent, now);
            afterMutation();
            smokeCheck(engine.active.has_value(), "warning connected");
            smokeCheck(capture(warning, smokeOutput / L"warning.png"), "warning capture");
            command(CancelCurrent, warning.window);
            smokeCheck(engine.task("imminent")->completed && !engine.active, "cancel popup");
            auto concurrentNow = snapshot();
            for (int i = 0; i < 2; ++i) {
                Task alert;
                alert.id = "parallel-" + std::to_string(i);
                alert.name = i == 0 ? "先提醒的任务" : "另一项独立任务";
                alert.kind = Kind::Countdown;
                alert.minutes = 1;
                alert.onceDue = concurrentNow.wall + 20000 + i * 15000;
                auto stamp = calendar.local(alert.onceDue);
                alert.day = stamp.day;
                alert.minute = stamp.minute;
                engine.upsert(alert, concurrentNow);
            }
            afterMutation();
            smokeCheck(engine.active && engine.otherWarnings.size() == 1, "parallel warning connected");
            smokeCheck(capture(warning, smokeOutput / L"warning-parallel.png"), "parallel warning capture");
            command(CycleAlert, warning.window);
            smokeCheck(engine.active && engine.active->keys.front().starts_with("parallel-1/"),
                       "parallel warning switch");
            command(CancelCurrent, warning.window);
            smokeCheck(engine.task("parallel-1")->completed && engine.active &&
                           engine.active->keys.front().starts_with("parallel-0/"),
                       "parallel cancellation preserves other group");
            command(CancelCurrent, warning.window);
            Engine restarted(calendar);
            restarted.state = storage.load().state;
            restarted.tick(snapshot(), Reconcile::Startup);
            smokeCheck(restarted.task("imminent")->completed && !restarted.active, "popup persistence");
            // Safety checks are confined to the simulated, isolated profile.
            size_t before = engine.state.tasks.size();
            openEditor(nullptr, true);
            SetWindowTextW(fields[3], L"0");
            command(EditorSave, editor.window);
            smokeCheck(editor.window && engine.state.tasks.size() == before, "invalid countdown rejected");
            SetWindowTextW(fields[3], L"90");
            smokeCheck(editorError.empty(), "input change clears stale validation");
            smokeCheck(capture(editor, smokeOutput / L"editor-countdown.png"), "countdown editor capture");
            MSG enter{};
            enter.message = WM_KEYDOWN;
            enter.wParam = VK_RETURN;
            enter.hwnd = fields[3];
            SetFocus(fields[3]);
            key(enter);
            smokeCheck(!editor.window && engine.state.tasks.size() == before + 1, "keyboard saves countdown");
            openEditor();
            command(EditorOnce, editor.window);
            SetWindowTextW(fields[1], L"2026-02-30");
            command(EditorSave, editor.window);
            smokeCheck(editor.window && !editorError.empty(), "invalid date rejected");
            SetWindowTextW(fields[1], dateText(calendar.local(snapshot().wall).day + 1).c_str());
            SetWindowTextW(fields[2], L"25:00");
            command(EditorSave, editor.window);
            smokeCheck(editor.window && !editorError.empty(), "invalid clock rejected");
            SetWindowTextW(fields[2], L"22:00");
            editorError.clear();
            smokeCheck(capture(editor, smokeOutput / L"editor-once.png"), "one-off editor capture");
            command(EditorSave, editor.window);
            smokeCheck(!editor.window, "one-off editor saves");
            auto addImminent = [&](const std::string &id) {
                auto time = snapshot();
                Task t;
                t.id = id;
                t.name = id;
                t.kind = Kind::Once;
                t.onceDue = time.wall + 30000;
                auto stamp = calendar.local(t.onceDue);
                t.day = stamp.day;
                t.minute = stamp.minute;
                engine.upsert(t, time);
                afterMutation();
                smokeCheck(engine.active.has_value(), "test warning armed");
                engine.active->monotonicDue = snapshot().steady - 1;
            };
            int calls = dispatchCount;
            addImminent("simulated-dispatch");
            pump();
            smokeCheck(dispatchCount == calls + 1 && engine.task("simulated-dispatch")->completed,
                       "one simulated OS request");
            smokeCheck(engine.state.logs.front().status == Status::Requested, "simulated result recorded");
            addImminent("failed-commit");
            auto original = storage.directory;
            auto blocker = smokeOutput / L"blocker";
            {
                std::ofstream file(blocker);
                file << "blocked";
            }
            storage.directory = blocker;
            calls = dispatchCount;
            pump();
            smokeCheck(blocked && engine.state.settings.paused && dispatchCount == calls,
                       "failed save cannot dispatch");
            smokeCheck(engine.state.logs.front().status == Status::Failed, "failed commit recorded");
            storage.directory = original;
            command(RetrySave, main.window);
            smokeCheck(!blocked && engine.state.settings.paused, "retry stays paused");
            command(Home, main.window);
            std::ofstream report(smokeOutput / L"smoke-results.json");
            report << "{\"passed\":true,\"simulate\":true,\"checks\":" << smokeChecks << "}";
            command(Quit, main.window);
        } catch (const std::exception &e) {
            std::ofstream report(smokeOutput / L"smoke-results.json");
            report << "{\"passed\":false,\"checks\":" << smokeChecks << ",\"error\":\"" << e.what() << "\"}";
            command(Quit, main.window);
            PostQuitMessage(1);
        }
    }
};
static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto *s = reinterpret_cast<Surface *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        s = static_cast<Surface *>(reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
        s->window = window;
        s->scale = windowDpi(window) / 96.f;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s || !application)
        return DefWindowProcW(window, message, wParam, lParam);
    auto &app = *application;
    if (app.taskbarCreated && message == app.taskbarCreated) {
        app.addTray();
        return 0;
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        app.paint(*s);
        return 0;
    case WM_SIZE:
        if (s->target)
            s->target->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
        if (s == &app.editor)
            app.positionFields();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        s->scale = HIWORD(wParam) / 96.f;
        s->discard();
        auto *r = reinterpret_cast<RECT *>(lParam);
        SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (s == &app.editor)
            app.positionFields();
        if (s == &app.warning)
            app.placeWarning();
        return 0;
    }
    case WM_GETMINMAXINFO:
        if (s == &app.main) {
            auto *info = reinterpret_cast<MINMAXINFO *>(lParam);
            info->ptMinTrackSize = {LONG(660 * s->scale), LONG(440 * s->scale)};
        }
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        int hit = s->hit(GET_X_LPARAM(lParam) / s->scale, GET_Y_LPARAM(lParam) / s->scale);
        if (hit != s->hover) {
            s->hover = hit;
            InvalidateRect(window, nullptr, FALSE);
        }
        SetCursor(LoadCursorW(nullptr, hit ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        s->hover = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int id = s->hit(GET_X_LPARAM(lParam) / s->scale, GET_Y_LPARAM(lParam) / s->scale);
        if (id) {
            s->focus = id;
            if (s != &app.warning)
                SetFocus(window);
            app.command(id, window);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (s == &app.main) {
            s->scroll =
                std::clamp(s->scroll - GET_WHEEL_DELTA_WPARAM(wParam) / 120.f * 64, 0.f, s->maxScroll);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_COMMAND:
        if (s == &app.editor && HIWORD(wParam) == EN_CHANGE) {
            app.editorError.clear();
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT: {
        auto t = app.theme();
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB((t.text >> 16) & 255, (t.text >> 8) & 255, t.text & 255));
        SetBkColor(dc, RGB((t.field >> 16) & 255, (t.field >> 8) & 255, t.field & 255));
        return reinterpret_cast<LRESULT>(app.fieldBrush);
    }
    case WM_MOUSEACTIVATE:
        if (s == &app.warning)
            return MA_NOACTIVATE;
        break;
    case WM_SHOWWINDOW:
        if (s == &app.main) {
            if (wParam)
                SetTimer(window, 2, 1000, nullptr);
            else {
                KillTimer(window, 2);
                s->discard();
            }
        } else if (!wParam)
            s->discard();
        break;
    case WM_TIMER:
        if (s == &app.main) {
            if (wParam == 1)
                app.pump();
            else if (wParam == 2 && IsWindowVisible(window))
                InvalidateRect(window, nullptr, FALSE);
            else if (wParam == 3) {
                if (app.preview && snapshot().steady >= app.previewDeadline)
                    app.hideWarning();
                else if (app.warning.window)
                    InvalidateRect(app.warning.window, nullptr, FALSE);
            } else if (wParam == 99)
                app.runSmoke();
        } else if (s == &app.warning && wParam == 4) {
            app.warningProgress = std::min(1.f, float(snapshot().steady - app.warningAnimation) / 180.f);
            app.placeWarning();
            if (app.warningProgress >= 1)
                KillTimer(window, 4);
        }
        return 0;
    case WM_TIMECHANGE:
        _tzset();
        if (s == &app.main)
            app.pump(Reconcile::TimeChange);
        return 0;
    case WM_POWERBROADCAST:
        if (s == &app.main && (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND))
            app.pump(Reconcile::Resume);
        return TRUE;
    case WM_SETTINGCHANGE:
        app.updateTheme();
        if (s == &app.main && lParam &&
            _wcsicmp(reinterpret_cast<const wchar_t *>(lParam), L"TimeZoneInformation") == 0) {
            _tzset();
            app.pump(Reconcile::TimeChange);
        }
        return 0;
    case WM_DISPLAYCHANGE:
        app.placeWarning();
        return 0;
    case WM_CLOSE:
        if (s == &app.main)
            ShowWindow(window, SW_HIDE);
        else if (s == &app.editor)
            app.closeEditor();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam && s == &app.main)
            app.removeTray();
        return 0;
    case WM_DESTROY:
        s->discard();
        if (s == &app.main) {
            app.removeTray();
            PostQuitMessage(0);
        }
        return 0;
    case ShowMessage:
        app.showMain();
        return 0;
    case TrayMessage: {
        auto e = LOWORD(lParam);
        if (e == WM_CONTEXTMENU || e == WM_RBUTTONUP)
            app.trayMenu();
        else if (e == NIN_SELECT || e == NIN_KEYSELECT || e == WM_LBUTTONDBLCLK)
            app.showMain();
        return 0;
    }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    configureDpi();
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool simulate = false, tray = false, smoke = false, benchmark = false, explicitData = false;
    std::filesystem::path data = defaultDataDirectory(), output;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--simulate")
            simulate = true;
        else if (a == L"--tray")
            tray = true;
        else if (a == L"--benchmark") {
            benchmark = true;
            simulate = true;
        } else if (a == L"--data-dir") {
            if (i + 1 >= argc) {
                LocalFree(argv);
                if (SUCCEEDED(initialized))
                    CoUninitialize();
                return 2;
            }
            data = argv[++i];
            explicitData = true;
        } else if (a == L"--smoke-test") {
            if (i + 1 >= argc) {
                LocalFree(argv);
                if (SUCCEEDED(initialized))
                    CoUninitialize();
                return 2;
            }
            smoke = true;
            simulate = true;
            output = std::filesystem::absolute(argv[++i]);
            data = output / L"data";
        }
    }
    LocalFree(argv);
    if (simulate && !explicitData && !smoke)
        data = defaultDataDirectory() / L"Simulation";
    data = std::filesystem::absolute(data).lexically_normal();
    int result = 0;
    {
        App app(instance, data);
        application = &app;
        app.simulate = simulate;
        app.benchmark = benchmark;
        app.trayStart = tray;
        app.smoke = smoke;
        app.smokeOutput = output;
        if (app.initialize()) {
            MSG m{};
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                if (!app.key(m)) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
            }
            result = int(m.wParam);
        }
        application = nullptr;
    }
    if (SUCCEEDED(initialized))
        CoUninitialize();
    return result;
}
