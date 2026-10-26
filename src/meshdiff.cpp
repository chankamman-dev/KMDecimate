// 量减面质量：两个网格之间的双向表面距离（按面积均匀撒点，找另一个网格上的最近点）。
// 用法：meshdiff 原模型 减面结果 [--samples N] [--heat 热力图.obj] [--heatmax 绝对距离]
// 输出均值 / RMS / 99 分位 / 最大值（Hausdorff），同时给出占原模型包围盒对角线的百分比。
#include "decimate.h"
#include <random>
#include <windows.h>

struct Grid {
    const Mesh* m = nullptr;
    double cell = 1;
    float lo[3];
    std::vector<uint64_t> keys;
    std::vector<uint32_t> start, items;

    static uint64_t key(int64_t x, int64_t y, int64_t z) { return uint64_t(x) << 42 | uint64_t(y) << 21 | uint64_t(z); }

    void build(const Mesh& mesh, int threads) {
        m = &mesh;
        size_t nt = mesh.nt();
        float hi[3] = {-1e30f, -1e30f, -1e30f};
        lo[0] = lo[1] = lo[2] = 1e30f;
        for (size_t i = 0; i < mesh.nv(); i++)
            for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], mesh.P[i * 3 + j]); hi[j] = std::max(hi[j], mesh.P[i * 3 + j]); }
        double area = 0;
        for (size_t t = 0; t < nt; t++) area += triArea(mesh, t);
        double ext = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
        cell = std::max(2 * std::sqrt(2 * area / std::max<size_t>(1, nt)), ext / 2000000.0 * 2);
        cell = std::max(cell, ext / 1.5e6);
        for (int j = 0; j < 3; j++) lo[j] -= float(cell);
        std::vector<std::pair<uint64_t, uint32_t>> pr;
        pr.reserve(nt * 3);
        for (size_t t = 0; t < nt; t++) {
            int64_t a[3], b[3];
            for (int j = 0; j < 3; j++) {
                float mn = 1e30f, mx = -1e30f;
                for (int k = 0; k < 3; k++) { float v = mesh.P[mesh.T[t * 3 + k] * 3 + j]; mn = std::min(mn, v); mx = std::max(mx, v); }
                a[j] = int64_t((mn - lo[j]) / cell); b[j] = int64_t((mx - lo[j]) / cell);
            }
            for (int64_t x = a[0]; x <= b[0]; x++)
                for (int64_t y = a[1]; y <= b[1]; y++)
                    for (int64_t z = a[2]; z <= b[2]; z++) pr.push_back({key(x, y, z), uint32_t(t)});
        }
        std::sort(pr.begin(), pr.end());
        items.resize(pr.size());
        for (size_t i = 0; i < pr.size(); i++) {
            if (i == 0 || pr[i].first != pr[i - 1].first) { keys.push_back(pr[i].first); start.push_back(uint32_t(i)); }
            items[i] = pr[i].second;
        }
        start.push_back(uint32_t(pr.size()));
        (void)threads;
    }

    static double triArea(const Mesh& m, size_t t) {
        const float* a = &m.P[m.T[t * 3] * 3];
        const float* b = &m.P[m.T[t * 3 + 1] * 3];
        const float* c = &m.P[m.T[t * 3 + 2] * 3];
        double e1[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
        double e2[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
        double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        return 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    }

    // Ericson《Real-Time Collision Detection》5.1.5
    static double distTri2(const double p[3], const float* A, const float* B, const float* C) {
        double a[3] = {A[0], A[1], A[2]}, b[3] = {B[0], B[1], B[2]}, c[3] = {C[0], C[1], C[2]};
        auto sub = [](const double* x, const double* y, double* r) { r[0] = x[0] - y[0]; r[1] = x[1] - y[1]; r[2] = x[2] - y[2]; };
        auto dot = [](const double* x, const double* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
        double ab[3], ac[3], ap[3], bp[3], cp[3], q[3];
        sub(b, a, ab); sub(c, a, ac); sub(p, a, ap);
        auto res = [&](double u, double v, double w) {
            for (int j = 0; j < 3; j++) q[j] = a[j] * u + b[j] * v + c[j] * w;
            double d[3]; sub(p, q, d); return dot(d, d);
        };
        double d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0 && d2 <= 0) return res(1, 0, 0);
        sub(p, b, bp);
        double d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0 && d4 <= d3) return res(0, 1, 0);
        double vc = d1 * d4 - d3 * d2;
        if (vc <= 0 && d1 >= 0 && d3 <= 0) { double v = d1 / (d1 - d3); return res(1 - v, v, 0); }
        sub(p, c, cp);
        double d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0 && d5 <= d6) return res(0, 0, 1);
        double vb = d5 * d2 - d1 * d6;
        if (vb <= 0 && d2 >= 0 && d6 <= 0) { double w = d2 / (d2 - d6); return res(1 - w, 0, w); }
        double va = d3 * d6 - d5 * d4;
        if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { double w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); return res(0, 1 - w, w); }
        double den = 1 / (va + vb + vc), v = vb * den, w = vc * den;
        return res(1 - v - w, v, w);
    }

    double nearest(const double p[3]) const {
        int64_t c[3];
        for (int j = 0; j < 3; j++) c[j] = int64_t((p[j] - lo[j]) / cell);
        double best = 1e300;
        for (int64_t r = 0; r < 4096; r++) {
            for (int64_t x = c[0] - r; x <= c[0] + r; x++)
                for (int64_t y = c[1] - r; y <= c[1] + r; y++)
                    for (int64_t z = c[2] - r; z <= c[2] + r; z++) {
                        if (std::max({std::llabs(x - c[0]), std::llabs(y - c[1]), std::llabs(z - c[2])}) != r) continue;
                        if (x < 0 || y < 0 || z < 0 || x >= (1 << 21) || y >= (1 << 21) || z >= (1 << 21)) continue;
                        auto it = std::lower_bound(keys.begin(), keys.end(), key(x, y, z));
                        if (it == keys.end() || *it != key(x, y, z)) continue;
                        size_t k = it - keys.begin();
                        for (uint32_t i = start[k]; i < start[k + 1]; i++) {
                            uint32_t t = items[i];
                            double d = distTri2(p, &m->P[m->T[t * 3] * 3], &m->P[m->T[t * 3 + 1] * 3], &m->P[m->T[t * 3 + 2] * 3]);
                            best = std::min(best, d);
                        }
                    }
            if (best < 1e300 && std::sqrt(best) <= r * cell) break;
        }
        return std::sqrt(best);
    }
};

