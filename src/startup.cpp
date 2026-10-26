#include "runtime.hpp"
#include <fstream>
#include <oleauto.h>
#include <sddl.h>
#include <stdexcept>
#include <taskschd.h>

namespace evening {
namespace {
template <class T> struct Com {
    T *p = nullptr;
    ~Com() {
        if (p)
            p->Release();
    }
    T **out() { return &p; }
    T *operator->() const { return p; }
};
struct Bstr {
    BSTR p;
    explicit Bstr(const std::wstring &s) : p(SysAllocString(s.c_str())) {}
    ~Bstr() { SysFreeString(p); }
    operator BSTR() const { return p; }
};
struct Apartment {
    HRESULT code = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~Apartment() {
        if (SUCCEEDED(code))
            CoUninitialize();
    }
};
void require(HRESULT code, const char *action) {
    if (FAILED(code))
        throw std::runtime_error(std::string(action) + "（系统代码 " +
                                 std::to_string(static_cast<unsigned long>(code)) + "）");
}
struct Scheduler {
    Apartment apartment;
    Com<ITaskService> service;
    Com<ITaskFolder> folder;
    Scheduler() {
        require(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                                 reinterpret_cast<void **>(service.out())),
                "无法连接任务计划程序");
        VARIANT empty;
        VariantInit(&empty);
        require(service->Connect(empty, empty, empty, empty), "无法连接任务计划程序");
        require(service->GetFolder(Bstr(L"\\"), folder.out()), "无法访问计划任务目录");
    }
};
std::wstring taskName(const std::filesystem::path &data) {
    return L"Evenfall.Engine." + runtimeIdentity(data);
}
std::wstring xml(const std::wstring &value) {
    std::wstring s;
    for (auto c : value) {
        if (c == L'&')
            s += L"&amp;";
        else if (c == L'<')
            s += L"&lt;";
        else if (c == L'>')
            s += L"&gt;";
        else if (c == L'\"')
            s += L"&quot;";
        else
            s += c;
    }
    return s;
}
std::wstring arguments(const std::filesystem::path &data, bool simulate) {
    return L"--engine --owner-sid " + quoteArgument(ownerSid()) + L" --data-dir " +
           quoteArgument(data.wstring()) + (simulate ? L" --simulate" : L"");
}
void validateTask(IRegisteredTask *task, const std::filesystem::path &data, bool legacy = false) {
    Com<ITaskDefinition> definition;
    Com<IPrincipal> principal;
    Com<IActionCollection> actions;
    Com<IAction> action;
    Com<IExecAction> exec;
    require(task->get_Definition(definition.out()), "读取计划任务失败");
    require(definition->get_Principal(principal.out()), "读取任务账户失败");
    TASK_LOGON_TYPE logon;
    require(principal->get_LogonType(&logon), "读取后台登录类型失败");
    if (!legacy && logon != TASK_LOGON_S4U)
        throw std::runtime_error("同名任务不是 S4U 后台任务，未修改");
    BSTR user = nullptr;
    require(principal->get_UserId(&user), "读取任务账户失败");
    // Task Scheduler may normalize the SID to an account name. Resolve both forms.
    std::wstring account(user ? user : L"");
    SysFreeString(user);
    PSID sid = nullptr;
    bool same = false;
    if (ConvertStringSidToSidW(account.c_str(), &sid)) {
        LPWSTR text = nullptr;
        ConvertSidToStringSidW(sid, &text);
        same = text && ownerSid() == text;
        if (text)
            LocalFree(text);
        LocalFree(sid);
    } else {
        DWORD bytes = 0, length = 0;
        SID_NAME_USE use{};
        LookupAccountNameW(nullptr, account.c_str(), nullptr, &bytes, nullptr, &length, &use);
        std::vector<BYTE> raw(bytes);
        std::wstring domain(length, L'\0');
        if (bytes &&
            LookupAccountNameW(nullptr, account.c_str(), raw.data(), &bytes, domain.data(), &length, &use)) {
            LPWSTR text = nullptr;
            ConvertSidToStringSidW(raw.data(), &text);
            same = text && ownerSid() == text;
            if (text)
                LocalFree(text);
        }
    }
    if (!same)
        throw std::runtime_error("同名计划任务属于其他账户，未修改");
    require(definition->get_Actions(actions.out()), "读取任务动作失败");
    LONG count = 0;
    actions->get_Count(&count);
    if (count != 1)
        throw std::runtime_error("同名计划任务不是本程序的单一后台动作，未修改");
    require(actions->get_Item(1, action.out()), "读取后台动作失败");
    require(action->QueryInterface(IID_IExecAction, reinterpret_cast<void **>(exec.out())),
            "任务动作类型错误");
    BSTR path = nullptr, args = nullptr;
    exec->get_Path(&path);
    exec->get_Arguments(&args);
    std::wstring executable(path ? path : L""), command(args ? args : L"");
    if (path)
        SysFreeString(path);
    if (args)
        SysFreeString(args);
    bool validArguments = legacy ? (command.empty() || command == L"--tray" || command == L"--ui --tray")
                                 : (command == arguments(data, false) || command == arguments(data, true));
    if (_wcsicmp(executable.c_str(), executablePath().c_str()) || !validArguments)
        throw std::runtime_error("计划任务路径与本次程序不一致，请先迁移或停用旧任务");
}
Json query(const std::filesystem::path &data) {
    return engineRequest(data, {{"op", "snapshot"}}, 1000);
}
Json change(const std::filesystem::path &data, const std::string &op, bool enabled = false) {
    auto state = query(data);
    if (!state.value("ok", false))
        throw std::runtime_error(state.value("error", "后台协议不兼容"));
    auto reply = engineRequest(
        data, {{"op", op}, {"enabled", enabled}, {"version", state["snapshot"]["state"]["version"]}});
    if (!reply.value("ok", false))
        throw std::runtime_error(reply.value("error", "后台操作失败"));
    return reply;
}
} // namespace
bool backgroundTaskExists(const std::filesystem::path &data) {
    Scheduler scheduler;
    Com<IRegisteredTask> task;
    auto code = scheduler.folder->GetTask(Bstr(taskName(data)), task.out());
    if (code == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || code == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        return false;
    require(code, "无法查询后台计划任务，未改用其他启动身份");
    VARIANT_BOOL enabled = VARIANT_FALSE;
    task->get_Enabled(&enabled);
    if (enabled == VARIANT_TRUE)
        validateTask(task.p, data);
    return enabled == VARIANT_TRUE;
}
bool runBackgroundTask(const std::filesystem::path &data, std::wstring &error) {
    try {
        Scheduler scheduler;
        Com<IRegisteredTask> task;
        Com<IRunningTask> running;
        require(scheduler.folder->GetTask(Bstr(taskName(data)), task.out()), "后台计划任务不存在");
        validateTask(task.p, data);
        VARIANT empty;
        VariantInit(&empty);
        require(task->Run(empty, running.out()), "无法启动后台计划任务");
        return true;
    } catch (const std::exception &e) {
        error = wide(e.what());
        return false;
    }
}
Json configureBackground(const std::filesystem::path &data, bool enabled, bool simulate) {
    bool recorded = false, existed = false, previousEnabled = false, previousSetting = false;
    std::wstring previousXml;
    Com<IRegisteredTask> legacy;
    bool legacyEnabled = false, legacyChanged = false;
    try {
        if ((GetFileAttributesW(data.c_str()) & FILE_ATTRIBUTE_ENCRYPTED) ||
            (GetFileAttributesW(executablePath().c_str()) & FILE_ATTRIBUTE_ENCRYPTED))
            throw std::runtime_error("S4U 后台不能读取加密文件，请使用未加密的本地目录");
        Scheduler scheduler;
        if (enabled && !simulate && runtimeIdentity(data) == runtimeIdentity(defaultDataDirectory()) &&
            SUCCEEDED(scheduler.folder->GetTask(Bstr(L"Evenfall"), legacy.out()))) {
            bool relevant = true;
            try {
                validateTask(legacy.p, data, true);
            } catch (...) {
                relevant = false;
            }
            if (relevant) {
                VARIANT_BOOL value = VARIANT_FALSE;
                require(legacy->get_Enabled(&value), "读取旧任务状态失败");
                legacyEnabled = value == VARIANT_TRUE;
                BSTR definition = nullptr;
                require(legacy->get_Xml(&definition), "备份旧任务失败");
                std::wstring text = definition ? definition : L"";
                if (definition)
                    SysFreeString(definition);
                auto folder = data / L"startup-backups";
                std::filesystem::create_directories(folder);
                std::ofstream backup(folder / (L"legacy-" + wide(newId()) + L".xml"), std::ios::binary);
                const BYTE bom[] = {0xff, 0xfe};
                backup.write(reinterpret_cast<const char *>(bom), sizeof(bom));
                backup.write(reinterpret_cast<const char *>(text.data()), text.size() * sizeof(wchar_t));
                backup.flush();
                if (!backup)
                    throw std::runtime_error("旧任务备份无法保存，未迁移");
            } else {
                legacy.p->Release();
                legacy.p = nullptr;
            }
        }
        Com<IRegisteredTask> previous;
        existed = SUCCEEDED(scheduler.folder->GetTask(Bstr(taskName(data)), previous.out()));
        if (existed) {
            validateTask(previous.p, data);
            BSTR raw = nullptr;
            previous->get_Xml(&raw);
            previousXml = raw ? raw : L"";
            if (raw)
                SysFreeString(raw);
            VARIANT_BOOL value;
            previous->get_Enabled(&value);
            previousEnabled = value == VARIANT_TRUE;
        }
        auto old = query(data);
        if (!old.value("ok", false))
            throw std::runtime_error(old.value("error", "后台版本不兼容"));
        previousSetting = old.at("snapshot").at("state").at("settings").value("background", false);
        if (enabled) {
            auto sid = ownerSid();
            std::wstring definition =
                L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
                L"<Task version=\"1.4\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
                L"<RegistrationInfo><Description>Evenfall background "
                L"scheduler</Description></RegistrationInfo>"
                L"<Triggers><BootTrigger><Enabled>true</Enabled></BootTrigger></Triggers>"
                L"<Principals><Principal id=\"Owner\"><UserId>" +
                xml(sid) +
                L"</UserId><LogonType>S4U</LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></"
                L"Principals>"
                L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
                L"<DisallowStartIfOnBatteries>false</"
                L"DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
                L"<AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable>"
                L"<RunOnlyIfNetworkAvailable>false</"
                L"RunOnlyIfNetworkAvailable><IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd>"
                L"<RestartOnIdle>false</RestartOnIdle></IdleSettings><AllowStartOnDemand>true</"
                L"AllowStartOnDemand>"
                L"<Enabled>true</Enabled><Hidden>false</Hidden><RunOnlyIfIdle>false</RunOnlyIfIdle>"
                L"<WakeToRun>false</WakeToRun><ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
                L"<RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure></Settings>"
                L"<Actions Context=\"Owner\"><Exec><Command>" +
                xml(executablePath()) + L"</Command><Arguments>" + xml(arguments(data, simulate)) +
                L"</Arguments></Exec></Actions></Task>";
            VARIANT empty, user, security;
            VariantInit(&empty);
            VariantInit(&user);
            VariantInit(&security);
            user.vt = VT_BSTR;
            user.bstrVal = SysAllocString(sid.c_str());
            security.vt = VT_BSTR;
            security.bstrVal =
                SysAllocString((L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;" + sid + L")").c_str());
            Com<IRegisteredTask> task;
            HRESULT registered =
                scheduler.folder->RegisterTask(Bstr(taskName(data)), Bstr(definition), TASK_CREATE_OR_UPDATE,
                                               user, empty, TASK_LOGON_S4U, security, task.out());
            VariantClear(&user);
            VariantClear(&security);
            require(registered, "S4U 任务注册失败，请检查批处理登录策略与管理员权限");
            recorded = true;
        } else if (existed) {
            require(previous->put_Enabled(VARIANT_FALSE), "禁用后台启动失败");
            recorded = true;
        }
        change(data, "background-record", enabled);
        if (enabled) {
            change(data, "stop");
            bool gone = false;
            for (int i = 0; i < 100; ++i) {
                HANDLE h = CreateFileW((data / L"instance.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                    gone = true;
                    break;
                }
                Sleep(100);
            }
            if (!gone)
                throw std::runtime_error("原后台未退出，未启动第二个调度器");
            std::wstring error;
            if (!runBackgroundTask(data, error))
                throw std::runtime_error(utf8(error));
            bool connected = false;
            auto deadline = GetTickCount64() + 10000;
            while (GetTickCount64() < deadline) {
                Json reply;
                try {
                    reply = query(data);
                } catch (...) {
                    Sleep(100);
                    continue;
                }
                if (reply.value("ok", false) && reply["snapshot"]["session"] == 0 &&
                    reply["snapshot"]["simulate"] == simulate) {
                    if (!reply["snapshot"].value("shutdownPrivilege", false))
                        throw std::runtime_error("S4U 账户没有本机关机权限，请检查系统策略");
                    connected = true;
                    break;
                }
                Sleep(100);
            }
            if (!connected)
                throw std::runtime_error("S4U 后台未就绪，请检查任务运行结果及账户登录策略");
            if (legacy.p && legacyEnabled) {
                require(legacy->put_Enabled(VARIANT_FALSE), "新后台已启动，但旧任务无法停用");
                legacyChanged = true;
                auto code = legacy->Stop(0);
                if (code != SCHED_E_TASK_NOT_RUNNING)
                    require(code, "旧版任务停止失败");
            }
        }
        return {{"ok", true}, {"background", enabled}};
    } catch (const std::exception &e) {
        std::string rollback;
        if (legacyChanged && FAILED(legacy->put_Enabled(legacyEnabled ? VARIANT_TRUE : VARIANT_FALSE)))
            rollback = "；旧任务启用状态需人工恢复";
        if (recorded) {
            try {
                Scheduler scheduler;
                if (existed) {
                    VARIANT empty, user;
                    VariantInit(&empty);
                    VariantInit(&user);
                    user.vt = VT_BSTR;
                    user.bstrVal = SysAllocString(ownerSid().c_str());
                    Com<IRegisteredTask> restored;
                    auto hr = scheduler.folder->RegisterTask(Bstr(taskName(data)), Bstr(previousXml),
                                                             TASK_CREATE_OR_UPDATE, user, empty,
                                                             TASK_LOGON_S4U, empty, restored.out());
                    VariantClear(&user);
                    require(hr, "恢复原后台任务失败");
                    restored->put_Enabled(previousEnabled ? VARIANT_TRUE : VARIANT_FALSE);
                } else
                    require(scheduler.folder->DeleteTask(Bstr(taskName(data)), 0), "清理未完成后台任务失败");
                try {
                    change(data, "background-record", previousSetting);
                    auto state = query(data);
                    auto paused = engineRequest(data, {{"op", "pause"},
                                                       {"paused", true},
                                                       {"version", state["snapshot"]["state"]["version"]}});
                    if (!paused.value("ok", false))
                        throw std::runtime_error("无法暂停后台");
                } catch (...) {
                    HANDLE lock = INVALID_HANDLE_VALUE;
                    for (int i = 0; i < 50 && lock == INVALID_HANDLE_VALUE; ++i) {
                        lock = CreateFileW((data / L"instance.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (lock == INVALID_HANDLE_VALUE)
                            Sleep(100);
                    }
                    if (lock == INVALID_HANDLE_VALUE)
                        throw std::runtime_error("请手动暂停后台并检查启动设置");
                    Storage storage(data);
                    auto state = storage.load().state;
                    state.settings.background = previousSetting;
                    state.settings.paused = true;
                    ++state.version;
                    std::wstring error;
                    bool saved = storage.save(state, error);
                    CloseHandle(lock);
                    if (!saved)
                        throw std::runtime_error(utf8(error));
                }
            } catch (const std::exception &failure) {
                rollback = std::string("；回退需人工检查：") + failure.what();
            }
        }
        return {{"ok", false}, {"error", std::string(e.what()) + rollback}};
    }
}
} // namespace evening
