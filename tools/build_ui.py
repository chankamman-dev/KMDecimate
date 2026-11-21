# Build custom ZBrush UI button PSD icons and preview PNGs for KM Decimate.
# Zero external dependencies: uses Win32 GDI (Microsoft YaHei Bold) with 4x supersampling
# to render crisp anti-aliased rounded buttons, vector icons, and centered typography.

from __future__ import annotations
import ctypes
import math
import pathlib
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
UI_DIR = ROOT / "src" / "zbrush" / "KMDecimate"

gdi32 = ctypes.windll.gdi32
user32 = ctypes.windll.user32


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", ctypes.c_uint32),
        ("biWidth", ctypes.c_int32),
        ("biHeight", ctypes.c_int32),
        ("biPlanes", ctypes.c_uint16),
        ("biBitCount", ctypes.c_uint16),
        ("biCompression", ctypes.c_uint32),
        ("biSizeImage", ctypes.c_uint32),
        ("biXPelsPerMeter", ctypes.c_int32),
        ("biYPelsPerMeter", ctypes.c_int32),
        ("biClrUsed", ctypes.c_uint32),
        ("biClrImportant", ctypes.c_uint32),
    ]


class SIZE(ctypes.Structure):
    _fields_ = [("cx", ctypes.c_long), ("cy", ctypes.c_long)]


