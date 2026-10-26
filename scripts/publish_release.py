"""Publish already verified portable artifacts to GitHub Releases."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "honina0101-boop/Scheduled-shutdown"
VERSION = "1.1.0"
TAG = "v" + VERSION
API = "https://api.github.com/repos/" + REPOSITORY


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(directory="dist", candidate=False):
    dist = ROOT / directory
    checks = json.loads((dist / "CHECKSUMS.json").read_text(encoding="utf-8"))
    for name, item in checks.items():
        file = dist / name
        if file.stat().st_size != item["bytes"] or digest(file) != item["sha256"]:
            raise RuntimeError("Artifact checksum mismatch: " + name)
    executable = dist / "Evenfall.exe"
    if executable.stat().st_size > 5_000_000:
        raise RuntimeError("EXE exceeds the 5 MB budget")
    report = ROOT / "build/qa/report.json"
    if report.exists() and not candidate:
        verified = json.loads(report.read_text(encoding="utf-8"))
        if verified["binary_sha256"] != digest(executable):
            raise RuntimeError("EXE differs from the verified binary")
    portable = dist / ("Evenfall-" + VERSION + "-win-x64.zip")
    shutil.copy2(dist / ("晚安-定时关机-" + VERSION + ".zip"), portable)
    source = dist / ("Evenfall-" + VERSION + "-source.zip")
    for file in [portable, source]:
        with zipfile.ZipFile(file) as archive:
            if archive.testzip() is not None:
                raise RuntimeError("Invalid ZIP: " + file.name)
    with zipfile.ZipFile(portable) as archive:
        if hashlib.sha256(archive.read("Evenfall.exe")).hexdigest() != digest(executable):
            raise RuntimeError("Portable EXE differs from the verified binary")
    assets = [portable, executable, source]
    sums = dist / "SHA256SUMS.txt"
    sums.write_text("".join(digest(file) + "  " + file.name + "\n" for file in assets),
                    encoding="ascii")
    return assets + [sums]


def credential():
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        return token
    env = os.environ.copy()
    env.update(GIT_TERMINAL_PROMPT="0", GCM_INTERACTIVE="Never")
    result = subprocess.run(
        ["git", "credential", "fill"],
        input="protocol=https\nhost=github.com\npath=" + REPOSITORY + ".git\n\n",
        text=True, capture_output=True, env=env, timeout=20, cwd=ROOT)
    fields = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    token = fields.get("password") if result.returncode == 0 else None
    if not token:
        raise RuntimeError("GitHub credentials unavailable; sign in with Git Credential Manager")
    return token


def request(token, method, url, value=None, raw=None, content_type=None):
    data = raw if raw is not None else (
        json.dumps(value, ensure_ascii=False).encode("utf-8") if value is not None else None)
    headers = {"Authorization": "Bearer " + token,
               "User-Agent": "Evenfall-Release",
               "Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28"}
    if data is not None:
        headers["Content-Type"] = content_type or "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=45) as response:
        return json.load(response)


def publish(assets, target, candidate=False):
    token = credential()
    if candidate:
        validation = "按用户要求停止最终功能验证，直接提供自测包。此版本标记为预发布，待用户验证；此前构建的测试结果不作为本包验收结果。"
    else:
        report = json.loads((ROOT / "build/qa/report.json").read_text(encoding="utf-8"))
        total = report["scheduler"]["assertions"] + report["persistence_platform"]["assertions"] + report["ui"]["checks"] + report["instance_lock"]["checks"] + report["ipc"]["checks"] + report["remote_ui"]["checks"]
        validation = str(total) + " 项隔离模拟检查通过；真实未登录关机仍须专用环境验收。"
    body = r"""## 下载与使用

下载 **Evenfall-1.1.0-win-x64.zip**，解压后双击 **Evenfall.exe**。单 EXE 小于 5 MB，Windows 10/11 64 位，无需另装运行库。

## 1.1.0：后台调度与界面分离

