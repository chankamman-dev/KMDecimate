# -*- coding: utf-8 -*-
"""ZScript 离线检查（在用户正在用的 ZBrush 里编译出错的脚本可能让 ZBrush 崩溃，所以先在这里查一遍）。

检查项：只含 ASCII；方括号和引号配对；命令名在已知列表里；例程先定义后调用、参数个数对；
kd 开头的变量在文本位置上先 VarDef 再使用（否则 ZBrush 报 Ambiguous input）；
内存块名字和变量名不冲突；不出现会让脚本静默中断的 [Val,[StrExtract,...]]；StrMerge 参数不超过 12 个。

用法：py -3 tools/zscript_lint.py src/zbrush/KMDecimate.txt
"""
import re
import sys

KNOWN = {
    "IConfig", "IShowActions", "ISubPalette", "IButton", "ISwitch", "ISlider", "IFreeze",
    "VarDef", "VarSet", "VarInc", "VarSave", "VarLoad", "If", "Loop", "LoopExit", "RoutineDef", "RoutineCall",
    "Exit", "Delay", "IGet", "ISet", "IPress", "IExists", "IGetMax", "IGetTitle", "Note", "NoteBar",
    "StrMerge", "StrLength", "StrExtract", "StrFind", "StrFromAsc", "StrToAsc", "FileExists", "FileNameResolvePath",
    "FileNameSetNext", "FileExecute", "MemCreate", "MemDelete", "MemWriteString", "MemReadString", "MemCopy",
    "MemSaveToFile", "MemCreateFromFile", "MemGetSize", "MemResize", "ZBrushInfo", "MemRead", "IColorSet",
    "IGetStatus", "IEnable", "SubToolGetActiveIndex", "SubToolGetFolderIndex", "SubToolGetStatus", "SubToolSetStatus", "SubToolSelect",
}
MEM_CMDS = {"MemCreate", "MemDelete", "MemWriteString", "MemReadString", "MemSaveToFile", "MemCreateFromFile",
            "MemGetSize", "MemResize", "MemRead"}
PREFIX = "kd"


def parse(text):
    pos, n = 0, len(text)

    def parse_list():
        nonlocal pos
        start = pos
        pos += 1
        items, cur = [], []
        while True:
            if pos >= n:
                raise SyntaxError("第 %d 行的 [ 没有闭合" % (text.count("\n", 0, start) + 1))
            ch = text[pos]
            if ch == "[":
                cur.append(parse_list())
                continue
            if ch == "]":
                items.append(cur)
                pos += 1
                return {"items": items, "at": start}
            if ch == ",":
                items.append(cur)
                cur = []
                pos += 1
                continue
            if ch == '"':
                end = text.find('"', pos + 1)
                if end < 0:
                    raise SyntaxError("第 %d 行引号没有闭合" % (text.count("\n", 0, pos) + 1))
                cur.append(("str", text[pos + 1:end], pos))
                pos = end + 1
                continue
            if text.startswith("//", pos) and (not cur and not items):
                pass
            j = pos
            while j < n and text[j] not in '[],"':
                j += 1
            tok = text[pos:j]
            if tok.strip():
                cur.append(("raw", tok.strip(), pos))
            pos = j

    nodes = []
    while pos < n:
        if text[pos] in " \t\r\n":
            pos += 1
            continue
        if text.startswith("//", pos):
            pos = text.find("\n", pos)
            if pos < 0:
                break
            continue
        if text[pos] != "[":
            raise SyntaxError("第 %d 行括号外有文字：%r" % (text.count("\n", 0, pos) + 1, text[pos:pos + 30]))
        nodes.append(parse_list())
    return nodes


