// 把带 Polypaint 的网格正交渲染成 PNG，用来肉眼比较减面前后的颜色（比如眼睛）。
// 用法：meshshot 模型 输出.png [--view +z] [--crop 中心x 中心y 宽] [--size 800] [--wire 1] [--flipy 1] [--focus x y z 宽]
//   view：从哪个方向看（+z = 从 +Z 往 -Z 看，屏幕右 = +X，上 = +Y；也可以 -z / +x / -x / +y / -y）
//   crop：在投影包围盒里取一块（0–1 归一，中心和宽度），默认整个模型；几个模型用同样参数就能对齐比较
//   wire：叠加三角形边（看颜色边界附近留了多少面）
//   flipy：上下翻转（ZBrush 的 GoZ 里 Y 朝下，从 -z 看是倒着的）
#include "meshio.h"
#include <cmath>
#include <windows.h>

static uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xffffffffu) {
    static uint32_t tab[256];
    if (!tab[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t x = i;
            for (int k = 0; k < 8; k++) x = x & 1 ? 0xedb88320u ^ (x >> 1) : x >> 1;
            tab[i] = x;
        }
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}

// 不压缩的 PNG（zlib 存储块），够用且不依赖库
static void writePng(const std::wstring& path, int w, int h, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t i = 0; i < raw.size(); i += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size());
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    }
    uint32_t ad = b << 16 | a;
    for (int k = 3; k >= 0; k--) z.push_back(uint8_t(ad >> (k * 8)));
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
        for (int k = 3; k >= 0; k--) out.push_back(uint8_t(d.size() >> (k * 8)));
        size_t s = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), d.begin(), d.end());
        uint32_t c = ~crc32(&out[s], out.size() - s);
        for (int k = 3; k >= 0; k--) out.push_back(uint8_t(c >> (k * 8)));
    };
    std::vector<uint8_t> ih;
    for (uint32_t v : {uint32_t(w), uint32_t(h)})
        for (int k = 3; k >= 0; k--) ih.push_back(uint8_t(v >> (k * 8)));
    ih.insert(ih.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ih);
    chunk("IDAT", z);
    chunk("IEND", {});
    writeAll(path, std::string(out.begin(), out.end()));
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) {
        printf("用法：meshshot 模型 输出.png [--view +z] [--crop 中心x 中心y 宽] [--size 800] [--wire 1]\n");
        return 1;
    }
    std::wstring view = L"+z";
    double cx = 0.5, cy = 0.5, cw = 1;
    int size = 800, wire = 0, flipy = 0;
    double focus[4] = {0, 0, 0, 0};  // --focus x y z 宽：以模型坐标里的一点为中心，取这么宽（比如 meshdiff 的 MAXAT）
    for (int i = 3; i < argc; i++) {
        std::wstring k = argv[i];
        if (k == L"--view" && i + 1 < argc) view = argv[++i];
        else if (k == L"--crop" && i + 3 < argc) { cx = _wtof(argv[i + 1]); cy = _wtof(argv[i + 2]); cw = _wtof(argv[i + 3]); i += 3; }
        else if (k == L"--size" && i + 1 < argc) size = _wtoi(argv[++i]);
        else if (k == L"--wire" && i + 1 < argc) wire = _wtoi(argv[++i]);
        else if (k == L"--flipy" && i + 1 < argc) flipy = _wtoi(argv[++i]);
        else if (k == L"--focus" && i + 4 < argc) {
            for (int j = 0; j < 4; j++) focus[j] = _wtof(argv[i + 1 + j]);
            i += 4;
        }
    }
    try {
        Mesh m = loadMesh(argv[1]);
        // 屏幕坐标 (sx, sy, 深度 sz，越大越近)
        int ax = view[1] == L'x' ? 0 : view[1] == L'y' ? 1 : 2;
        double sg = view[0] == L'-' ? -1 : 1;
        int ux = ax == 0 ? 2 : 0, uy = ax == 1 ? 2 : 1;  // 屏幕右、上对应的轴
        double us = ax == 0 ? -sg : sg;                   // 从 +x 看时右边是 -z
        if (ax == 1) us = 1;
        size_t nv = m.nv();
        std::vector<double> S(nv * 3);
        double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
        for (size_t i = 0; i < nv; i++) {
            const float* p = &m.P[i * 3];
            double sx = us * p[ux], sy = (ax == 1 ? -sg : 1) * (flipy ? -1 : 1) * p[uy], sz = sg * p[ax];
            S[i * 3] = sx; S[i * 3 + 1] = sy; S[i * 3 + 2] = sz;
            lo[0] = std::min(lo[0], sx); hi[0] = std::max(hi[0], sx);
            lo[1] = std::min(lo[1], sy); hi[1] = std::max(hi[1], sy);
        }
        double ext = std::max(hi[0] - lo[0], hi[1] - lo[1]);
        double vw = ext * cw, x0 = lo[0] + cx * (hi[0] - lo[0]) - vw / 2, y0 = lo[1] + cy * (hi[1] - lo[1]) - vw / 2;
        if (focus[3] > 0) {
            vw = focus[3];
            x0 = us * focus[ux] - vw / 2;
            y0 = (ax == 1 ? -sg : 1) * (flipy ? -1 : 1) * focus[uy] - vw / 2;
        }
        double sc = size / vw;
        int W = size, H = size;
        std::vector<float> zb(size_t(W) * H, -1e30f);
        std::vector<uint8_t> img(size_t(W) * H * 3, 160);
        auto col = [&](uint32_t v, int j) { return m.C.empty() ? 200.0 : double(m.C[v] >> (16 - 8 * j) & 255); };
        for (size_t t = 0; t < m.nt(); t++) {
            uint32_t vi[3] = {m.T[t * 3], m.T[t * 3 + 1], m.T[t * 3 + 2]};
            double X[3], Y[3], Z[3];
            for (int k = 0; k < 3; k++) {
                X[k] = (S[vi[k] * 3] - x0) * sc;
                Y[k] = H - (S[vi[k] * 3 + 1] - y0) * sc;
                Z[k] = S[vi[k] * 3 + 2];
            }
            double area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
            if (area == 0) continue;
            // 简单光照：法线和视线夹角（双面）
            const float* a = &m.P[vi[0] * 3]; const float* b = &m.P[vi[1] * 3]; const float* c = &m.P[vi[2] * 3];
            double e1[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
            double e2[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
            double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            double shade = nl > 0 ? 0.55 + 0.45 * std::fabs(n[ax]) / nl : 1;
            int bx0 = std::max(0, int(std::floor(std::min({X[0], X[1], X[2]})))), bx1 = std::min(W - 1, int(std::ceil(std::max({X[0], X[1], X[2]}))));
            int by0 = std::max(0, int(std::floor(std::min({Y[0], Y[1], Y[2]})))), by1 = std::min(H - 1, int(std::ceil(std::max({Y[0], Y[1], Y[2]}))));
            for (int py = by0; py <= by1; py++)
                for (int px = bx0; px <= bx1; px++) {
                    double qx = px + 0.5, qy = py + 0.5;
                    double w0 = ((X[1] - qx) * (Y[2] - qy) - (X[2] - qx) * (Y[1] - qy)) / area;
                    double w1 = ((X[2] - qx) * (Y[0] - qy) - (X[0] - qx) * (Y[2] - qy)) / area;
                    double w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float z = float(w0 * Z[0] + w1 * Z[1] + w2 * Z[2]);
                    size_t o = size_t(py) * W + px;
                    if (z <= zb[o]) continue;
                    zb[o] = z;
                    bool edge = wire && std::min({w0 * std::fabs(area) / std::hypot(X[1] - X[2], Y[1] - Y[2]),
                                                  w1 * std::fabs(area) / std::hypot(X[2] - X[0], Y[2] - Y[0]),
                                                  w2 * std::fabs(area) / std::hypot(X[0] - X[1], Y[0] - Y[1])}) < 0.6;
                    for (int j = 0; j < 3; j++) {
                        double v = (w0 * col(vi[0], j) + w1 * col(vi[1], j) + w2 * col(vi[2], j)) * shade;
                        img[o * 3 + j] = uint8_t(std::clamp(edge ? v * 0.55 : v, 0.0, 255.0));
                    }
                }
        }
        writePng(argv[2], W, H, img);
        printf("写出 %s（%d×%d，%zu 三角形）\n", toUtf8(argv[2]).c_str(), W, H, m.nt());
    } catch (std::exception& e) {
        printf("出错：%s\n", e.what());
        return 2;
    }
    return 0;
}
