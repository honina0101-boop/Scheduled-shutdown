"""Package the executable and source without user data or development tools."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,zipfile
ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--build-dir',default='build')
parser.add_argument('--msvc',action='store_true')
parser.add_argument('--reuse-exe',action='store_true',help='Package the already reviewed EXE without modifying its bytes')
args=parser.parse_args()
build=(ROOT/args.build_dir).resolve()
dist=ROOT/'dist';dist.mkdir(exist_ok=True)
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
usage="""晚安 · 定时关机 1.0.0
直接双击 Evenfall.exe，免安装。

新建任务：执行一次、按星期重复，或多少分钟后关机。
右下角提前一分钟提醒，可取消本次，或推迟 10 / 30 / 60 分钟。
取消只影响本次，重复计划保留，重启软件也不会恢复本次任务。
关闭窗口进入托盘，托盘右键“退出”停止软件。
睡眠、软件停止或暂停错过时间时，跳过，不主动唤醒电脑。
锁屏时任务继续；普通关机可能被未保存文档阻止。
设置中可启用登录启动、提示音、主题、强制关机或预览提醒。
强制关机可能丢失未保存内容，建议保持默认普通关机。
开启登录启动前请把 EXE 放在固定目录，移动后重新关闭并开启。
默认不开机自启动，没有预置任务。
数据：%LOCALAPPDATA%\\Evenfall（仅当前用户）
保存异常会暂停自动任务；重试保存后，检查并恢复任务。

软件离线运行，不上传数据。本地发布未做代码签名。
许可见 THIRD-PARTY-NOTICES.txt 和 licenses 文件夹。
"""
(dist/'使用说明.txt').write_text(usage,encoding='utf-8-sig')
shutil.copy2(ROOT/'THIRD-PARTY-NOTICES.txt',dist/'THIRD-PARTY-NOTICES.txt')
portable=dist/'晚安-定时关机-1.0.0.zip'
licenses=['JSON-LICENSE.txt','GCC-RUNTIME-EXCEPTION.txt','GPL-3.0.txt','MINGW-LICENSE.txt']
project_license=ROOT/'LICENSE'
with zipfile.ZipFile(portable,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
    for name in ['Evenfall.exe','使用说明.txt','THIRD-PARTY-NOTICES.txt']:archive.write(dist/name,name)
    if project_license.exists():archive.write(project_license,'LICENSE')
    for name in licenses:
        if (ROOT/'third_party'/name).exists():archive.write(ROOT/'third_party'/name,'licenses/'+name)
source_archive=dist/'Evenfall-1.0.0-source.zip'
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