def main(path):
    raw = open(path, "rb").read()
    errors = []
    bad = [i for i, b in enumerate(raw) if b > 126 or (b < 32 and b not in (9, 10, 13))]
    if bad:
        errors.append("含非 ASCII 字节（第 %d 行）" % (raw.count(b"\n", 0, bad[0]) + 1))
    text = raw.decode("ascii", errors="replace")
    # 去掉注释（只处理行首 // 注释，字符串里不会出现）
    text = re.sub(r"(?m)^\s*//.*$", lambda m: " " * len(m.group(0)), text)
    try:
        nodes = parse(text)
    except SyntaxError as e:
        print("失败 语法：", e)
        return 1

    line = lambda off: text.count("\n", 0, off) + 1
    defined = {}     # 变量名 -> 定义位置
    routines = {}    # 例程名 -> 参数
    mems = set()

    # 先收集所有 VarDef 的位置（文本顺序）与内存块名
    def collect(node):
        cmd = node["items"][0][0][1] if node["items"][0] and node["items"][0][0][0] == "raw" else None
        args = node["items"][1:]
        if cmd == "VarDef" and args and args[0] and args[0][0][0] == "raw":
            nm = re.match(r"[A-Za-z_]\w*", args[0][0][1]).group(0)
            defined.setdefault(nm, node["at"])
        if cmd in MEM_CMDS and args and args[0] and args[0][0][0] == "raw":
            mems.add(args[0][0][1])
        for a in args:
            for p in a:
                if isinstance(p, dict):
                    collect(p)
    for nd in nodes:
        collect(nd)
    for m in mems:
        if m in defined:
            errors.append("内存块名 %s 和变量同名" % m)

    def walk(node, params):
        items = node["items"]
        if not items[0] or items[0][0][0] != "raw":
            errors.append("第 %d 行：列表没有命令名" % line(node["at"]))
            return
        cmd = items[0][0][1]
        args = items[1:]
        if cmd not in KNOWN:
            errors.append("第 %d 行：未知命令 %s" % (line(node["at"]), cmd))
        if cmd == "StrMerge" and len(args) > 12:
            errors.append("第 %d 行：StrMerge 参数 %d 个（上限 12）" % (line(node["at"]), len(args)))
        if cmd == "Val" or (cmd == "Val" and args and any(isinstance(p, dict) for p in args[0])):
            errors.append("第 %d 行：不要用 Val（[Val,[StrExtract]] 会静默中断）" % line(node["at"]))
        if cmd == "RoutineCall":
            r = args[0][0][1]
            if r not in routines:
                errors.append("第 %d 行：调用 %s 时它还没定义" % (line(node["at"]), r))
            elif len(args) - 1 != len(routines[r]):
                errors.append("第 %d 行：%s 需要 %d 个参数，给了 %d 个" % (line(node["at"]), r, len(routines[r]), len(args) - 1))
        for i, a in enumerate(args):
            for p in a:
                if isinstance(p, dict):
                    walk(p, params)
                elif p[0] == "raw":
                    if cmd in MEM_CMDS and i == 0:
                        continue
                    if cmd == "RoutineCall" and i == 0:
                        continue
                    if cmd == "FileNameSetNext" and "MemBlock:" in p[1]:
                        continue
                    for ident in re.findall(r"[A-Za-z_]\w*", p[1]):
                        if not ident.startswith(PREFIX) or ident in params or ident in mems or ident in routines:
                            continue
                        at = defined.get(ident)
                        if at is None:
                            errors.append("第 %d 行：变量 %s 从未 VarDef" % (line(p[2]), ident))
                        elif at > p[2] and not (cmd == "VarDef" and i == 0):
                            errors.append("第 %d 行：变量 %s 在第 %d 行才定义（会报 Ambiguous input）" % (line(p[2]), ident, line(at)))

    for nd in nodes:
        items = nd["items"]
        cmd = items[0][0][1] if items[0] and items[0][0][0] == "raw" else None
        if cmd == "RoutineDef":
            rname = items[1][0][1]
            params = [a[0][1] for a in items[3:] if a]
            for pr in params:
                if pr in defined:
                    errors.append("例程 %s 的参数 %s 和全局变量同名" % (rname, pr))
            routines[rname] = params
            for p in items[2]:
                if isinstance(p, dict):
                    walk(p, set(params))
        else:
            walk(nd, set())

    for e in errors:
        print("失败", e)
    print("%s：%d 个例程，%d 个变量，%d 个内存块，问题 %d 个" % (path, len(routines), len(defined), len(mems), len(errors)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "src/zbrush/KMDecimate.txt"))
