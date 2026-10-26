# -*- coding: utf-8 -*-
"""生成语言包、检查 ZScript、组装插件，并（可选）装进 ZBrush。

插件 = KMDecimate.txt + KMDecimate\\（KMDecimate.exe、ZFileUtils64.dll、KMDecimate.GoZ、lang_zh.bin）
用法：py -3 tools/package.py                 只组装到 dist\\
      py -3 tools/package.py --install [目录…]  组装并复制进 ZPlugs64（默认 2026 用户目录和 2024 安装目录）
先运行 build.ps1 编译出 bin\\KMDecimate.exe。
"""
import filecmp
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZB = os.path.join(ROOT, "src", "zbrush")
DIST = os.path.join(ROOT, "dist", "ZPlugs64")
ZFU = r"G:\ai liabary\KMPaint\src\zbrush\KMPaint\ZFileUtils64.dll"
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
    shutil.copy2(ZFU, d)
    build_lang(os.path.join(d, "lang_zh.bin"))
    print("已组装到 %s" % DIST)


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
    if "--install" in sys.argv:
        targets = [a for a in sys.argv[1:] if a != "--install"] or DEFAULT
        for t in targets:
            install(t)


if __name__ == "__main__":
    main()