struct Stat { double mean, rms, p99, max; };

static Stat sampleDist(const Mesh& a, const Grid& gb, size_t n, int threads) {
    std::vector<double> cdf(a.nt());
    double acc = 0;
    for (size_t t = 0; t < a.nt(); t++) { acc += Grid::triArea(a, t); cdf[t] = acc; }
    std::vector<double> d(n);
    kmd::parallelFor(n, threads, 2048, [&](size_t i) {
        std::mt19937_64 rng(i * 0x9E3779B97F4A7C15ull + 1);
        std::uniform_real_distribution<double> U(0, 1);
        double r = (i + U(rng)) / n * acc;
        size_t t = std::min(a.nt() - 1, size_t(std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin()));
        double u = U(rng), v = U(rng);
        if (u + v > 1) { u = 1 - u; v = 1 - v; }
        const float* A = &a.P[a.T[t * 3] * 3];
        const float* B = &a.P[a.T[t * 3 + 1] * 3];
        const float* C = &a.P[a.T[t * 3 + 2] * 3];
        double p[3];
        for (int j = 0; j < 3; j++) p[j] = A[j] + u * (B[j] - A[j]) + v * (C[j] - A[j]);
        d[i] = gb.nearest(p);
    });
    Stat s{};
    double sq = 0;
    for (double x : d) { s.mean += x; sq += x * x; s.max = std::max(s.max, x); }
    s.mean /= n;
    s.rms = std::sqrt(sq / n);
    std::nth_element(d.begin(), d.begin() + size_t(n * 0.99), d.end());
    s.p99 = d[size_t(n * 0.99)];
    return s;
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) { printf("用法：meshdiff 原模型 减面结果 [--samples N] [--heat 热力图.obj] [--heatmax 距离]\n"); return 1; }
    size_t samples = 1000000;
    std::wstring heat;
    double heatMax = 0;
    for (int i = 3; i + 1 < argc; i += 2) {
        std::wstring k = argv[i];
        if (k == L"--samples") samples = size_t(_wtof(argv[i + 1]));
        else if (k == L"--heat") heat = argv[i + 1];
        else if (k == L"--heatmax") heatMax = _wtof(argv[i + 1]);
    }
    int threads = int(std::thread::hardware_concurrency());
    try {
        Mesh a = loadMesh(argv[1]), b = loadMesh(argv[2]);
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < a.nv(); i++)
            for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], a.P[i * 3 + j]); hi[j] = std::max(hi[j], a.P[i * 3 + j]); }
        double diag = std::sqrt(double(hi[0] - lo[0]) * (hi[0] - lo[0]) + double(hi[1] - lo[1]) * (hi[1] - lo[1]) + double(hi[2] - lo[2]) * (hi[2] - lo[2]));
        auto t0 = kmd::Clock::now();
        Grid ga, gb;
        ga.build(a, threads);
        gb.build(b, threads);
        Stat ab = sampleDist(b, ga, samples, threads);  // 结果表面 → 原模型
        Stat ba = sampleDist(a, gb, samples, threads);  // 原模型表面 → 结果
        printf("原模型 %zu 三角形，结果 %zu 三角形，对角线 %.6g，%zu 个采样点，用时 %.2f 秒\n", a.nt(), b.nt(), diag, samples, kmd::secs(t0));
        auto pr = [&](const char* name, const Stat& s) {
            printf("%-18s 均值 %.4e (%.5f%%)  RMS %.4e (%.5f%%)  99%% %.4e (%.5f%%)  最大 %.4e (%.4f%%)\n", name, s.mean, s.mean / diag * 100,
                   s.rms, s.rms / diag * 100, s.p99, s.p99 / diag * 100, s.max, s.max / diag * 100);
        };
        pr("结果→原模型", ab);
        pr("原模型→结果", ba);
        printf("DIFF diag=%.6g mean=%.6e rms=%.6e p99=%.6e hausdorff=%.6e\n", diag, std::max(ab.mean, ba.mean), std::max(ab.rms, ba.rms),
               std::max(ab.p99, ba.p99), std::max(ab.max, ba.max));
        if (!heat.empty()) {  // 结果网格按"离原模型多远"上色：蓝 → 绿 → 红
            std::vector<double> vd(b.nv());
            kmd::parallelFor(b.nv(), threads, 1024, [&](size_t i) {
                double p[3] = {b.P[i * 3], b.P[i * 3 + 1], b.P[i * 3 + 2]};
                vd[i] = ga.nearest(p);
            });
            double mx = heatMax > 0 ? heatMax : ab.p99 * 1.5;
            b.C.resize(b.nv());
            for (size_t i = 0; i < b.nv(); i++) {
                double s = std::clamp(vd[i] / mx, 0.0, 1.0);
                double r = std::clamp(2 * s - 1, 0.0, 1.0), g = 1 - std::fabs(2 * s - 1), bl = std::clamp(1 - 2 * s, 0.0, 1.0);
                b.C[i] = uint32_t(r * 255) << 16 | uint32_t(g * 255) << 8 | uint32_t(bl * 255);
            }
            saveMesh(heat, b);
            printf("热力图 %s（红 = %.4e 及以上）\n", toUtf8(heat).c_str(), mx);
        }
    } catch (std::exception& e) {
        printf("出错：%s\n", e.what());
        return 2;
    }
    return 0;
}
