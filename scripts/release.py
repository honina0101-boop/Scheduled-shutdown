"""Package the executable and source without user data or development tools."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,zipfile
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--build-dir',default='build')
parser.add_argument('--dist-dir',default='dist',help='Output directory; use a fresh directory while an older portable EXE is running')
parser.add_argument('--msvc',action='store_true')
parser.add_argument('--reuse-exe',action='store_true',help='Package the already reviewed EXE without modifying its bytes')
args=parser.parse_args()
build=(ROOT/args.build_dir).resolve()
dist=ROOT/args.dist_dir;dist.mkdir(parents=True,exist_ok=True)
source=build/'Evenfall.exe';target=dist/'Evenfall.exe'
if args.reuse_exe:
    assert target.exists(),'Reviewed EXE is missing'
else:
    shutil.copy2(source,target)
    if not args.msvc:
        tools=ROOT/'.tools/w64devkit/bin'
        subprocess.run([str(tools/'objcopy.exe'),'--only-keep-debug',str(source),str(dist/'Evenfall.debug')],check=True)
        subprocess.run([str(tools/'strip.exe'),'--strip-all',str(target)],check=True)
    elif (build/'Evenfall.pdb').exists():
        shutil.copy2(build/'Evenfall.pdb',dist/'Evenfall.pdb')
assert 0<target.stat().st_size<=5_000_000,'EXE exceeds 5 MB budget'
usage="""晚安 · 定时关机 1.1.0
解压后双击 Evenfall.exe，放在固定、本地、未加密目录。

设置 → 未登录也执行任务：创建当前账户的免保存密码 S4U 后台任务，可能需要管理员授权。
设置 → 登录后启动托盘界面：登录后显示托盘，双击打开主窗口并连接后台。
两个开关独立，新用户默认关闭。手动运行仍可使用。
未登录／锁屏时不能操作桌面提醒，但后台仍会按计划关闭整台电脑。

新建一次、星期重复或倒计时任务。提前一分钟提醒，取消本次或推迟 10／30／60 分钟。
关闭任务右侧开关会持续停用，必须手动开启；取消本次只跳过该次计划日期。
关闭主窗口进入托盘。“退出界面，后台继续”不停止任务；“停止后台并退出”停止当次运行。
关闭后台开机启动不结束已在运行的后台。暂停全部会持久化，须手动恢复。
登录／重新打开界面不会重置倒计时。睡眠或后台停止错过的任务跳过，不主动唤醒电脑。
普通关机可能被未保存文档阻止；强制关机可能丢失未保存内容。

后台独占数据，界面通过本机通信操作。未连接时可重试，不会启动第二个调度器。
S4U 不保存密码，不能读取网络或加密文件，受 Windows 账户及系统策略限制。
升级前退出旧程序及后台，备份整个 %LOCALAPPDATA%\\Evenfall 和旧 EXE 再替换。
数据格式为 v3；回退旧版须先停止并停用新版后台，再恢复升级前备份。
程序离线运行，未做代码签名。使用说明、验证边界及交接记录见项目 README 和 docs。
许可见 LICENSE、THIRD-PARTY-NOTICES.txt 和 licenses 文件夹。
"""
(dist/'使用说明.txt').write_text(usage,encoding='utf-8-sig')
shutil.copy2(ROOT/'THIRD-PARTY-NOTICES.txt',dist/'THIRD-PARTY-NOTICES.txt')
portable=dist/'晚安-定时关机-1.1.0.zip'
licenses=['JSON-LICENSE.txt','GCC-RUNTIME-EXCEPTION.txt','GPL-3.0.txt','MINGW-LICENSE.txt']
project_license=ROOT/'LICENSE'
with zipfile.ZipFile(portable,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
    for name in ['Evenfall.exe','使用说明.txt','THIRD-PARTY-NOTICES.txt']:archive.write(dist/name,name)
    if project_license.exists():archive.write(project_license,'LICENSE')
    for name in licenses:
        if (ROOT/'third_party'/name).exists():archive.write(ROOT/'third_party'/name,'licenses/'+name)
source_archive=dist/'Evenfall-1.1.0-source.zip'
with zipfile.ZipFile(source_archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
    for directory in ['src','tests','resources','scripts','third_party','docs']:
        for file in sorted((ROOT/directory).rglob('*')):
            if file.is_file() and '__pycache__' not in file.parts and file.name!='fix_build.py':archive.write(file,str(file.relative_to(ROOT)))
    for name in ['CMakeLists.txt','README.md','.gitignore','.clang-format','THIRD-PARTY-NOTICES.txt']:archive.write(ROOT/name,name)
    if project_license.exists():archive.write(project_license,'LICENSE')
checksums={}
for file in [target,portable,source_archive]:checksums[file.name]={'bytes':file.stat().st_size,'sha256':hashlib.sha256(file.read_bytes()).hexdigest()}
(dist/'CHECKSUMS.json').write_text(json.dumps(checksums,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(checksums,ensure_ascii=False,indent=2))
