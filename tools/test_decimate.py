# -*- coding: utf-8 -*-
"""KMDecimate 离线自动化回归测试（不依赖 ZBrush 或外部测试模型）。

覆盖：
1. ZScript 静态检查（tools/zscript_lint.py）
2. 开放边界 + 分块接缝防撕裂回归测试（decimate.h plan LOCKED/BORDER 修复）
3. 不可再减网格防空转 64 遍回归测试
4. 小模型（< 20,000 面）单遍完成测试
5. 离群点/极稀疏包围盒切块防 OOM 测试
6. GoZ 流式读写（含四边形、顶点色、polygroup）与损坏/截断文件防护测试
7. job.zvr 交接、run_id / peak_commit_mib / 失败上下文日志测试（含调用 test_job_handoff.py）
8. 检查更新在损坏 update.zvr 下的降级处理测试

用法：py -3 tools/test_decimate.py
"""
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "bin")
KMDEC = os.path.join(BIN, "kmdec.exe")
KMDEC_GUI = os.path.join(BIN, "KMDecimate.exe")
MESHGEN = os.path.join(BIN, "meshgen.exe")
MESHDIFF = os.path.join(BIN, "meshdiff.exe")

fails = 0


def check(name, ok, info=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + (("  " + str(info)) if info and not ok else ""))
    if not ok:
        fails += 1


def run_cmd(args, env_extra=None, timeout=120):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def parse_timing(out):
    m = re.search(r"TIMING (.+)", out)
    if not m:
        return {}
    res = {}
    for tok in m.group(1).split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                res[k] = float(v)
            except ValueError:
                res[k] = v
    return res


def zvr(values):
    b = struct.pack("<II", 1001, len(values))
    for v in values:
        b += b"F" + struct.pack("<f", v)
    return b


