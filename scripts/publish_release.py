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
VERSION = "1.0.0"
TAG = "v" + VERSION
API = "https://api.github.com/repos/" + REPOSITORY


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare():
    dist = ROOT / "dist"
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
    body = """## 下载与使用

推荐下载 **Evenfall-1.0.0-win-x64.zip**，解压后双击 **Evenfall.exe**。免安装，无需另装运行库，适用于 Windows 10/11 64 位。

也可以直接下载 **Evenfall.exe**（约 1.28 MB）。第一次运行没有预置任务，请自行创建。

GitHub 自动附带的 Source code (zip / tar.gz) 是源码，不是可运行程序。

## 功能

- 执行一次、按星期重复、快速倒计时；支持多个任务。
- 提前 60 秒提醒，可取消本次或推迟 10 / 30 / 60 分钟；重复计划继续保留。
- 托盘常驻、暂停恢复、登录自启动、浅色／深色主题及执行记录。

## 验证

115 项模拟检查通过。Windows 10 已完成本地验证；实际关机、Windows 11、真实登录启动、睡眠恢复和多个物理显示器尚待专用环境验证。本版本未做代码签名。

校验值见 SHA256SUMS.txt；使用说明和许可证包含在推荐的便携 ZIP 中。
"""
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
    parser.add_argument("--target-sha", help="Full verified commit SHA for the release tag")
    args = parser.parse_args()
    try:
        assets = prepare()
        if args.prepare_only:
            print("Verified release assets: " + ", ".join(file.name for file in assets))
        else:
            if not args.target_sha or not re.fullmatch(r"[0-9a-f]{40}", args.target_sha):
                raise RuntimeError("--target-sha must be a full verified commit SHA")
            publish(assets, args.target_sha)
    except (RuntimeError, urllib.error.URLError, subprocess.TimeoutExpired) as error:
        raise SystemExit("Release failed: " + str(error))
