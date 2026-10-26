// 造大测试模型：中点细分 N 次（每次面数 ×4），沿法线加一点多频起伏，模拟雕刻细节。
// 用法：meshgen 输入 输出 [--levels 1] [--amp 0.3]   amp 是起伏幅度（相对细分后平均边长）
#include "decimate.h"
#include <map>
#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) { printf("用法：meshgen 输入 输出 [--levels 1] [--amp 0.3]\n"); return 1; }
    int levels = 1;
    double amp = 0.3;
    for (int i = 3; i + 1 < argc; i += 2) {
        std::wstring k = argv[i];
        if (k == L"--levels") levels = _wtoi(argv[i + 1]);
        else if (k == L"--amp") amp = _wtof(argv[i + 1]);
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
    saveMesh(argv[2], m);
    printf("输出 %zu 顶点，%zu 三角形\n", m.nv(), m.nt());
    return 0;
}
