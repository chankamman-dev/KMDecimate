"""Compares the last KM Remesh run with pre-decimation and the last one without it.

Reads (all written by the plugin into the work folder):
  zr_last_pre.zvr / zr_last_full.zvr    step times (see kdRT in KMDecimate.txt)
  zr_base_pre.GoZ / zr_base_full.GoZ    the ZRemesher result (one level): topology
  zr_final_pre.GoZ / zr_final_full.GoZ  the final mesh: how far apart the two results are
Writes zr_compare_base.png (wireframes side by side: left with pre-decimation, right without) into the work folder.

usage: py -3 tools/compare_remesh.py [work folder]   (default C:\\Users\\Public\\KMDecimate\\work)
"""
import math
import os
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\Public\KMDecimate\work"
BIN = os.path.join(ROOT, "bin")
STEPS = [(1, "复制"), (16, "  预减面：导出"), (17, "  预减面：减面"), (18, "  预减面：导入"), (2, "预减面合计"),
         (3, "ZRemesher"), (14, "准备投射"), (4, "细分并投射"), (5, "  其中最高一级"), (15, "收尾"), (6, "合计")]
COUNTS = [(7, "原模型面数"), (8, "预减面后"), (9, "ZRemesher 后"), (10, "最终面数"), (11, "细分级数"), (19, "子工具数")]


def zvr(path):
    b = open(path, "rb").read()
    n = struct.unpack_from("<I", b, 4)[0]
    return [struct.unpack_from("<f", b, 8 + i * 5 + 1)[0] for i in range(n)]


def goz_faces(path):
    """Points and faces (3 or 4 indices) of a GoZ file."""
    d = open(path, "rb").read()
    pos, pts, faces = 32, [], []
    while pos + 8 <= len(d):
        tag, size = struct.unpack_from("<II", d, pos)
        if tag == 0 or size < 8:
            break
        body = pos + 8
        if tag == 10001:
            n = struct.unpack_from("<Q", d, body)[0]
            pts = [struct.unpack_from("<3f", d, body + 8 + i * 12) for i in range(n)]
        elif tag == 20001:
            n = struct.unpack_from("<Q", d, body)[0]
            for i in range(n):
                q = struct.unpack_from("<4I", d, body + 8 + i * 16)
                faces.append(q[:3] if q[3] == 0xFFFFFFFF else q)
        pos += size
    return pts, faces


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def topology(pts, faces):
    edge_faces, nbr = {}, {}
    quad_aspect, quad_angle = [], []
    for f in faces:
        k = len(f)
        for i in range(k):
            a, b = f[i], f[(i + 1) % k]
            e = (a, b) if a < b else (b, a)
            edge_faces[e] = edge_faces.get(e, 0) + 1
            nbr.setdefault(a, set()).add(b)
            nbr.setdefault(b, set()).add(a)
        if k == 4:
            sides = [dist(pts[f[i]], pts[f[(i + 1) % 4]]) for i in range(4)]
            if min(sides) > 0:
                quad_aspect.append(max(sides) / min(sides))
            worst = 0.0
            for i in range(4):
                p, a, b = pts[f[i]], pts[f[i - 1]], pts[f[(i + 1) % 4]]
                u = [x - y for x, y in zip(a, p)]
                v = [x - y for x, y in zip(b, p)]
                nu, nv = math.sqrt(sum(x * x for x in u)), math.sqrt(sum(x * x for x in v))
                if nu > 0 and nv > 0:
                    c = max(-1.0, min(1.0, sum(x * y for x, y in zip(u, v)) / (nu * nv)))
                    worst = max(worst, abs(math.degrees(math.acos(c)) - 90))
            quad_angle.append(worst)
    border = set()
    for (a, b), c in edge_faces.items():
        if c == 1:
            border.update((a, b))
    val = {}
    for v, s in nbr.items():
        if v not in border:
            val[len(s)] = val.get(len(s), 0) + 1
    lens = [dist(pts[a], pts[b]) for a, b in edge_faces]
    mean = sum(lens) / len(lens)
    cv = math.sqrt(sum((x - mean) ** 2 for x in lens) / len(lens)) / mean

    def pct(xs, p):
        xs = sorted(xs)
        return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else 0

    interior = sum(val.values())
    poles = interior - val.get(4, 0)
    return {"面数": len(faces), "四边形": sum(1 for f in faces if len(f) == 4), "三角形": sum(1 for f in faces if len(f) == 3),
            "极点（内部度数≠4）": "%d（%.1f%%：3 度 %d，5 度 %d，6 度及以上 %d）" % (
                poles, 100.0 * poles / max(interior, 1), val.get(3, 0), val.get(5, 0), sum(c for d, c in val.items() if d >= 6)),
            "边界点": len(border), "边长变异系数（越小越均匀）": "%.3f" % cv,
            "四边形长宽比 中位 / 95%": "%.2f / %.2f" % (pct(quad_aspect, 0.5), pct(quad_aspect, 0.95)),
            "四边形最大偏离直角 中位 / 95%": "%.1f° / %.1f°" % (pct(quad_angle, 0.5), pct(quad_angle, 0.95))}


