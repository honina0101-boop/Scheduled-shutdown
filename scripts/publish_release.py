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
VERSION = "1.0.1"
TAG = "v" + VERSION
API = "https://api.github.com/repos/" + REPOSITORY


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(directory="dist"):
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
    if report.exists():
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


def publish(assets, target):
    token = credential()
    report = json.loads((ROOT / "build/qa/report.json").read_text(encoding="utf-8"))
    total = report["scheduler"]["assertions"] + report["persistence_platform"]["assertions"] + report["ui"]["checks"] + report["instance_lock"]["checks"]
    body = r"""## 下载与使用

推荐下载 **Evenfall-1.0.1-win-x64.zip**，解压后双击 **Evenfall.exe**。免安装，无需另装运行库，适用于 Windows 10/11 64 位。

也可以直接下载 **Evenfall.exe**（小于 5 MB）。第一次运行没有预置任务，请自行创建。

GitHub 自动附带的 Source code (zip / tar.gz) 是源码，不是可运行程序。

## 1.0.1 修复

- 关机请求同时检查倒计时与系统时间，避免午夜前几毫秒提前提交；正常提醒保持原定时间。
- 记录区区分原计划、本次目标、实际请求时间（到秒）；保留旧日志原始事实。
- 停用持续有效，清除提醒和推迟项；提交前复核启用状态、任务版本、存在性及暂停状态。补充开关操作记录，并防止旧编辑草稿重新启用任务。
- 数据目录采用跨会话独占文件锁，阻止计划任务后台副本与桌面副本同时调度；锁定失败时不启动调度。
- 推迟 10／30／60 分钟选项在提醒卡片内部展开，支持 Tab、Enter、Esc，消除独立弹出菜单被遮挡的路径。
- 新增开发交接记录及午夜、停用、旧数据迁移回归测试。

本机已发现后台会话与桌面会话重复运行条件；旧版会话内互斥量无法拦截。历史日志没有进程标识，无法追溯哪一个副本提交了某次请求。菜单遮挡的原始操作路径未复现，本次移除了独立弹出菜单。

## 升级

先停止运行旧版的 Windows 计划任务并从托盘退出旧版，确认所有旧进程退出后复制备份 `%LOCALAPPDATA%\Evenfall`，再替换 EXE。新版读取旧数据并保存为 schemaVersion 2；回退 1.0.0 时须恢复升级前备份。建议仅使用程序内的登录自启动，避免另设非交互式计划任务；新版与旧版混用时，旧版不会遵守新增文件锁。取消本次不等于停用：前一天 23:59 取消紧接着的 00:00 后，再下一天 00:00 继续。

## 验证

{total} 项模拟检查通过。Windows 10 已完成本地验证；实际关机、Windows 11、真实登录启动、睡眠恢复和多个物理显示器尚待专用环境验证。本版本未做代码签名。

校验值见 SHA256SUMS.txt；使用说明和许可证包含在推荐的便携 ZIP 中。
""".replace("{total}", str(total))
    try:
        release = request(token, "GET", API + "/releases/tags/" + TAG)
    except urllib.error.HTTPError as error:
        if error.code != 404:
            raise
        release = request(token, "POST", API + "/releases",
                          {"tag_name": TAG, "target_commitish": target,
                           "name": "晚安 · 定时关机 " + VERSION,
                           "body": body, "draft": True, "prerelease": False})
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
                      {"draft": False, "body": body})
    print("Published: " + release["html_url"], flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--dist-dir", default="dist")
    parser.add_argument("--target-sha", help="Full verified commit SHA for the release tag")
    args = parser.parse_args()
    try:
        assets = prepare(args.dist_dir)
        if args.prepare_only:
            print("Verified release assets: " + ", ".join(file.name for file in assets))
        else:
            if not args.target_sha or not re.fullmatch(r"[0-9a-f]{40}", args.target_sha):
                raise RuntimeError("--target-sha must be a full verified commit SHA")
            publish(assets, args.target_sha)
    except (RuntimeError, urllib.error.URLError, subprocess.TimeoutExpired) as error:
        raise SystemExit("Release failed: " + str(error))