def write_icosahedron(path):
    t = (1.0 + math.sqrt(5.0)) / 2.0
    verts = [
        (-1, t, 0), (1, t, 0), (-1, -t, 0), (1, -t, 0),
        (0, -1, t), (0, 1, t), (0, -1, -t), (0, 1, -t),
        (t, 0, -1), (t, 0, 1), (-t, 0, -1), (-t, 0, 1),
    ]
    faces = [
        (0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11),
        (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
        (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9),
        (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1),
    ]
    with open(path, "w", encoding="ascii") as f:
        for v in verts:
            f.write(f"v {v[0]} {v[1]} {v[2]}\n")
        for a, b, c in faces:
            f.write(f"f {a+1} {b+1} {c+1}\n")


def write_grid_obj(path, n, flat=False):
    with open(path, "w", encoding="ascii") as f:
        for y in range(n + 1):
            for x in range(n + 1):
                z = 0.0 if flat else 0.02 * math.sin(x * 0.2) * math.cos(y * 0.2)
                f.write(f"v {x/n:.6f} {y/n:.6f} {z:.6f}\n")
        for y in range(n):
            for x in range(n):
                i = y * (n + 1) + x + 1
                f.write(f"f {i} {i+1} {i+n+2}\n")
                f.write(f"f {i} {i+n+2} {i+n+1}\n")


def analyze_obj_topology(obj_path):
    edges = {}
    verts = []
    nfaces = 0
    with open(obj_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("v "):
                verts.append(tuple(map(float, line.split()[1:4])))
            elif line.startswith("f "):
                nfaces += 1
                idx = [int(tok.split("/")[0]) - 1 for tok in line.split()[1:4]]
                for a, b in ((idx[0], idx[1]), (idx[1], idx[2]), (idx[2], idx[0])):
                    k = (min(a, b), max(a, b))
                    edges[k] = edges.get(k, 0) + 1
    b_edges = [k for k, c in edges.items() if c == 1]
    nm_edges = [k for k, c in edges.items() if c > 2]
    adj = {}
    for a, b in b_edges:
        adj.setdefault(a, []).append(b)
        adj.setdefault(b, []).append(a)
    seen, loops = set(), 0
    for v in adj:
        if v not in seen:
            loops += 1
            st = [v]
            while st:
                u = st.pop()
                if u in seen:
                    continue
                seen.add(u)
                st.extend(adj[u])
    chi = len(verts) - len(edges) + nfaces
    return len(b_edges), len(nm_edges), loops, chi


def write_quad_goz(path, nx, ny, with_mask=False, with_uv_seam=False):
    """写一个带四边形、顶点色、polygroup（以及可选 Mask16 / 双岛 UV4 接缝）的 GoZ 文件。"""
    verts = []
    colors = []
    masks = []
    for y in range(ny + 1):
        for x in range(nx + 1):
            z = 0.05 * math.sin(x * 0.25) * math.cos(y * 0.25)
            verts.append((x / nx, y / ny, z))
            r = 250 if x < nx // 2 else 30
            g = 80 if y < ny // 2 else 220
            b = 40
            colors.append((b, g, r, 255))
            if with_mask:
                # 左半边完全遮罩（0），右半边无遮罩（65535）
                masks.append(0 if x < nx // 2 else 65535)
    quads = []
    groups = []
    uvs = []
    for y in range(ny):
        for x in range(nx):
            i = y * (nx + 1) + x
            quads.append((i, i + 1, i + nx + 2, i + nx + 1))
            groups.append(1 if x < nx // 2 else 2)
            if with_uv_seam:
                # 左岛 u ∈ [0.0, 0.4]，右岛 u ∈ [0.6, 1.0]，中间 [0.4, 0.6] 为 UV 接缝空隙
                left = x < nx // 2
                def uv_of(gx, gy):
                    fx, fy = gx / nx, gy / ny
                    return (fx * 0.8 if left else 0.6 + (fx - 0.5) * 0.8), fy
                u0, v0 = uv_of(x, y)
                u1, v1 = uv_of(x + 1, y)
                u2, v2 = uv_of(x + 1, y + 1)
                u3, v3 = uv_of(x, y + 1)
                uvs.append((u0, v0, u1, v1, u2, v2, u3, v3))

    out = bytearray()
    out += b"GoZb 1.0 ZBrush GoZ Binary......"
    # Points
    out += struct.pack("<IIQ", 10001, 16 + len(verts) * 12, len(verts))
    for x, y, z in verts:
        out += struct.pack("<fff", x, y, z)
    # Faces (quads)
    out += struct.pack("<IIQ", 20001, 16 + len(quads) * 16, len(quads))
    for q in quads:
        out += struct.pack("<IIII", *q)
    # UV4
    if with_uv_seam:
        out += struct.pack("<IIQ", 25001, 16 + len(uvs) * 32, len(uvs))
        for rec in uvs:
            out += struct.pack("<8f", *rec)
    # Mrgb
    out += struct.pack("<IIIf", 35001, 16 + len(colors) * 4, len(colors), 1.0)
    for b, g, r, m in colors:
        out += struct.pack("<BBBB", b, g, r, m)
    # Mask16
    if with_mask:
        out += struct.pack("<IIQ", 30002, 16 + len(masks) * 2, len(masks))
        for mv in masks:
            out += struct.pack("<H", mv)
    # Groups
    out += struct.pack("<IIQ", 40001, 16 + len(groups) * 2, len(groups))
    for grp in groups:
        out += struct.pack("<H", grp)
    out += b"\0" * 16
    with open(path, "wb") as f:
        f.write(out)


def parse_goz_chunks(path):
    """解析 GoZ 文件中的 Points / Faces / UV4 / Mask16 / Mrgb 块用于校验。"""
    d = open(path, "rb").read()
    pos = 32
    res = {"tags": set(), "verts": [], "tris": [], "uvs": [], "masks": []}
    while pos + 8 <= len(d):
        tag, sz = struct.unpack_from("<II", d, pos)
        if tag == 0 or sz < 8:
            break
        res["tags"].add(tag)
        body = pos + 8
        if tag == 10001:
            n = struct.unpack_from("<Q", d, body)[0]
            res["verts"] = [struct.unpack_from("<fff", d, body + 8 + i * 12) for i in range(n)]
        elif tag == 20001:
            n = struct.unpack_from("<Q", d, body)[0]
            for i in range(n):
                q0, q1, q2, q3 = struct.unpack_from("<IIII", d, body + 8 + i * 16)
                res["tris"].append((q0, q1, q2))
                if q3 != 0xFFFFFFFF:
                    res["tris"].append((q0, q2, q3))
        elif tag == 25001:
            n = struct.unpack_from("<Q", d, body)[0]
            res["uvs"] = [struct.unpack_from("<8f", d, body + 8 + i * 32) for i in range(n)]
        elif tag == 30002:
            n = struct.unpack_from("<Q", d, body)[0]
            res["masks"] = list(struct.unpack_from(f"<{n}H", d, body + 8))
        pos += sz
    return res


def main():
    for exe in (KMDEC, KMDEC_GUI, MESHGEN, MESHDIFF):
        if not os.path.exists(exe):
            raise SystemExit(f"缺少可执行文件：{exe}，请先运行 build.ps1")

    work = tempfile.mkdtemp(prefix="kd_reg_")
    try:
        # 1. ZScript lint
        rc, out = run_cmd([sys.executable, os.path.join(ROOT, "tools", "zscript_lint.py"),
                           os.path.join(ROOT, "src", "zbrush", "KMDecimate.txt")])
        check("zscript_lint passes", rc == 0, out.strip())

        # 2. Seam + open-border crack regression: flat open sheet (border=0 forces border-adjacent seam edges to cost=0!)
        # With the old decimate.h:315 bug, LOCKED seam vertices adjacent to BORDER were deleted, cracking the sheet (chi!=1 / loops>1).
        flat80k = os.path.join(work, "flat80k.obj")
        flat_out = os.path.join(work, "flat_out.obj")
        write_grid_obj(flat80k, 200, flat=True)  # 80,000 tris
        rc, out = run_cmd([KMDEC, flat80k, flat_out, "--percent", "30", "--border", "0"])
        b_edges, nm_edges, loops, chi = analyze_obj_topology(flat_out)
        check("seam+border crack regression (flat sheet, border=0, 30%)",
              rc == 0 and chi == 1 and nm_edges == 0 and loops == 1,
              f"rc={rc} b_edges={b_edges} nm={nm_edges} loops={loops} chi={chi}")

        grid80k = os.path.join(work, "grid80k.obj")
        grid_out2 = os.path.join(work, "grid_out2.obj")
        write_grid_obj(grid80k, 200)  # 80,000 tris
        rc, out = run_cmd([KMDEC, grid80k, grid_out2, "--percent", "5"])
        b_edges, nm_edges, loops, chi = analyze_obj_topology(grid_out2)
        check("seam+border wavy sheet default border (5%)",
              rc == 0 and chi == 1 and nm_edges == 0 and loops == 1,
              f"rc={rc} b_edges={b_edges} nm={nm_edges} loops={loops} chi={chi}")

        # 3. Uncollapsible mesh -> must NOT spin 64 passes!
        # Construct 60 disjoint tiny tetrahedra (240 tris) or non-manifold fan where every vertex is PERMLOCK
        nm_obj = os.path.join(work, "nonmanifold.obj")
        nm_out = os.path.join(work, "nonmanifold_out.obj")
        with open(nm_obj, "w", encoding="ascii") as f:
            # 300 triangles sharing edge (0,1) -> all edges on (0,1) are non-manifold -> PERMLOCK
            f.write("v 0 0 0\nv 1 0 0\n")
            for i in range(200):
                ang = 2 * math.pi * i / 200
                f.write(f"v 0.5 {math.cos(ang):.6f} {math.sin(ang):.6f}\n")
            for i in range(200):
                f.write(f"f 1 2 {i+3}\n")
        rc, out = run_cmd([KMDEC, nm_obj, nm_out, "--percent", "1"])
        tm = parse_timing(out)
        check("uncollapsible mesh stops early (passes <= 2, not 64)",
              rc == 0 and tm.get("passes", 99) <= 2,
              f"rc={rc} timing={tm}")

        # 4. Small mesh (< 20k tris) finishes in 1 pass
        grid5k = os.path.join(work, "grid5k.obj")
        grid5k_out = os.path.join(work, "grid5k_out.obj")
        write_grid_obj(grid5k, 50)  # 5,000 tris
        rc, out = run_cmd([KMDEC, grid5k, grid5k_out, "--percent", "10"])
        tm = parse_timing(out)
        check("small mesh (<20k tris) runs in 1 pass",
              rc == 0 and tm.get("passes") == 1 and abs(tm.get("out_tris", 0) - 500) <= 5,
              f"rc={rc} timing={tm}")

        # 5. Sparse / outlier bounding box (previously could OOM denseOf or throw '切块失败')
        ico = os.path.join(work, "ico.obj")
        write_icosahedron(ico)
        ico80k = os.path.join(work, "ico80k.obj")
        rc, out = run_cmd([MESHGEN, ico, ico80k, "--levels", "6", "--amp", "0.3"])  # 20 * 4^6 = 81,920 tris
        check("meshgen 80k sphere", rc == 0, out)
        outlier_obj = os.path.join(work, "outlier.obj")
        shutil.copy(ico80k, outlier_obj)
        with open(outlier_obj, "r", encoding="ascii") as f:
            nv_base = sum(1 for ln in f if ln.startswith("v "))
        with open(outlier_obj, "a", encoding="ascii") as f:
            f.write("v 100000 100000 100000\nv 100001 100000 100000\nv 100000 100001 100000\n")
            f.write(f"f {nv_base+1} {nv_base+2} {nv_base+3}\n")
        outlier_out = os.path.join(work, "outlier_out.obj")
        rc, out = run_cmd([KMDEC, outlier_obj, outlier_out, "--percent", "10"])
        tm = parse_timing(out)
        check("sparse/outlier bbox succeeds via sparse cell mapping",
              rc == 0 and abs(tm.get("out_tris", 0) - 8192) < 500,
              f"rc={rc} timing={tm} out={out[-200:]}")

        # 6. GoZ streaming round-trip (quads + polypaint + polygroups) + corrupted GoZ protection
        quad_goz = os.path.join(work, "quad_in.GoZ")
        quad_out = os.path.join(work, "quad_out.GoZ")
        write_quad_goz(quad_goz, 120, 120)  # 14,400 quads = 28,800 tris
        rc, out = run_cmd([KMDEC, quad_goz, quad_out, "--percent", "15", "--groups", "10"])
        tm = parse_timing(out)
        check("GoZ streaming quad+color+groups round-trip",
              rc == 0 and tm.get("in_tris") == 28800 and abs(tm.get("out_tris", 0) - 4320) < 200,
              f"rc={rc} timing={tm}")
        rc_d, out_d = run_cmd([MESHDIFF, quad_goz, quad_out])
        check("GoZ round-trip meshdiff valid with color",
              rc_d == 0 and "COLOR mean=" in out_d,
              out_d.strip())

        # Corrupted / truncated GoZ -> must return error cleanly, no crash
        bad_goz = os.path.join(work, "bad_trunc.GoZ")
        raw = open(quad_goz, "rb").read()
        open(bad_goz, "wb").write(raw[:len(raw) // 2])
        rc, out = run_cmd([KMDEC, bad_goz, os.path.join(work, "bad_out.GoZ"), "--percent", "10"])
        check("truncated GoZ returns clean error (rc==2)", rc == 2 and "出错" in out, f"rc={rc} out={out}")

        # Out-of-bounds vertex index in GoZ
        oob_goz = os.path.join(work, "oob.GoZ")
        oob = bytearray(b"GoZb 1.0 ZBrush GoZ Binary......")
        oob += struct.pack("<IIQfff", 10001, 16 + 12, 1, 0.0, 0.0, 0.0)
        oob += struct.pack("<IIQIIII", 20001, 16 + 16, 1, 0, 1, 99999, 0xFFFFFFFF)
        oob += b"\0" * 16
        open(oob_goz, "wb").write(oob)
        rc, out = run_cmd([KMDEC, oob_goz, os.path.join(work, "oob_out.GoZ"), "--percent", "10"])
        check("OOB vertex index GoZ rejected cleanly", rc == 2 and "越界" in out, f"rc={rc} out={out}")

        # 7. Job handoff suite + new log fields (run=, part=, peak_commit_mib=) + error context
        painted_goz = os.path.join(work, "painted320k.GoZ")
        rc, out = run_cmd([MESHGEN, ico, painted_goz, "--levels", "7", "--amp", "0.3", "--paint", "12"])
        check("meshgen painted GoZ for handoff tests", rc == 0, out)
        rc, out = run_cmd([sys.executable, os.path.join(ROOT, "tools", "test_job_handoff.py"), KMDEC_GUI, painted_goz])
        check("test_job_handoff.py all checks pass", rc == 0, out)

        # Verify run=, part=, peak_commit_mib= and error diagnostics directly
        jdir = os.path.join(work, "jobtest")
        os.makedirs(jdir, exist_ok=True)
        shutil.copy(painted_goz, os.path.join(jdir, "in.GoZ"))
        open(os.path.join(jdir, "job.zvr"), "wb").write(zvr([2, 10, 1, 0, 8, 108, 424242, 0]))
        rc, _ = run_cmd([KMDEC_GUI, os.path.join(jdir, "job.zvr")])
        log_line = open(os.path.join(jdir, "log.txt"), encoding="utf-8").read().strip().splitlines()[-1]
        check("job log records run_id, part, peak_commit_mib",
              rc == 0 and "run=424242 " in log_line and "part=" in log_line and "peak_commit_mib=" in log_line,
              log_line)

        # Trigger error during load -> check structured error context
        shutil.copy(bad_goz, os.path.join(jdir, "in.GoZ"))
        open(os.path.join(jdir, "job.zvr"), "wb").write(zvr([2, 10, 1, 0, 8, 108, 424243, 0]))
        rc, _ = run_cmd([KMDEC_GUI, os.path.join(jdir, "job.zvr")])
        err_line = open(os.path.join(jdir, "log.txt"), encoding="utf-8").read().strip().splitlines()[-1]
        check("error log includes stage, run, avail_phys_mib",
              rc == 2 and "stage=load" in err_line and "run=424243" in err_line and "avail_phys_mib=" in err_line,
              err_line)

        # 8. Corrupted update.zvr must fail cleanly rather than treating version as 0.0.0
        upd = os.path.join(jdir, "update.zvr")
        open(upd, "wb").write(b"bad")
        rc, _ = run_cmd([KMDEC_GUI, upd], {"KMDEC_TEST_NOBOX": "1"})
        utxt = open(os.path.join(jdir, "update.txt"), encoding="utf-8").read().strip()
        check("corrupt update.zvr reports error instead of 0.0.0", rc == 2 and utxt.startswith("error 0 -"), utxt)

        # 9. Installer PowerShell syntax & -ListOnly discovery check
        rc, out = run_cmd(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                           os.path.join(ROOT, "src", "installer", "install.ps1"), "-ListOnly"])
        check("install.ps1 -ListOnly runs cleanly", rc == 0, out.strip())

        # 10. Mask16 protection & preservation (left half masked=0, right half unmasked=65535)
        mask_in = os.path.join(work, "mask_in.GoZ")
        mask_out = os.path.join(work, "mask_out.GoZ")
        mask_off = os.path.join(work, "mask_off.GoZ")
        write_quad_goz(mask_in, 160, 160, with_mask=True)  # 51,200 tris
        rc, out = run_cmd([KMDEC, mask_in, mask_out, "--percent", "20", "--color", "0", "--coloredge", "0", "--mask", "10"])
        g_on = parse_goz_chunks(mask_out)
        left_on = sum(1 for a, b, c in g_on["tris"] if (g_on["verts"][a][0] + g_on["verts"][b][0] + g_on["verts"][c][0]) / 3 < 0.48)
        right_on = sum(1 for a, b, c in g_on["tris"] if (g_on["verts"][a][0] + g_on["verts"][b][0] + g_on["verts"][c][0]) / 3 > 0.52)
        has_masked_v = any(m < 1000 for m in g_on["masks"]) and any(m > 64000 for m in g_on["masks"])
        check("Mask16 protects masked region & preserves Mask16 chunk",
              rc == 0 and 30002 in g_on["tags"] and has_masked_v and left_on > right_on * 2.5,
              f"rc={rc} left={left_on} right={right_on} masks={len(g_on['masks'])}")

        rc2, _ = run_cmd([KMDEC, mask_in, mask_off, "--percent", "20", "--color", "0", "--coloredge", "0", "--mask", "0"])
        g_off = parse_goz_chunks(mask_off)
        check("Mask16 dropped when --mask 0", rc2 == 0 and 30002 not in g_off["tags"], f"rc={rc2} tags={g_off['tags']}")

        # 11. UV4 preservation, UV seam integrity & zero UV foldover (GoZ + OBJ)
        uv_in = os.path.join(work, "uv_in.GoZ")
        uv_out = os.path.join(work, "uv_out.GoZ")
        write_quad_goz(uv_in, 140, 140, with_uv_seam=True)  # 39,200 tris, two UV islands separated by gap (0.4, 0.6)
        rc, out = run_cmd([KMDEC, uv_in, uv_out, "--percent", "15", "--color", "0", "--coloredge", "0", "--uv", "10"])
        guv = parse_goz_chunks(uv_out)
        cross_seam = 0
        uv_flips = 0
        for rec in guv["uvs"]:
            us = (rec[0], rec[2], rec[4])
            vs = (rec[1], rec[3], rec[5])
            if not (max(us) <= 0.405 or min(us) >= 0.595):
                cross_seam += 1
            area = (us[1] - us[0]) * (vs[2] - vs[0]) - (vs[1] - vs[0]) * (us[2] - us[0])
            if area < -1e-9:
                uv_flips += 1
        check("GoZ UV4 preserved with zero seam bleeding and zero UV flips",
              rc == 0 and 25001 in guv["tags"] and len(guv["uvs"]) == len(guv["tris"]) > 3000 and cross_seam == 0 and uv_flips == 0,
              f"rc={rc} uvs={len(guv['uvs'])} tris={len(guv['tris'])} cross_seam={cross_seam} flips={uv_flips}")

        # OBJ UV round-trip (GoZ -> OBJ with vt -> OBJ decimated with --uv 10)
        uv_obj_mid = os.path.join(work, "uv_mid.obj")
        uv_obj_out = os.path.join(work, "uv_out.obj")
        rc, _ = run_cmd([KMDEC, uv_in, uv_obj_mid, "--percent", "80", "--color", "0", "--coloredge", "0", "--uv", "10"])
        rc2, _ = run_cmd([KMDEC, uv_obj_mid, uv_obj_out, "--percent", "25", "--color", "0", "--coloredge", "0", "--uv", "10"])
        obj_txt = open(uv_obj_out, encoding="utf-8").read()
        check("OBJ vt/f UV round-trip preserves UVs",
              rc == 0 and rc2 == 0 and "\nvt " in obj_txt and "/" in obj_txt,
              f"rc={rc},{rc2}")

        # 12. job.zvr 10-element handoff with keepMask=1 & keepUV=1
        muv_in = os.path.join(work, "muv_in.GoZ")
        write_quad_goz(muv_in, 100, 100, with_mask=True, with_uv_seam=True)
        shutil.copy(muv_in, os.path.join(jdir, "in.GoZ"))
        open(os.path.join(jdir, "job.zvr"), "wb").write(zvr([2, 20, 1, 0, 8, 108, 555555, 0, 1, 1]))
        rc, _ = run_cmd([KMDEC_GUI, os.path.join(jdir, "job.zvr")])
        g_job = parse_goz_chunks(os.path.join(jdir, "out.GoZ"))
        jlog = open(os.path.join(jdir, "log.txt"), encoding="utf-8").read().strip().splitlines()[-1]
        jnote = open(os.path.join(jdir, "note.txt"), encoding="utf-8").read()
        check("job.zvr keepMask=1 keepUV=1 preserves both chunks and logs mask=1 uv=1",
              rc == 0 and 25001 in g_job["tags"] and 30002 in g_job["tags"] and "mask=1 uv=1" in jlog and "保留了遮罩" in jnote and "保留了 UV" in jnote,
              f"rc={rc} tags={g_job['tags']} log={jlog} note={jnote}")

        open(os.path.join(jdir, "job.zvr"), "wb").write(zvr([2, 20, 1, 0, 8, 108, 555556, 0, 0, 0]))
        rc, _ = run_cmd([KMDEC_GUI, os.path.join(jdir, "job.zvr")])
        g_job0 = parse_goz_chunks(os.path.join(jdir, "out.GoZ"))
        jlog0 = open(os.path.join(jdir, "log.txt"), encoding="utf-8").read().strip().splitlines()[-1]
        check("job.zvr keepMask=0 keepUV=0 drops both chunks",
              rc == 0 and 25001 not in g_job0["tags"] and 30002 not in g_job0["tags"] and "mask=0 uv=0" in jlog0,
              f"rc={rc} tags={g_job0['tags']} log={jlog0}")

        # 13. lang_zh.tsv builds cleanly within fixed 64B / 160B record limits
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import package as pkg
        lang_out = os.path.join(work, "lang_zh.bin")
        pkg.build_lang(lang_out)
        check("lang_zh.tsv builds to lang_zh.bin without overflow",
              os.path.exists(lang_out) and os.path.getsize(lang_out) >= 66 * 256,
              f"size={os.path.getsize(lang_out) if os.path.exists(lang_out) else 0}")

    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f"\nRegression summary: {fails} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
