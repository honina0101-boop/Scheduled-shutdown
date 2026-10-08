"""Run isolated, simulated release validation. Never calls real shutdown."""
import argparse,ctypes,datetime,hashlib,json,os,platform,re,subprocess,time,winreg
from pathlib import Path
from ctypes import wintypes
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('--build-dir',default='build');parser.add_argument('--exe',default='dist/Evenfall.exe')
args=parser.parse_args();build=(ROOT/args.build_dir).resolve();exe=(ROOT/args.exe).resolve()
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
ui_result=out/'ui/smoke-results.json'
if ui_result.exists():
    assert ui_result.resolve().is_relative_to(ROOT),'test report path escaped workspace'
    ui_result.unlink()
run([exe,'--smoke-test',out/'ui'])
ui=json.loads((out/'ui/smoke-results.json').read_text(encoding='utf-8'))
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
    handle=None
    try:
        time.sleep(3)
        assert process.poll() is None,'benchmark process stopped'
        handle=kernel.OpenProcess(0x410,False,process.pid)
        if not handle:raise ctypes.WinError(ctypes.get_last_error())
        began=time.perf_counter();cpu=cpu_seconds(handle);time.sleep(8)
        duration=time.perf_counter()-began;used=cpu_seconds(handle)-cpu
        result=read_memory(handle);result.update({'observed_seconds':round(duration,2),'cpu_seconds':round(used,4),'cpu_percent_one_core':round(used/duration*100,3)})
        return result
    finally:
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
 'ui':ui,'cold_tray':cold,'after_window_hidden':warm,'dll_imports':dlls,'autostart_unchanged':True,
 'unverified':['real shutdown on a dedicated test machine','Windows 11 hardware','multiple physical monitors','actual logon startup','real suspend/resume']}
(out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
total=report['scheduler']['assertions']+report['persistence_platform']['assertions']+ui['checks']
text=f"""# 本地验证报告

测试时间：{report['tested_at']}（北京时间）
系统：{report['os']}
发布 EXE：{size:,} 字节（{size/1_000_000:.2f} MB），单文件，无需额外运行库。
SHA-256：{report['binary_sha256']}

## 结果

- 调度：{report['scheduler']['assertions']} 项断言通过。
- Windows 时区及存储：{report['persistence_platform']['assertions']} 项断言通过。
- 界面模拟自检：{ui['checks']} 项通过。
- 合计：{total} 项。
- 模拟测试没有调用真实关机，没有修改真实自启动项。
- 导出并查看浅／深色、任务编辑、设置、记录和提醒；另导出 150%／200%。

## 托盘资源样本

| 模式 | 工作集 MiB | 私有内存 MiB | CPU（单核百分比） |
|---|---:|---:|---:|
| 冷启动直接到托盘 | {cold['working_set_mib']} | {cold['private_mib']} | {cold['cpu_percent_one_core']} |
| 显示窗口后隐藏 | {warm['working_set_mib']} | {warm['private_mib']} | {warm['cpu_percent_one_core']} |

每种模式观察约 8 秒。峰值及完整输出见 build/qa/report.json。当前机器样本不代表所有驱动与系统版本。

## 实测边界

已在当前 Windows 10 环境验证。Windows 11 真机、多块物理显示器、真实登录启动、真实睡眠恢复以及实际关机，尚未在专用环境执行；验证步骤见 TESTING.md。所有自动关机测试都使用模拟执行器。
"""
(ROOT/'docs/QA-REPORT.md').write_text(text,encoding='utf-8')
print(json.dumps(report,ensure_ascii=False,indent=2))
