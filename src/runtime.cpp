#include "runtime.hpp"
#include <bcrypt.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <sddl.h>
#include <set>
#include <stdexcept>

namespace evening {
namespace {
constexpr DWORD MaxFrame = 4 * 1024 * 1024;
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
struct StreamReset {
    std::mutex &mutex;
    HANDLE &stream;
    ~StreamReset() {
        std::lock_guard guard(mutex);
        stream = INVALID_HANDLE_VALUE;
    }
};
std::wstring tokenSid(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> bytes(size);
    if (!size || !GetTokenInformation(token, TokenUser, bytes.data(), size, &size))
        return {};
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(bytes.data())->User.Sid, &text))
        return {};
    std::wstring result(text);
    LocalFree(text);
    return result;
}
bool peerIsOwner(HANDLE pipe, bool server) {
    ULONG pid = 0;
    if (!(server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid)))
        return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    HANDLE raw = nullptr;
    if (!process.value || !OpenProcessToken(process.value, TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    return tokenSid(token.value) == ownerSid();
}
bool shutdownPrivilegeAvailable() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        return false;
    Handle token(raw);
    LUID wanted{};
    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &wanted))
        return false;
    DWORD size = 0;
    GetTokenInformation(token.value, TokenPrivileges, nullptr, 0, &size);
    std::vector<BYTE> bytes(size);
    if (!size || !GetTokenInformation(token.value, TokenPrivileges, bytes.data(), size, &size))
        return false;
    auto *privileges = reinterpret_cast<TOKEN_PRIVILEGES *>(bytes.data());
    for (DWORD i = 0; i < privileges->PrivilegeCount; ++i)
        if (privileges->Privileges[i].Luid.LowPart == wanted.LowPart &&
            privileges->Privileges[i].Luid.HighPart == wanted.HighPart)
            return true;
    return false;
}
bool transfer(HANDLE pipe, void *data, DWORD size, bool writing, DWORD timeout) {
    auto *bytes = static_cast<BYTE *>(data);
    ULONGLONG end = GetTickCount64() + timeout;
    while (size) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.value)
            return false;
        OVERLAPPED overlapped{};
        overlapped.hEvent = event.value;
        DWORD count = 0;
        BOOL ok = writing ? WriteFile(pipe, bytes, size, &count, &overlapped)
                          : ReadFile(pipe, bytes, size, &count, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD remaining = static_cast<DWORD>(
                std::min<ULONGLONG>(timeout, end > GetTickCount64() ? end - GetTickCount64() : 0));
            if (WaitForSingleObject(event.value, remaining) != WAIT_OBJECT_0) {
                CancelIoEx(pipe, &overlapped);
                GetOverlappedResult(pipe, &overlapped, &count, TRUE);
                return false;
            }
            ok = GetOverlappedResult(pipe, &overlapped, &count, FALSE);
        }
        if (!ok || !count)
            return false;
        bytes += count;
        size -= count;
    }
    return true;
}
bool writeFrame(HANDLE pipe, const Json &message, DWORD timeout = 2000) {
    auto text = message.dump();
    if (text.size() > MaxFrame)
        return false;
    DWORD size = static_cast<DWORD>(text.size());
    return transfer(pipe, &size, sizeof(size), true, timeout) &&
           transfer(pipe, text.data(), size, true, timeout);
}
Json readFrame(HANDLE pipe, DWORD timeout = 8000) {
    DWORD size = 0;
    if (!transfer(pipe, &size, sizeof(size), false, timeout) || !size || size > MaxFrame)
        throw std::runtime_error("后台连接已断开或消息长度无效");
    std::string text(size, '\0');
    if (!transfer(pipe, text.data(), size, false, timeout))
        throw std::runtime_error("后台响应超时");
    return Json::parse(text);
}
HANDLE connectPipe(const std::filesystem::path &data, DWORD timeout) {
    auto name = pipeName(data);
    if (!WaitNamedPipeW(name.c_str(), timeout))
        return INVALID_HANDLE_VALUE;
    HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (h != INVALID_HANDLE_VALUE && !peerIsOwner(h, false)) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}