def read_png(path):
    data = open(path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif typ == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    return w, h, [raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)] for y in range(h)]


def write_png(path, w, h, rows):
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(b"\0" + r for r in rows), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def side_by_side(out, files):
    imgs = [read_png(f) for f in files]
    gap = b"\xff\xff\xff" * 8
    rows = [gap.join(im[2][y] for im in imgs) for y in range(imgs[0][1])]
    write_png(out, sum(im[0] for im in imgs) + 8 * (len(imgs) - 1), imgs[0][1], rows)


def main():
    p = {tag: os.path.join(WORK, "zr_last_%s.zvr" % tag) for tag in ("pre", "full")}
    missing = [f for f in p.values() if not os.path.exists(f)]
    if missing:
        sys.exit("还缺：" + "、".join(missing) + "（预减面开和关各重拓扑一次）")
    r = {tag: zvr(f) for tag, f in p.items()}
    print("%-16s %12s %12s" % ("", "预减面", "不预减面"))
    for i, name in STEPS:
        print("%-16s %11.1fs %11.1fs" % (name, r["pre"][i], r["full"][i]))
    for i, name in COUNTS:
        print("%-16s %12d %12d" % (name, r["pre"][i], r["full"][i]))
    print("预减面目标 %d 个三角形" % r["pre"][12])

    base = {tag: os.path.join(WORK, "zr_base_%s.GoZ" % tag) for tag in ("pre", "full")}
    if all(os.path.exists(f) for f in base.values()):
        print("\nZRemesher 结果的布线")
        t = {tag: topology(*goz_faces(f)) for tag, f in base.items()}
        for k in t["pre"]:
            print("  %-26s 预减面 %-36s 不预减面 %s" % (k, t["pre"][k], t["full"][k]))
        shots = []
        for tag in ("pre", "full"):
            png = os.path.join(WORK, "zr_shot_%s.png" % tag)
            subprocess.run([os.path.join(BIN, "meshshot.exe"), base[tag], png, "--view", "-z", "--flipy", "1",
                            "--wire", "1", "--quads", "1", "--size", "700"], capture_output=True)
            shots.append(png)
        out = os.path.join(WORK, "zr_compare_base.png")
        side_by_side(out, shots)
        print("  线框对比图：%s（左：预减面，右：不预减面）" % out)

    final = {tag: os.path.join(WORK, "zr_final_%s.GoZ" % tag) for tag in ("pre", "full")}
    if all(os.path.exists(f) for f in final.values()):
        print("\n两个最终模型之间的距离（以不预减面为准）")
        res = subprocess.run([os.path.join(BIN, "meshdiff.exe"), final["full"], final["pre"], "--samples", "500000"],
                             capture_output=True, text=True, encoding="utf-8", errors="replace")
        for line in res.stdout.splitlines():
            if line.startswith(("原模型", "结果", "DIFF", "MAXAT")):
                print("  " + line)


if __name__ == "__main__":
    main()