def render_text_mask(text: str, font_px: int, weight: int = 700, font_name: str = "Microsoft YaHei UI") -> tuple[int, int, bytes]:
    """Render UTF-16 text using Win32 GDI at high resolution and return (w, h, grayscale_alpha_bytes)."""
    hdc = gdi32.CreateCompatibleDC(0)
    # Create font (negative height = character height, ANTIALIASED_QUALITY = 4)
    hfont = gdi32.CreateFontW(
        -font_px, 0, 0, 0, weight, 0, 0, 0, 1, 0, 0, 4, 0, font_name
    )
    old_font = gdi32.SelectObject(hdc, hfont)
    sz = SIZE()
    gdi32.GetTextExtentPoint32W(hdc, text, len(text), ctypes.byref(sz))
    tw = max(1, sz.cx + font_px // 2)
    th = max(1, sz.cy + font_px // 4)

    bmi = BITMAPINFOHEADER()
    bmi.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.biWidth = tw
    bmi.biHeight = -th  # top-down DIB
    bmi.biPlanes = 1
    bmi.biBitCount = 32
    bmi.biCompression = 0

    pv = ctypes.c_void_p()
    hbmp = gdi32.CreateDIBSection(hdc, ctypes.byref(bmi), 0, ctypes.byref(pv), 0, 0)
    old_bmp = gdi32.SelectObject(hdc, hbmp)

    # Clear to black, draw white text
    ctypes.memset(pv, 0, tw * th * 4)
    gdi32.SetBkMode(hdc, 2)  # OPAQUE
    gdi32.SetBkColor(hdc, 0x000000)
    gdi32.SetTextColor(hdc, 0xFFFFFF)
    pad_x = font_px // 4
    pad_y = font_px // 8
    gdi32.TextOutW(hdc, pad_x, pad_y, text, len(text))

    raw = ctypes.string_at(pv, tw * th * 4)
    gdi32.SelectObject(hdc, old_bmp)
    gdi32.DeleteObject(hbmp)
    gdi32.SelectObject(hdc, old_font)
    gdi32.DeleteObject(hfont)
    gdi32.DeleteDC(hdc)

    # Extract tight horizontal bounding box and luminance
    alpha = bytearray(tw * th)
    min_x, max_x = tw, 0
    min_y, max_y = th, 0
    for y in range(th):
        row_off = y * tw * 4
        for x in range(tw):
            off = row_off + x * 4
            # Average R,G,B in case ClearType was used
            lum = (raw[off] + raw[off + 1] + raw[off + 2]) // 3
            if lum > 0:
                alpha[y * tw + x] = lum
                if x < min_x:
                    min_x = x
                if x > max_x:
                    max_x = x
                if y < min_y:
                    min_y = y
                if y > max_y:
                    max_y = y

    if max_x < min_x:
        return 1, 1, b"\x00"

    cw = max_x - min_x + 1
    ch = max_y - min_y + 1
    cropped = bytearray(cw * ch)
    for y in range(ch):
        cropped[y * cw : (y + 1) * cw] = alpha[(min_y + y) * tw + min_x : (min_y + y) * tw + min_x + cw]
    return cw, ch, bytes(cropped)


def sd_rounded_box(px: float, py: float, cx: float, cy: float, hw: float, hh: float, r: float) -> float:
    dx = abs(px - cx) - (hw - r)
    dy = abs(py - cy) - (hh - r)
    outside = math.hypot(max(dx, 0.0), max(dy, 0.0))
    inside = min(max(dx, dy), 0.0)
    return outside + inside - r


def sd_segment(px: float, py: float, ax: float, ay: float, bx: float, by: float) -> float:
    pax, pay = px - ax, py - ay
    bax, bay = bx - ax, by - ay
    denom = bax * bax + bay * bay
    h = 0.0 if denom == 0.0 else max(0.0, min(1.0, (pax * bax + pay * bay) / denom))
    return math.hypot(pax - bax * h, pay - bay * h)


def draw_icon_mesh(px: float, py: float, cx: float, cy: float, rad: float) -> float:
    """Crisp diamond/triangle mesh icon."""
    top = (cx, cy - rad)
    bot = (cx, cy + rad)
    left = (cx - rad * 1.05, cy)
    right = (cx + rad * 1.05, cy)
    mid_l = (cx - rad * 0.42, cy + rad * 0.15)
    mid_r = (cx + rad * 0.42, cy - rad * 0.15)
    segs = [
        (top, left), (left, bot), (bot, right), (right, top),
        (top, mid_l), (mid_l, bot), (top, mid_r), (mid_r, bot),
        (left, mid_l), (mid_l, mid_r), (mid_r, right),
    ]
    d = min(sd_segment(px, py, a[0], a[1], b[0], b[1]) for a, b in segs)
    return max(0.0, min(1.0, 1.35 - d))


def draw_icon_remesh(px: float, py: float, cx: float, cy: float, rad: float) -> float:
    """Crisp quad-grid / remesh projection icon."""
    r = rad * 0.92
    tl, tr = (cx - r, cy - r * 0.85), (cx + r, cy - r * 0.85)
    bl, br = (cx - r, cy + r * 0.85), (cx + r, cy + r * 0.85)
    ml, mr = (cx - r, cy), (cx + r, cy)
    mt, mb = (cx, cy - r * 0.85), (cx, cy + r * 0.85)
    segs = [
        (tl, tr), (tr, br), (br, bl), (bl, tl),
        (ml, mr), (mt, mb),
    ]
    d = min(sd_segment(px, py, a[0], a[1], b[0], b[1]) for a, b in segs)
    return max(0.0, min(1.0, 1.35 - d))


def make_button_rgb(
    w: int,
    h: int,
    text: str,
    top_rgb: tuple[int, int, int],
    bot_rgb: tuple[int, int, int],
    border_rgb: tuple[int, int, int],
    hi_rgb: tuple[int, int, int],
    text_rgb: tuple[int, int, int] = (255, 255, 255),
    font_px: int = 15,
    radius: float = 5.0,
    icon_kind: str = "",
) -> tuple[bytes, bytes, bytes]:
    """Render an anti-aliased button (w x h) and return (R_plane, G_plane, B_plane)."""
    # Render text at 4x resolution and downsample to 1x for razor-sharp anti-aliasing
    ss = 4
    tw4, th4, mask4 = render_text_mask(text, font_px * ss, weight=700)
    tw = (tw4 + ss - 1) // ss
    th = (th4 + ss - 1) // ss
    tmask = bytearray(tw * th)
    for y in range(th):
        for x in range(tw):
            acc = 0
            cnt = 0
            for dy in range(ss):
                yy = y * ss + dy
                if yy >= th4:
                    continue
                for dx in range(ss):
                    xx = x * ss + dx
                    if xx >= tw4:
                        continue
                    acc += mask4[yy * tw4 + xx]
                    cnt += 1
            # Slight gamma boost so white/light text on dark or orange backgrounds looks crisp and bold
            val = (acc / max(1, cnt)) / 255.0
            val = min(1.0, (val ** 0.78) * 1.08)
            tmask[y * tw + x] = int(val * 255.0 + 0.5)

    icon_rad = max(6.5, h * 0.21)
    icon_gap = int(h * 0.25) if icon_kind else 0
    icon_w = int(icon_rad * 2.2) if icon_kind else 0
    total_w = icon_w + icon_gap + tw
    start_x = (w - total_w) // 2
    icon_cx = start_x + icon_w * 0.5
    icon_cy = h * 0.5
    tx0 = start_x + icon_w + icon_gap
    ty0 = (h - th) // 2

    # ZBrush panel dark surround color (matches ZBrush subpalette background ~ rgb(43,43,43))
    bg_outer = (43, 43, 43)

    r_plane = bytearray(w * h)
    g_plane = bytearray(w * h)
    b_plane = bytearray(w * h)

    cx, cy = w * 0.5, h * 0.5
    hw, hh = w * 0.5 - 1.0, h * 0.5 - 1.0

    for y in range(h):
        t = y / max(1, h - 1)
        # Smooth vertical gradient with subtle top specular sheen
        gr = top_rgb[0] * (1.0 - t) + bot_rgb[0] * t
        gg = top_rgb[1] * (1.0 - t) + bot_rgb[1] * t
        gb = top_rgb[2] * (1.0 - t) + bot_rgb[2] * t
        if t < 0.45:
            sheen = (0.45 - t) * 0.14
            gr = min(255.0, gr + (255.0 - gr) * sheen)
            gg = min(255.0, gg + (255.0 - gg) * sheen)
            gb = min(255.0, gb + (255.0 - gb) * sheen)

        for x in range(w):
            px, py = x + 0.5, y + 0.5
            d = sd_rounded_box(px, py, cx, cy, hw, hh, radius)

            # Outer coverage
            cov = max(0.0, min(1.0, 0.5 - d))
            if cov <= 0.0:
                idx = y * w + x
                r_plane[idx], g_plane[idx], b_plane[idx] = bg_outer
                continue

            # Inner fill vs 1.2px border
            r, g, b = gr, gg, gb

            # Top inner highlight line
            if -2.2 <= d <= -0.8 and y < 4:
                hi_a = max(0.0, 1.0 - abs(d + 1.5)) * (1.0 - y / 4.0) * 0.55
                r = r * (1.0 - hi_a) + hi_rgb[0] * hi_a
                g = g * (1.0 - hi_a) + hi_rgb[1] * hi_a
                b = b * (1.0 - hi_a) + hi_rgb[2] * hi_a

            # 1px crisp border
            border_a = max(0.0, min(1.0, 1.1 - abs(d + 0.4)))
            if border_a > 0.0:
                r = r * (1.0 - border_a) + border_rgb[0] * border_a
                g = g * (1.0 - border_a) + border_rgb[1] * border_a
                b = b * (1.0 - border_a) + border_rgb[2] * border_a

            # Subtle vector icon + drop shadow
            if icon_kind and abs(px - icon_cx) <= icon_rad + 4 and abs(py - icon_cy) <= icon_rad + 4:
                fn = draw_icon_mesh if icon_kind == "mesh" else draw_icon_remesh
                ia_sh = fn(px, py - 1.0, icon_cx, icon_cy, icon_rad) * 0.35
                if ia_sh > 0.0:
                    r *= 1.0 - ia_sh
                    g *= 1.0 - ia_sh
                    b *= 1.0 - ia_sh
                ia = fn(px, py, icon_cx, icon_cy, icon_rad) * 0.95
                if ia > 0.0:
                    r = r * (1.0 - ia) + text_rgb[0] * ia
                    g = g * (1.0 - ia) + text_rgb[1] * ia
                    b = b * (1.0 - ia) + text_rgb[2] * ia

            # Text drop shadow (1px down)
            tx = x - tx0
            tys = y - (ty0 + 1)
            if 0 <= tx < tw and 0 <= tys < th:
                sa = (tmask[tys * tw + tx] / 255.0) * 0.42
                if sa > 0.0:
                    r *= 1.0 - sa
                    g *= 1.0 - sa
                    b *= 1.0 - sa

            # Text foreground
            ty = y - ty0
            if 0 <= tx < tw and 0 <= ty < th:
                ta = tmask[ty * tw + tx] / 255.0
                if ta > 0.0:
                    r = r * (1.0 - ta) + text_rgb[0] * ta
                    g = g * (1.0 - ta) + text_rgb[1] * ta
                    b = b * (1.0 - ta) + text_rgb[2] * ta

            # Blend with outer surround
            fr = int(bg_outer[0] * (1.0 - cov) + r * cov + 0.5)
            fg = int(bg_outer[1] * (1.0 - cov) + g * cov + 0.5)
            fb = int(bg_outer[2] * (1.0 - cov) + b * cov + 0.5)
            idx = y * w + x
            r_plane[idx] = max(0, min(255, fr))
            g_plane[idx] = max(0, min(255, fg))
            b_plane[idx] = max(0, min(255, fb))

    return bytes(r_plane), bytes(g_plane), bytes(b_plane)


def packbits_encode_row(row: bytes) -> bytes:
    """PackBits RLE encoder for a single scanline (matches Adobe PSD compression=1)."""
    out = bytearray()
    n = len(row)
    i = 0
    while i < n:
        # Check for a run of identical bytes
        run_end = i + 1
        while run_end < n and run_end - i < 128 and row[run_end] == row[i]:
            run_end += 1
        if run_end - i >= 3:
            cnt = run_end - i
            out.append((257 - cnt) & 0xFF)
            out.append(row[i])
            i = run_end
        else:
            lit_start = i
            while i < n and i - lit_start < 128:
                if i + 2 < n and row[i] == row[i + 1] == row[i + 2]:
                    break
                i += 1
            cnt = i - lit_start
            out.append(cnt - 1)
            out.extend(row[lit_start:i])
    return bytes(out)


def save_psd(path: pathlib.Path, w: int, h: int, planes: tuple[bytes, bytes, bytes]) -> None:
    """Write a standard 8-bit 3-channel RGB PSD with RLE compression (exact format used by ZBrush plugins)."""
    header = struct.pack(">4sH6sHIIHH", b"8BPS", 1, b"\x00" * 6, 3, h, w, 8, 3)
    color_mode_section = struct.pack(">I", 0)
    image_resources_section = struct.pack(">I", 0)
    layer_mask_section = struct.pack(">I", 0)

    rle_rows = []
    byte_counts = bytearray()
    for plane in planes:
        for y in range(h):
            enc = packbits_encode_row(plane[y * w : (y + 1) * w])
            rle_rows.append(enc)
            byte_counts.extend(struct.pack(">H", len(enc)))

    image_data = struct.pack(">H", 1) + bytes(byte_counts) + b"".join(rle_rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + color_mode_section + image_resources_section + layer_mask_section + image_data)


def save_png(path: pathlib.Path, w: int, h: int, planes: tuple[bytes, bytes, bytes]) -> None:
    raw = bytearray()
    r, g, b = planes
    for y in range(h):
        raw.append(0)
        off = y * w
        for x in range(w):
            i = off + x
            raw.extend((r[i], g[i], b[i]))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def build_all(out_dir: pathlib.Path = UI_DIR, preview_dir: pathlib.Path | None = None) -> list[pathlib.Path]:
    specs = [
        # Primary Decimate button: 280x48 native screen pixel resolution (matches 160x32 aspect ratio)
        dict(
            name="btn_decimate.psd",
            w=280,
            h=48,
            text="减  面",
            top_rgb=(249, 132, 36),
            bot_rgb=(220, 88, 12),
            border_rgb=(255, 174, 96),
            hi_rgb=(255, 220, 180),
            text_rgb=(255, 255, 255),
            font_px=21,
            radius=6.0,
            icon_kind="mesh",
        ),
        # Secondary ZRemesh Project button: 280x48 vibrant cyan/blue matching GitHub SVG
        dict(
            name="btn_zremesh.psd",
            w=280,
            h=48,
            text="重拓扑并投射",
            top_rgb=(22, 148, 232),
            bot_rgb=(2, 108, 188),
            border_rgb=(86, 190, 252),
            hi_rgb=(180, 230, 255),
            text_rgb=(255, 255, 255),
            font_px=20,
            radius=6.0,
            icon_kind="remesh",
        ),
        # Half-width batch buttons: 138x26 native screen pixel resolution for razor-sharp CJK text
        dict(
            name="btn_batch_vis.psd",
            w=138,
            h=26,
            text="批量减面(可见)",
            top_rgb=(78, 82, 90),
            bot_rgb=(54, 57, 64),
            border_rgb=(118, 124, 136),
            hi_rgb=(170, 176, 188),
            text_rgb=(245, 247, 250),
            font_px=13,
            radius=4.0,
            icon_kind="",
        ),
        dict(
            name="btn_batch_all.psd",
            w=138,
            h=26,
            text="批量减面(全部)",
            top_rgb=(78, 82, 90),
            bot_rgb=(54, 57, 64),
            border_rgb=(118, 124, 136),
            hi_rgb=(170, 176, 188),
            text_rgb=(245, 247, 250),
            font_px=13,
            radius=4.0,
            icon_kind="",
        ),
        # Footer buttons: 138x24
        dict(
            name="btn_about.psd",
            w=138,
            h=24,
            text="关  于",
            top_rgb=(66, 69, 76),
            bot_rgb=(48, 50, 56),
            border_rgb=(98, 102, 112),
            hi_rgb=(145, 150, 160),
            text_rgb=(225, 229, 236),
            font_px=13,
            radius=4.0,
            icon_kind="",
        ),
        dict(
            name="btn_update.psd",
            w=138,
            h=24,
            text="检查更新",
            top_rgb=(66, 69, 76),
            bot_rgb=(48, 50, 56),
            border_rgb=(98, 102, 112),
            hi_rgb=(145, 150, 160),
            text_rgb=(225, 229, 236),
            font_px=13,
            radius=4.0,
            icon_kind="",
        ),
    ]

    written = []
    for sp in specs:
        planes = make_button_rgb(
            w=sp["w"],
            h=sp["h"],
            text=sp["text"],
            top_rgb=sp["top_rgb"],
            bot_rgb=sp["bot_rgb"],
            border_rgb=sp["border_rgb"],
            hi_rgb=sp["hi_rgb"],
            text_rgb=sp["text_rgb"],
            font_px=sp["font_px"],
            radius=sp["radius"],
            icon_kind=sp["icon_kind"],
        )
        psd_path = out_dir / sp["name"]
        save_psd(psd_path, sp["w"], sp["h"], planes)
        written.append(psd_path)
        if preview_dir is not None:
            png_path = preview_dir / (pathlib.Path(sp["name"]).stem + ".png")
            save_png(png_path, sp["w"], sp["h"], planes)
    return written


if __name__ == "__main__":
    prev = pathlib.Path(r"C:\Users\61914\.gemini\antigravity\brain\4e85cb59-5bf9-45af-bfc3-fabc0edebda7\scratch\ui_preview")
    files = build_all(UI_DIR, prev)
    for f in files:
        print("Generated:", f, f.stat().st_size, "bytes")