Json groupJson(const Group &g) {
    return {{"keys", g.keys}, {"due", g.due}, {"monotonicDue", g.monotonicDue}};
}
class Runtime {
  public:
    WindowsCalendar calendar;
    Engine engine{calendar};
    Storage storage;
    Handle lock;
    HWND window = nullptr;
    bool simulate = false, blocked = false;
    std::string notice;
    std::string epoch = newId();
    bool shutdownAvailable = shutdownPrivilegeAvailable();
    std::mutex stateMutex, wakeMutex;
    std::condition_variable changed;
    std::atomic_bool stopping{false};
    std::atomic_bool fatal{false};
    std::atomic_uint64_t publishedVersion{0};
    std::atomic_uint workers{0};
    std::thread listener;
    Runtime(const std::filesystem::path &data, bool test) : storage(data), simulate(test) {}
    bool save() {
        ++engine.state.version;
        std::wstring error;
        if (storage.save(engine.state, error)) {
            blocked = false;
            publishedVersion = engine.state.version;
            return true;
        }
        blocked = true;
        engine.state.settings.paused = true;
        engine.clearWarnings();
        notice = utf8(error);
        publishedVersion = engine.state.version;
        return false;
    }
    Json snapshotJson() {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        Json groups = Json::array();
        if (engine.active)
            groups.push_back(groupJson(*engine.active));
        for (const auto &g : engine.otherWarnings)
            groups.push_back(groupJson(g));
        return {{"protocol", ProtocolVersion},
                {"release", ReleaseVersion},
                {"epoch", epoch},
                {"state", encode(engine.state)},
                {"groups", groups},
                {"blocked", blocked},
                {"notice", notice},
                {"session", session},
                {"simulate", simulate},
                {"shutdownPrivilege", shutdownAvailable},
                {"steady", snapshot().steady}};
    }
    void tick(Reconcile why = Reconcile::Normal) {
        std::lock_guard guard(stateMutex);
        tickLocked(why);
    }
    void tickLocked(Reconcile why = Reconcile::Normal) {
        if (blocked || stopping) {
            KillTimer(window, 1);
            return;
        }
        auto result = engine.tick(snapshot(), why);
        if (result.changed && !save()) {
            engine.result(result.dispatch, false, "状态保存失败，没有提交系统请求");
            changed.notify_all();
            return;
        }
        if (!result.dispatch.empty()) {
            auto allowed = engine.authorizeDispatch(result.dispatch, snapshot());
            if (!allowed.empty()) {
                auto power = requestShutdown(engine.state.settings.force, simulate);
                engine.result(allowed, power.accepted, utf8(power.message), power.requestedAt);
                notice = utf8(power.message);
            }
            save();
        }
        if (result.changed)
            changed.notify_all();
        SetTimer(window, 1, static_cast<UINT>(engine.nextWait(snapshot())), nullptr);
    }
    bool selectGroup(const Json &keys) {
        auto expected = keys.get<std::vector<std::string>>();
        if (engine.active && engine.active->keys == expected)
            return true;
        for (auto &g : engine.otherWarnings)
            if (g.keys == expected) {
                std::swap(*engine.active, g);
                return true;
            }
        return false;
    }
    Json command(const Json &request) {
        std::lock_guard guard(stateMutex);
        Json reply = {{"ok", false}, {"requestId", request.value("requestId", std::string{})}};
        try {
            if (request.at("protocol") != ProtocolVersion || request.at("release") != ReleaseVersion)
                throw std::runtime_error("界面与后台版本不一致，请退出旧版并重新启动");
            auto op = request.at("op").get<std::string>();
            if (op == "snapshot") {
                reply["ok"] = true;
                reply["snapshot"] = snapshotJson();
                return reply;
            }
            if (stopping)
                throw std::runtime_error("后台正在停止，请稍后重新连接");
            if (request.at("version").get<uint64_t>() != engine.state.version)
                throw std::runtime_error("后台状态已更新，请刷新后重试");
            if (blocked && op != "retry")
                throw std::runtime_error("数据保存失败，后台已暂停；请先重试保存");
            auto now = snapshot();
            if (op == "upsert") {
                auto candidate = encode(engine.state);
                candidate["tasks"] = Json::array({request.at("task")});
                candidate["pending"] = Json::array();
                auto value = decode(candidate).tasks.front();
                const auto *existing = engine.task(value.id);
                if (request.value("mustExist", false) && !existing)
                    throw std::runtime_error("任务已被删除，请关闭编辑器");
                if (existing && existing->editVersion != value.editVersion)
                    throw std::runtime_error("任务已在后台变更，请点击重新载入后再编辑");
                if (!engine.task(value.id) && engine.state.tasks.size() >= 512)
                    throw std::runtime_error("任务数量达到上限");
                if (value.kind != Kind::Weekly && !value.completed && value.onceDue <= now.wall)
                    throw std::runtime_error("任务时间必须在未来");
                engine.upsert(value, now);
            } else if (op == "enable" || op == "erase") {
                auto id = request.at("id").get<std::string>();
                if (!engine.task(id))
                    throw std::runtime_error("任务不存在");
                if (op == "enable")
                    engine.enable(id, request.at("enabled").get<bool>(), now);
                else
                    engine.erase(id, now);
            } else if (op == "pause")
                engine.pause(now, request.at("paused").get<bool>());
            else if (op == "cancel" || op == "snooze") {
                if (!selectGroup(request.at("keys")))
                    throw std::runtime_error("本次提醒已结束，不能再操作");
                if (op == "cancel")
                    engine.cancel(now);
                else {
                    int minutes = request.at("minutes");
                    if (minutes != 10 && minutes != 30 && minutes != 60)
                        throw std::runtime_error("推迟时间无效");
                    engine.snooze(now, minutes);
                }
            } else if (op == "settings") {
                auto candidate = encode(engine.state);
                auto &settings = candidate["settings"];
                for (auto key : {"sound", "force", "theme"})
                    if (request.at("settings").contains(key))
                        settings[key] = request["settings"][key];
                engine.state.settings = decode(candidate).settings;
            } else if (op == "login-start") {
                if (simulate)
                    throw std::runtime_error("模拟模式禁止修改自启动");
                std::wstring error;
                bool enabled = request.at("enabled");
                bool previous = hasAutostart();
                if (!setAutostart(enabled, error))
                    throw std::runtime_error(utf8(error));
                engine.state.settings.autostart = enabled;
                if (!save()) {
                    setAutostart(previous, error);
                    engine.state.settings.autostart = previous;
                    throw std::runtime_error(notice);
                }
                tickLocked();
                reply["ok"] = !blocked;
                if (blocked)
                    reply["error"] = notice;
                reply["snapshot"] = snapshotJson();
                changed.notify_all();
                return reply;
            } else if (op == "background-record")
                engine.state.settings.background = request.at("enabled").get<bool>();
            else if (op != "retry" && op != "stop")
                throw std::runtime_error("未知后台命令");
            if (!save())
                throw std::runtime_error(notice);
            if (op != "stop") {
                tickLocked();
                if (blocked)
                    throw std::runtime_error(notice);
            }
            reply["ok"] = true;
            if (op == "stop") {
                stopping = true;
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
        } catch (const std::exception &error) {
            reply["error"] = error.what();
        }
        reply["snapshot"] = snapshotJson();
        changed.notify_all();
        return reply;
    }
    struct Work {
        Runtime *runtime;
        HANDLE pipe;
    };
    static DWORD WINAPI serve(void *raw) {
        std::unique_ptr<Work> work(static_cast<Work *>(raw));
        Runtime &r = *work->runtime;
        Handle pipe(work->pipe);
        try {
            if (!peerIsOwner(pipe.value, true))
                throw std::runtime_error("身份验证失败");
            auto request = readFrame(pipe.value, 3000);
            bool subscribe = request.value("subscribe", false);
            auto reply = r.command(request);
            if (!writeFrame(pipe.value, reply))
                throw std::runtime_error("断开");
            // DisconnectNamedPipe discards unread bytes. A bounded acknowledgement
            // keeps short replies intact without an unbounded FlushFileBuffers wait.
            if (!subscribe || !reply.value("ok", false))
                readFrame(pipe.value, 1500);
            if (subscribe && reply.value("ok", false)) {
                uint64_t version = reply["snapshot"]["state"]["version"];
                auto heartbeat = GetTickCount64();
                while (!r.stopping) {
                    std::unique_lock wait(r.wakeMutex);
                    r.changed.wait_for(wait, std::chrono::seconds(4),
                                       [&] { return r.stopping || r.publishedVersion != version; });
                    wait.unlock();
                    Json state;
                    {
                        std::lock_guard guard(r.stateMutex);
                        state = r.snapshotJson();
                    }
                    auto next = state["state"]["version"].get<uint64_t>();
                    if (next != version || GetTickCount64() - heartbeat >= 4000) {
                        if (!writeFrame(pipe.value, {{"ok", true}, {"snapshot", state}}))
                            break;
                        version = next;
                        heartbeat = GetTickCount64();
                    }
                }
            }
        } catch (...) {
        }
        DisconnectNamedPipe(pipe.value);
        --r.workers;
        r.changed.notify_all();
        return 0;
    }
    void listen() {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        auto dacl = L"D:P(A;;GA;;;SY)(A;;GRGW;;;" + ownerSid() + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(dacl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                  nullptr)) {
            fatal = true;
            stopping = true;
            PostMessageW(window, WM_CLOSE, 0, 0);
            return;
        }
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        bool first = true;
        while (!stopping) {
            Handle pipe(CreateNamedPipeW(
                pipeName(storage.directory).c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 32, 65536,
                65536, 0, &attributes));
            first = false;
            if (pipe.value == INVALID_HANDLE_VALUE) {
                fatal = true;
                stopping = true;
                PostMessageW(window, WM_CLOSE, 0, 0);
                break;
            }
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            OVERLAPPED o{};
            o.hEvent = event.value;
            BOOL connected = ConnectNamedPipe(pipe.value, &o);
            DWORD error = connected ? ERROR_SUCCESS : GetLastError();
            if (!connected && error == ERROR_IO_PENDING) {
                while (!stopping && WaitForSingleObject(event.value, 250) == WAIT_TIMEOUT) {
                }
                if (stopping) {
                    CancelIoEx(pipe.value, &o);
                    DWORD count;
                    GetOverlappedResult(pipe.value, &o, &count, TRUE);
                    break;
                }
                DWORD count = 0;
                connected = GetOverlappedResult(pipe.value, &o, &count, FALSE);
            } else if (error == ERROR_PIPE_CONNECTED)
                connected = TRUE;
            if (!connected)
                continue;
            if (workers >= 31)
                continue;
            auto *work = new Work{this, pipe.value};
            ++workers;
            if (!QueueUserWorkItem(serve, work, WT_EXECUTELONGFUNCTION)) {
                --workers;
                delete work;
            } else
                pipe.value = INVALID_HANDLE_VALUE;
        }
        LocalFree(descriptor);
    }
};
LRESULT CALLBACK engineWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto *r = reinterpret_cast<Runtime *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        r = static_cast<Runtime *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        r->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(r));
    }
    if (r) {
        if (message == WM_TIMER || message == WM_APP + 1) {
            r->tick();
            return 0;
        }
        if (message == WM_TIMECHANGE) {
            _tzset();
            r->tick(Reconcile::TimeChange);
            return 0;
        }
        if (message == WM_SETTINGCHANGE && lp &&
            _wcsicmp(reinterpret_cast<LPCWSTR>(lp), L"TimeZoneInformation") == 0) {
            _tzset();
            r->tick(Reconcile::TimeChange);
            return 0;
        }
        if (message == WM_POWERBROADCAST && (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)) {
            r->tick(Reconcile::Resume);
            return TRUE;
        }
        if (message == WM_CLOSE) {
            r->stopping = true;
            r->changed.notify_all();
            DestroyWindow(window);
            return 0;
        }
        if (message == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        if (message == WM_QUERYENDSESSION)
            return TRUE;
    }
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace
std::wstring ownerSid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        throw std::runtime_error("无法读取当前账户");
    Handle token(raw);
    auto sid = tokenSid(token.value);
    if (sid.empty())
        throw std::runtime_error("无法识别当前账户");
    return sid;
}
std::wstring runtimeIdentity(const std::filesystem::path &data) {
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(data)).wstring();
    CharLowerBuffW(canonical.data(), static_cast<DWORD>(canonical.size()));
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("无法计算后台标识");
    BYTE bytes[32]{};
    auto code = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (code >= 0)
        code = BCryptHashData(hash, reinterpret_cast<PUCHAR>(canonical.data()),
                              static_cast<ULONG>(canonical.size() * sizeof(wchar_t)), 0);
    if (code >= 0)
        code = BCryptFinishHash(hash, bytes, sizeof(bytes), 0);
    if (hash)
        BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (code < 0)
        throw std::runtime_error("无法计算后台标识");
    std::wstring out = ownerSid() + L".";
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (auto byte : bytes) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}
std::wstring pipeName(const std::filesystem::path &data) {
    return L"\\\\.\\pipe\\Evenfall." + runtimeIdentity(data);
}
std::wstring quoteArgument(const std::wstring &value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"')
            out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
Json engineRequest(const std::filesystem::path &data, Json request, DWORD timeout) {
    Handle pipe(connectPipe(data, timeout));
    if (pipe.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("无法连接后台，请启动后台或退出旧版程序");
    request["protocol"] = request.value("protocol", ProtocolVersion);
    request["release"] = request.value("release", std::string(ReleaseVersion));
    request["requestId"] = request.value("requestId", newId());
    if (!writeFrame(pipe.value, request, timeout))
        throw std::runtime_error("后台命令未能送达");
    auto reply = readFrame(pipe.value, timeout);
    writeFrame(pipe.value, {{"ack", true}}, timeout);
    return reply;
}
int runEngine(const std::filesystem::path &data, bool simulate) {
    Runtime r(data, simulate);
    std::error_code error;
    std::filesystem::create_directories(data, error);
    if (error)
        return 2;
    r.lock.value = CreateFileW((data / L"instance.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (r.lock.value == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_SHARING_VIOLATION ? 0 : 2;
    auto loaded = r.storage.load();
    r.engine.state = std::move(loaded.state);
    r.publishedVersion = r.engine.state.version;
    r.notice = utf8(loaded.warning);
    if (!simulate)
        r.engine.state.settings.autostart = hasAutostart();
    WNDCLASSW cls{};
    cls.lpfnWndProc = engineWindow;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"Evenfall.Engine.1";
    RegisterClassW(&cls);
    if (!CreateWindowExW(0, cls.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, cls.hInstance,
                         &r))
        return 2;
    r.tick(Reconcile::Startup);
    r.listener = std::thread([&] { r.listen(); });
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    r.stopping = true;
    r.changed.notify_all();
    r.listener.join();
    while (r.workers) {
        std::unique_lock wait(r.wakeMutex);
        r.changed.wait_for(wait, std::chrono::milliseconds(100));
    }
    return r.fatal ? 2 : 0;
}
bool launchEngine(const std::filesystem::path &data, bool simulate, std::wstring &error) {
    try {
        if (!simulate && backgroundTaskExists(data))
            return runBackgroundTask(data, error);
    } catch (const std::exception &failure) {
        error = wide(failure.what());
        return false;
    }
    auto command = quoteArgument(executablePath()) + L" --engine --data-dir " +
                   quoteArgument(data.wstring()) + (simulate ? L" --simulate" : L"");
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &info)) {
        error = L"后台启动失败（错误 " + std::to_wstring(GetLastError()) + L"）";
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}
EngineConnection::~EngineConnection() {
    stop();
}
void EngineConnection::stop() {
    stopping = true;
    while (!finished) {
        {
            std::lock_guard guard(pipeMutex);
            if (stream != INVALID_HANDLE_VALUE)
                CancelIoEx(stream, nullptr);
        }
        Sleep(20);
    }
    if (worker.joinable())
        worker.join();
}
void EngineConnection::start(std::filesystem::path data, bool simulate, HWND window, UINT message) {
    finished = false;
    worker = std::thread([this, data, simulate, window, message] {
        bool attempted = false;
        ULONGLONG began = GetTickCount64();
        std::string launchFailure;
        while (!stopping) {
            try {
                if (startRequested.exchange(false)) {
                    attempted = false;
                    began = GetTickCount64();
                    launchFailure.clear();
                }
                Handle pipe(connectPipe(data, 200));
                StreamReset reset{pipeMutex, stream};
                if (pipe.value == INVALID_HANDLE_VALUE) {
                    if (!attempted) {
                        std::wstring error;
                        attempted = true;
                        if (!launchEngine(data, simulate, error))
                            launchFailure = utf8(error);
                    }
                    if (!launchFailure.empty())
                        throw std::runtime_error(launchFailure);
                    throw std::runtime_error(
                        GetTickCount64() - began < 10000
                            ? "正在连接后台…"
                            : "后台未就绪，请重试并检查任务计划程序、数据权限或旧版进程");
                }
                Json request{{"protocol", ProtocolVersion},
                             {"release", ReleaseVersion},
                             {"op", "snapshot"},
                             {"subscribe", true},
                             {"requestId", newId()}};
                {
                    std::lock_guard guard(pipeMutex);
                    stream = pipe.value;
                }
                if (!writeFrame(pipe.value, request))
                    throw std::runtime_error("连接中断");
                while (!stopping) {
                    auto packet = readFrame(pipe.value, 6000);
                    bool ok = packet.value("ok", false);
                    std::string failure = packet.value("error", "后台协议不兼容");
                    if (!ok)
                        writeFrame(pipe.value, {{"ack", true}});
                    auto *copy = new Json(std::move(packet));
                    if (!PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(copy)))
                        delete copy;
                    if (!ok)
                        throw std::runtime_error(failure);
                }
                {
                    std::lock_guard guard(pipeMutex);
                    stream = INVALID_HANDLE_VALUE;
                }
            } catch (const std::exception &error) {
                {
                    std::lock_guard guard(pipeMutex);
                    stream = INVALID_HANDLE_VALUE;
                }
                if (!stopping) {
                    auto *copy = new Json{{"ok", false}, {"offline", true}, {"error", error.what()}};
                    if (!PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(copy)))
                        delete copy;
                }
                for (int i = 0; i < 10 && !stopping; ++i)
                    Sleep(100);
            }
        }
        finished = true;
    });
}
} // namespace evening
