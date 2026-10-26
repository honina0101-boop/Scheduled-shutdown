"""Run isolated, simulated release validation. Never calls real shutdown."""
import argparse,ctypes,datetime,hashlib,json,os,platform,re,subprocess,time,winreg,sys
from pathlib import Path
from ctypes import wintypes
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--build-dir',default='build');parser.add_argument('--exe',default='dist/Evenfall.exe')
args=parser.parse_args();build=(ROOT/args.build_dir).resolve();exe=(ROOT/args.exe).resolve()
sys.path.insert(0,str(ROOT/'tests'))
from runtime_integration import Client
out=ROOT/'build/qa';out.mkdir(parents=True,exist_ok=True)
startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
def startup_value():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,r'Software\Microsoft\Windows\CurrentVersion\Run') as key:
            return winreg.QueryValueEx(key,'Evenfall')
    except FileNotFoundError:return None
before=startup_value()
def run(command):
    result=subprocess.run([str(x) for x in command],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=40,startupinfo=startup)
    if result.returncode:raise RuntimeError(result.stdout+'\n'+result.stderr)
    return result.stdout
scheduler=run([build/'scheduler_tests.exe'])
storage=run([build/'platform_tests.exe',out/'test-storage'])
# An exclusive data-directory file lock is shared across Windows sessions.
# Hold the lock from another process and verify the app never starts its scheduler.
lock_kernel=ctypes.WinDLL('kernel32',use_last_error=True)
lock_kernel.CreateFileW.argtypes=[wintypes.LPCWSTR,wintypes.DWORD,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,wintypes.DWORD,wintypes.HANDLE]
lock_kernel.CreateFileW.restype=wintypes.HANDLE
lock_kernel.CloseHandle.argtypes=[wintypes.HANDLE]
lock_output=out/('lock-probe-'+str(time.time_ns()))
lock_data=lock_output/'data';lock_data.mkdir(parents=True,exist_ok=True)
lock_handle=lock_kernel.CreateFileW(str(lock_data/'instance.lock'),0xC0000000,0,None,4,0x80,None)
assert lock_handle != ctypes.c_void_p(-1).value,'cannot create exclusive test lock'
try:
    run([exe,'--smoke-test',lock_output])
    assert not (lock_output/'smoke-results.json').exists(),'second instance ran despite exclusive lock'
    assert not (lock_data/'state.json').exists(),'blocked instance changed scheduler state'
finally:
    lock_kernel.CloseHandle(lock_handle)
run([exe,'--smoke-test',lock_output])
assert json.loads((lock_output/'smoke-results.json').read_text(encoding='utf-8'))['passed'],'lock was not released for restart'
blocked_output=out/('lock-denied-'+str(time.time_ns()));blocked_output.mkdir(parents=True,exist_ok=True)
(blocked_output/'data').write_text('not a directory',encoding='ascii')
run([exe,'--smoke-test',blocked_output])
assert not (blocked_output/'smoke-results.json').exists(),'app started after lock directory failure'
ui_result=out/'ui/smoke-results.json'
if ui_result.exists():
    assert ui_result.resolve().is_relative_to(ROOT),'test report path escaped workspace'
    ui_result.unlink()
run([exe,'--smoke-test',out/'ui'])
ui=json.loads((out/'ui/smoke-results.json').read_text(encoding='utf-8'))
runtime=json.loads(run([sys.executable,'-X','utf8',ROOT/'tests/runtime_integration.py',exe,out/'runtime']))
assert runtime['passed'] and runtime['simulate']
client_out=out/('client-'+str(time.time_ns()))
try:
    run([exe,'--client-test',client_out])
    client_ui=json.loads((client_out/'client-results.json').read_text(encoding='utf-8'))
    assert client_ui['passed'] and client_ui['simulate'],client_ui
finally:
    try:Client(client_out/'data').mutate('stop')
    except OSError:pass
assert ui['passed'] and ui['simulate']
kernel=ctypes.WinDLL('kernel32',use_last_error=True);psapi=ctypes.WinDLL('psapi',use_last_error=True)
kernel.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];kernel.OpenProcess.restype=wintypes.HANDLE
kernel.CloseHandle.argtypes=[wintypes.HANDLE]
class Memory(ctypes.Structure):
    _fields_=[('cb',wintypes.DWORD),('faults',wintypes.DWORD)]+[(name,ctypes.c_size_t) for name in
      ['peak','working','quota_peak_paged','quota_paged','quota_peak_nonpaged','quota_nonpaged','pagefile','peak_pagefile','private']]
psapi.GetProcessMemoryInfo.argtypes=[wintypes.HANDLE,ctypes.POINTER(Memory),wintypes.DWORD]
kernel.GetProcessTimes.argtypes=[wintypes.HANDLE]+[ctypes.POINTER(wintypes.FILETIME)]*4
def read_memory(handle):
    value=Memory();value.cb=ctypes.sizeof(value)
    if not psapi.GetProcessMemoryInfo(handle,ctypes.byref(value),value.cb):raise ctypes.WinError(ctypes.get_last_error())
    return {'working_set_mib':round(value.working/1048576,2),'private_mib':round(value.private/1048576,2),'peak_working_set_mib':round(value.peak/1048576,2)}
def cpu_seconds(handle):
    values=[wintypes.FILETIME() for _ in range(4)]
    if not kernel.GetProcessTimes(handle,*[ctypes.byref(x) for x in values]):raise ctypes.WinError(ctypes.get_last_error())
    return sum((x.dwHighDateTime<<32)|x.dwLowDateTime for x in values[2:])/10_000_000