- 设置“未登录也执行任务”后，使用当前账户的免保存密码 S4U 计划任务运行唯一后台；登录后托盘界面连接它。
- 两个独立启动开关；关闭界面不停止调度。托盘可选择“退出界面，后台继续”或“停止后台并退出”。
- 登录、解锁、重新打开界面不会重新安排倒计时。未登录时无法显示可操作桌面提醒，但后台仍按计划执行。
- 本地命名管道限制账户身份并拒绝远程访问，后台独占数据、保存成功再确认；过期命令和过期编辑草稿被拒绝。
- 保留午夜双时钟保护、持续停用、取消本次、卡片内推迟选项和执行记录。

## 设置与升级

EXE 放在固定、本地、未加密目录。开启“未登录也执行任务”可能需要管理员授权；软件不获取或保存 Windows 登录密码。S4U 受账户批处理登录、关机权限及系统策略限制，不能访问网络或加密文件。设置失败明确报错，不改用 SYSTEM。

升级前退出所有旧副本并备份配置及旧 EXE。新版本读取旧数据、保存为 schemaVersion 3；回退旧版须先停止并停用新版后台，再恢复升级前备份。不要再另设运行普通界面的非交互式计划任务，使用程序管理的后台入口。

## 验证边界

{validation}

未登录冷启动、真实关机、Windows 11、多物理显示器、真实注销／睡眠恢复等仍需专用环境验收；不会把模拟结果称为真实未登录关机通过。本版本未签名。

使用说明及许可证包含在便携包中；校验值见 SHA256SUMS.txt，开发记录及验证详情见源码 docs。
""".replace("{validation}", validation)
    try:
        release = request(token, "GET", API + "/releases/tags/" + TAG)
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
        release = request(token, "POST", API + "/releases",
                          {"tag_name": TAG, "target_commitish": target,
                           "name": "晚安 · 定时关机 " + VERSION,
                           "body": body, "draft": True, "prerelease": candidate})
    if not release["draft"] and release["prerelease"] != candidate:
        raise RuntimeError("Existing release verification status differs; refusing to change it")
    upload = release["upload_url"].split("{", 1)[0]
    existing = {asset["name"]: asset for asset in release["assets"]}
    for file in assets:
        previous = existing.get(file.name)
        expected = "sha256:" + digest(file)
        if previous:
            if previous.get("digest") != expected:
                raise RuntimeError("Existing release asset differs: " + file.name)
            print("Verified existing asset: " + file.name, flush=True)
            continue
        media = ("application/zip" if file.suffix == ".zip" else
                 "text/plain" if file.suffix == ".txt" else "application/octet-stream")
        uploaded = request(token, "POST",
                           upload + "?name=" + urllib.parse.quote(file.name),
                           raw=file.read_bytes(), content_type=media)
        if uploaded["size"] != file.stat().st_size:
            raise RuntimeError("Uploaded asset size mismatch: " + file.name)
        if uploaded.get("digest") and uploaded["digest"] != expected:
            raise RuntimeError("Uploaded asset digest mismatch: " + file.name)
        print("Uploaded: " + file.name, flush=True)
    release = request(token, "PATCH", API + "/releases/" + str(release["id"]),
                      {"draft": False, "body": body, "prerelease": candidate, "make_latest": "false" if candidate else "true"})
    print("Published: " + release["html_url"], flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--candidate", action="store_true", help="Explicitly publish an unverified prerelease for user acceptance")
    parser.add_argument("--dist-dir", default="dist")
    parser.add_argument("--target-sha", help="Full verified commit SHA for the release tag")
    args = parser.parse_args()
    try:
        assets = prepare(args.dist_dir, args.candidate)
        if args.prepare_only:
            print("Verified release assets: " + ", ".join(file.name for file in assets))
        else:
            if not args.target_sha or not re.fullmatch(r"[0-9a-f]{40}", args.target_sha):
                raise RuntimeError("--target-sha must be a full verified commit SHA")
            publish(assets, args.target_sha, args.candidate)
    except (RuntimeError, urllib.error.URLError, subprocess.TimeoutExpired) as error:
        raise SystemExit("Release failed: " + str(error))
