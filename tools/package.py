# -*- coding: utf-8 -*-
"""生成语言包、检查 ZScript、组装插件，并（可选）装进 ZBrush，或者打成给别人用的安装包。

插件 = KMDecimate.txt + KMDecimate\\（KMDecimate.exe、ZFileUtils64.dll、KMDecimate.GoZ、lang_zh.bin）
用法：py -3 tools/package.py                 只组装到 dist\\
      py -3 tools/package.py --install [目录…]  组装并复制进 ZPlugs64（默认 2026 用户目录和 2024 安装目录）
      py -3 tools/package.py --zip [目录]       组装并打成 dist\\KMDecimate_<版本>_<日期>.zip（再复制一份到 目录）
先运行 build.ps1 编译出 bin\\KMDecimate.exe。
ZFileUtils64.dll 依次找：环境变量 KM_ZFU、ZFU、同级的 KMPaint 仓库。

安装包里：安装.bat / 卸载.bat / install.ps1 / 使用说明.txt（src\\installer\\），
files\\ 下是插件本身，files\\placeholder\\KMDecimate.zsc 是第一次安装时放的占位编译文件（ZBrush 发现 .txt 更新会重新编译）。
"""
import datetime
import filecmp
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZB = os.path.join(ROOT, "src", "zbrush")
INST = os.path.join(ROOT, "src", "installer")
DIST = os.path.join(ROOT, "dist", "ZPlugs64")
ZFU = r"G:\ai liabary\KMPaint\src\zbrush\KMPaint\ZFileUtils64.dll"
ZFU_OTHERS = [os.path.join(os.path.dirname(ROOT), "KMPaint", "src", "zbrush", "KMPaint", "ZFileUtils64.dll")]
DEFAULT = [r"F:\Zbrush Assest\ZStartup\ZPlugs64", r"F:\art software\zbrush 2024.0.4\ZStartup\ZPlugs64"]
KEY, LABEL, TIP = 32, 64, 160


def field(text, size, what):
    b = text.encode("utf-8")
    if len(b) >= size:
        raise SystemExit("%s 太长（%d 字节，上限 %d）：%s" % (what, len(b), size - 1, text))
    return b + b"\0" * (size - len(b))


def build_lang(out):
    rows = {}
    with open(os.path.join(ZB, "lang_zh.tsv"), encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            parts = (line.split("\t") + ["", "", ""])[:4]
            rows[int(parts[0])] = parts[1:]
    n = max(max(rows) + 1, 16)
    data = bytearray()
    for i in range(n):
        key, label, tip = rows.get(i, ["", "", ""])
        data += field(key, KEY, "键") + field(label, LABEL, "标签") + field(tip, TIP, "提示")
    with open(out, "wb") as f:
        f.write(bytes(data))


def assemble():
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "zscript_lint.py"), os.path.join(ZB, "KMDecimate.txt")])
    if r.returncode:
        raise SystemExit("ZScript 检查没通过")
    d = os.path.join(DIST, "KMDecimate")
    os.makedirs(d, exist_ok=True)
    shutil.copy2(os.path.join(ZB, "KMDecimate.txt"), os.path.join(DIST, "KMDecimate.txt"))
    shutil.copy2(os.path.join(ROOT, "bin", "KMDecimate.exe"), d)
    shutil.copy2(os.path.join(ZB, "KMDecimate", "KMDecimate.GoZ"), d)
    zfu = [p for p in [os.environ.get("KM_ZFU", ""), ZFU] + ZFU_OTHERS if p and os.path.exists(p)]
    if not zfu:
        raise SystemExit("找不到 ZFileUtils64.dll：设置环境变量 KM_ZFU 指向它")
    shutil.copy2(zfu[0], d)
    build_lang(os.path.join(d, "lang_zh.bin"))
    print("已组装到 %s" % DIST)


def version():
    text = open(os.path.join(ZB, "KMDecimate.txt"), encoding="ascii").read()
    ver = re.match(r"// KM Decimate (\d+(\.\d+)+)", text).group(1)
    # “检查更新”按钮交给 exe 的版本号（kdUpd 1–3）要和第一行一样，否则会一直提示有新版本或者漏掉新版本
    upd = [int(re.search(r"\[VarSet,kdUpd\(%d\),(\d+)\]" % i, text).group(1)) for i in (1, 2, 3)]
    want = (ver.split(".") + ["0", "0"])[:3]
    if upd != [int(x) for x in want]:
        raise SystemExit("KMDecimate.txt 里 kdUpd 的版本 %s 和第一行的 %s 不一样" % (upd, ver))
    return ver


def make_zip(copy_to):
    ver = version()
    name = "KMDecimate_" + ver
    out = os.path.join(ROOT, "dist", name)
    if os.path.exists(out):
        shutil.rmtree(out)
    shutil.copytree(DIST, os.path.join(out, "files"))
    os.makedirs(os.path.join(out, "files", "placeholder"))
    shutil.copy2(os.path.join(INST, "placeholder", "KMDecimate.zsc"), os.path.join(out, "files", "placeholder"))
    for f in ["安装.bat", "卸载.bat"]:
        shutil.copy2(os.path.join(INST, f), out)
    for f in ["install.ps1", "使用说明.txt"]:   # 版本号写进去；保持 UTF-8 带 BOM（PowerShell 5.1 和记事本都认）
        text = open(os.path.join(INST, f), encoding="utf-8-sig").read().replace("{{VER}}", ver)
        with open(os.path.join(out, f), "w", encoding="utf-8-sig", newline="") as w:
            w.write(text)
    zpath = os.path.join(ROOT, "dist", "%s_%s.zip" % (name, datetime.date.today().strftime("%Y%m%d")))
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:   # 中文文件名按 UTF-8 写入（Windows 资源管理器能正确显示）
        for d, _, files in os.walk(out):
            for f in files:
                p = os.path.join(d, f)
                z.write(p, os.path.join(name, os.path.relpath(p, out)).replace("\\", "/"))
    with zipfile.ZipFile(zpath) as z:
        assert z.testzip() is None
        n = len(z.infolist())
    print("打包完成：%s（%d 个文件，%.1f MB）" % (zpath, n, os.path.getsize(zpath) / 1048576.0))
    if copy_to:
        shutil.copy2(zpath, copy_to)
        print("已复制到 %s" % copy_to)


def install(target):
    if not os.path.isdir(target):
        print("跳过（目录不存在）：%s" % target)
        return
    for root, _, files in os.walk(DIST):
        rel = os.path.relpath(root, DIST)
        dst = os.path.normpath(os.path.join(target, rel))
        os.makedirs(dst, exist_ok=True)
        for f in files:
            src, out = os.path.join(root, f), os.path.join(dst, f)
            # 内容相同就不复制：ZFileUtils64.dll 在 ZBrush 运行时被占用
            if os.path.exists(out) and filecmp.cmp(src, out, shallow=False):
                continue
            shutil.copy2(src, out)
    print("已安装到 %s" % target)


def main():
    assemble()
    if "--zip" in sys.argv:
        rest = [a for a in sys.argv[1:] if a != "--zip"]
        make_zip(rest[0] if rest else "")
    elif "--install" in sys.argv:
        targets = [a for a in sys.argv[1:] if a != "--install"] or DEFAULT
        for t in targets:
            install(t)


if __name__ == "__main__":
    main()