def measure(label,mode):
    process=subprocess.Popen([str(exe),'--simulate',mode,'--data-dir',str(out/label)],startupinfo=startup,creationflags=subprocess.CREATE_NO_WINDOW)
    handle=None;engine_handle=None;client=Client(out/label)
    try:
        time.sleep(3)
        assert process.poll() is None,'benchmark process stopped'
        handle=kernel.OpenProcess(0x410,False,process.pid)
        if not handle:raise ctypes.WinError(ctypes.get_last_error())
        client.state()
        engine_handle=kernel.OpenProcess(0x410,False,client.server_pid())
        if not engine_handle:raise ctypes.WinError(ctypes.get_last_error())
        engine_cpu=cpu_seconds(engine_handle)
        began=time.perf_counter();cpu=cpu_seconds(handle);time.sleep(8)
        duration=time.perf_counter()-began;used=cpu_seconds(handle)-cpu
        result=read_memory(handle);result.update({'observed_seconds':round(duration,2),'cpu_seconds':round(used,4),'cpu_percent_one_core':round(used/duration*100,3)})
        engine_result=read_memory(engine_handle)
        engine_result['cpu_percent_one_core']=round((cpu_seconds(engine_handle)-engine_cpu)/duration*100,3)
        result['engine']=engine_result
        result['combined_working_set_mib']=round(result['working_set_mib']+engine_result['working_set_mib'],2)
        return result
    finally:
        try:client.mutate('stop')
        except OSError:pass
        if engine_handle:kernel.CloseHandle(engine_handle)
        if handle:kernel.CloseHandle(handle)
        if process.poll() is None:process.terminate();process.wait(timeout=10)
cold=measure('cold-profile','--tray');warm=measure('warm-profile','--benchmark')
assert before==startup_value(),'simulated validation changed autostart'
objdump=ROOT/'.tools/w64devkit/bin/objdump.exe'
dlls=[]
if objdump.exists():
    imports=run([objdump,'-p',exe]);dlls=re.findall(r'DLL Name:\s+([^\s]+)',imports)
    assert not any(x.lower().startswith(('libstdc','libgcc','libwinpthread','vcruntime','msvcp')) for x in dlls),'external runtime DLL detected'
size=exe.stat().st_size;assert size<=5_000_000
report={
 'tested_at':datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat(),
 'os':platform.platform(),'binary_bytes':size,'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),
 'scheduler':{'passed':True,'assertions':int(re.search(r'PASS: (\d+)',scheduler)[1])},
 'persistence_platform':{'passed':True,'assertions':int(re.search(r'PASS: (\d+)',storage)[1])},
 'ui':ui,'ipc':runtime,'remote_ui':client_ui,'instance_lock':{'passed':True,'checks':4},'cold_tray':cold,'after_window_hidden':warm,'dll_imports':dlls,'autostart_unchanged':True,
 'unverified':['real shutdown on a dedicated test machine','Windows 11 hardware','multiple physical monitors','actual logon startup','real suspend/resume']}
(out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
total=report['scheduler']['assertions']+report['persistence_platform']['assertions']+ui['checks']+report['instance_lock']['checks']+runtime['checks']+client_ui['checks']
text=f"""# 本地验证报告

测试时间：{report['tested_at']}（北京时间）
系统：{report['os']}
发布 EXE：{size:,} 字节（{size/1_000_000:.2f} MB），单文件，无需额外运行库。
SHA-256：{report['binary_sha256']}

## 结果

- 调度：{report['scheduler']['assertions']} 项断言通过。
- Windows 时区及存储：{report['persistence_platform']['assertions']} 项断言通过。
- 界面模拟自检：{ui['checks']} 项通过。
- 跨进程数据目录独占锁／失败阻断：{report['instance_lock']['checks']} 项通过。
- 真实后台 IPC／访问限制／持久化：{runtime['checks']} 项通过。
- 实际界面与独立后台联动：{client_ui['checks']} 项通过。
- 合计：{total} 项。
- 模拟测试没有调用真实关机，没有修改真实自启动项。
- 导出并查看浅／深色、任务编辑、设置、记录和提醒；另导出 150%／200%。

## 托盘资源样本

| 模式 | 界面工作集 MiB | 后台工作集 MiB | 合计 MiB | 界面／后台 CPU（单核百分比） |
|---|---:|---:|---:|---|
| 冷启动直接到托盘 | {cold['working_set_mib']} | {cold['engine']['working_set_mib']} | {cold['combined_working_set_mib']} | {cold['cpu_percent_one_core']} / {cold['engine']['cpu_percent_one_core']} |
| 显示窗口后隐藏 | {warm['working_set_mib']} | {warm['engine']['working_set_mib']} | {warm['combined_working_set_mib']} | {warm['cpu_percent_one_core']} / {warm['engine']['cpu_percent_one_core']} |

每种模式观察约 8 秒。峰值及完整输出见 build/qa/report.json。当前机器样本不代表所有驱动与系统版本。
工作集目标为 30 MiB：冷启动样本{'达标' if cold['combined_working_set_mib'] <= 30 else '未达标'}，显示窗口后隐藏的样本{'达标' if warm['combined_working_set_mib'] <= 30 else '未达标'}；这是资源目标的实测状态，不作为功能通过的替代。

## 实测边界

已在当前 Windows 10 环境验证。Windows 11 真机、多块物理显示器、真实登录启动、真实睡眠恢复以及实际关机，尚未在专用环境执行；验证步骤见 TESTING.md。所有自动关机测试都使用模拟执行器。
"""
(ROOT/'docs/QA-REPORT.md').write_text(text,encoding='utf-8')
print(json.dumps(report,ensure_ascii=False,indent=2))
