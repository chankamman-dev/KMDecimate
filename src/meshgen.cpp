// 造大测试模型：中点细分 N 次（每次面数 ×4），沿法线加一点多频起伏，模拟雕刻细节。
// 用法：meshgen 输入 输出 [--levels 1] [--amp 0.3] [--paint 0]   amp 是起伏幅度（相对细分后平均边长）
// paint > 0 时画上边界很硬的 Polypaint（沿对角线约 paint 个斑点，带白圈和细红线，模拟眼睛、描线），测“保留顶点色”
#include "decimate.h"
#include <map>
#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) { printf("用法：meshgen 输入 输出 [--levels 1] [--amp 0.3] [--paint 0]\n"); return 1; }
    int levels = 1;
    double amp = 0.3, paint = 0;
    for (int i = 3; i + 1 < argc; i += 2) {
        std::wstring k = argv[i];
        if (k == L"--levels") levels = _wtoi(argv[i + 1]);
        else if (k == L"--amp") amp = _wtof(argv[i + 1]);
        else if (k == L"--paint") paint = _wtof(argv[i + 1]);
    }
    Mesh m = loadMesh(argv[1]);
    m.C.clear();
    m.G.clear();
    for (int l = 0; l < levels; l++) {
        size_t nt = m.nt();
        std::vector<std::pair<uint64_t, uint32_t>> es;
        es.reserve(nt * 3);
        for (size_t t = 0; t < nt; t++)
            for (int k = 0; k < 3; k++) {
                uint64_t a = m.T[t * 3 + k], b = m.T[t * 3 + (k + 1) % 3];
                es.push_back({std::min(a, b) << 32 | std::max(a, b), uint32_t(t * 3 + k)});
            }
        std::sort(es.begin(), es.end());
        std::vector<uint32_t> mid(nt * 3);
        for (size_t i = 0; i < es.size(); i++) {
            if (i == 0 || es[i].first != es[i - 1].first) {
                uint32_t a = uint32_t(es[i].first >> 32), b = uint32_t(es[i].first);
                for (int j = 0; j < 3; j++) m.P.push_back((m.P[a * 3 + j] + m.P[b * 3 + j]) * 0.5f);
            }
            mid[es[i].second] = uint32_t(m.nv() - 1);
        }
        std::vector<uint32_t> T;
        T.reserve(nt * 12);
        for (size_t t = 0; t < nt; t++) {
            uint32_t a = m.T[t * 3], b = m.T[t * 3 + 1], c = m.T[t * 3 + 2];
            uint32_t ab = mid[t * 3], bc = mid[t * 3 + 1], ca = mid[t * 3 + 2];
            T.insert(T.end(), {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca});
        }
        m.T.swap(T);
    }
    // 顶点法线 + 平均边长
    size_t nv = m.nv();
    std::vector<double> N(nv * 3, 0);
    double elen = 0;
    for (size_t t = 0; t < m.nt(); t++) {
        const float* a = &m.P[m.T[t * 3] * 3];
        const float* b = &m.P[m.T[t * 3 + 1] * 3];
        const float* c = &m.P[m.T[t * 3 + 2] * 3];
        double e1[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
        double e2[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
        double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 3; j++) N[m.T[t * 3 + k] * 3 + j] += n[j];
        elen += std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    }
    elen /= m.nt();
    double A = amp * elen;
    for (size_t i = 0; i < nv; i++) {
        double* n = &N[i * 3];
        double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l == 0) continue;
        float* p = &m.P[i * 3];
        double f = 0, s = 1 / (elen * 6);
        for (int o = 0; o < 4; o++, s *= 2.3)
            f += std::sin(p[0] * s + 1.3 * o) * std::sin(p[1] * s * 1.1 + 0.7 * o) * std::sin(p[2] * s * 0.9 + 2.1 * o) / (o + 1);
        for (int j = 0; j < 3; j++) p[j] += float(A * f * n[j] / l);
    }
    if (paint > 0) {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t i = 0; i < nv; i++)
            for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], m.P[i * 3 + j]); hi[j] = std::max(hi[j], m.P[i * 3 + j]); }
        double diag = std::sqrt(double(hi[0] - lo[0]) * (hi[0] - lo[0]) + double(hi[1] - lo[1]) * (hi[1] - lo[1]) + double(hi[2] - lo[2]) * (hi[2] - lo[2]));
        double cell = diag / paint;
        m.C.resize(nv);
        for (size_t i = 0; i < nv; i++) {
            double q[3], d2 = 0;
            for (int j = 0; j < 3; j++) { q[j] = (m.P[i * 3 + j] - lo[j]) / cell; double f = q[j] - std::floor(q[j]) - 0.5; d2 += f * f; }
            double d = std::sqrt(d2), line = std::fabs(q[0] + 0.5 * q[1] - std::floor(q[0] + 0.5 * q[1]) - 0.5);
            uint32_t c = 0xf0c8aa;                    // 肤色
            if (d < 0.07) c = 0x281e14;               // 瞳孔
            else if (d < 0.2) {                       // 虹膜：由内到外变暗的绿，加逐点噪声（像手绘的虹膜纹理）
                uint32_t hsh = uint32_t(i * 2654435761u);
                hsh ^= hsh >> 13; hsh *= 0x5bd1e995u; hsh ^= hsh >> 15;
                double nz = (int(hsh & 255) - 128) / 128.0 * 35, g = 1 - (d - 0.07) / 0.13 * 0.5;
                auto ch = [&](double v) { return uint32_t(std::clamp(v * g + nz, 0.0, 255.0)); };
                c = ch(150) << 16 | ch(170) << 8 | ch(60);
            } else if (d < 0.28) c = 0xfafafa;        // 眼白
            else if (d < 0.295) c = 0x101010;         // 细黑描边（像睫毛线）
            else if (line < 0.025) c = 0xc82828;      // 细红线
            m.C[i] = 0xff000000u | c;                 // 遮罩字节 ff = 没遮罩（和 ZBrush 导出的一样）
        }
    }
    saveMesh(argv[2], m);
    printf("输出 %zu 顶点，%zu 三角形\n", m.nv(), m.nt());
    return 0;
}
