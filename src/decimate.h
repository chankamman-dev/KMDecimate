// 多线程二次误差（QEM）边折叠减面。
//
// 做法：按包围盒把三角形切成很多块，块与块交界的顶点锁住，各块各自建堆、并行折叠。
// 所有块共用同一个误差阈值 τ，一轮一轮往上抬，所以细节多的地方自然留得多（全局自适应），
// 不会出现"每块平均减同样比例"的问题。第一遍停在目标的 (1+slack) 倍，
// 第二遍把网格错开半格重新切块（上一遍的接缝落进块内部被正常处理），收到精确目标。
// threads=1 时退化成经典的单线程全局堆 QEM，用来做对照。
#pragma once
#include "meshio.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

struct DecimateOptions {
    size_t targetTris = 0;
    int threads = 0;            // 0 = 全部逻辑核
    int chunksPerThread = 6;    // 每个线程分到的块数（越多负载越均衡，接缝越多）
    double slack = 0.15;        // 倒数第二遍停在目标 ×(1+slack)，最后一遍错开网格收尾
    double keep = 0.25;         // 每一遍最多减到当前面数的这个比例
    double seamShare = 0.15;    // 接缝点最多占这一遍目标顶点数的比例，超了就少切几块
    double borderWeight = 10;   // 开放边界约束
    double groupWeight = 0;     // >0 时保留 polygroup 边界
    double flipCos = 0.2;       // 折叠后三角形法线与原法线夹角余弦的下限
};

struct PassStat {
    size_t fromTris = 0, goal = 0, parts = 0, seamVerts = 0;
    int rounds = 0;
    double time = 0, merge = 0;
};

struct DecimateStats {
    double setup = 0;
    std::vector<PassStat> passes;
    int threads = 1;
};

namespace kmd {

using Clock = std::chrono::steady_clock;
inline double secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

template <class F>
void parallelFor(size_t n, int threads, size_t grain, F&& f) {
    if (threads <= 1 || n <= grain) { for (size_t i = 0; i < n; i++) f(i); return; }
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (;;) {
            size_t s = next.fetch_add(grain);
            if (s >= n) break;
            size_t e = std::min(n, s + grain);
            for (size_t i = s; i < e; i++) f(i);
        }
    };
    std::vector<std::thread> ts;
    for (int i = 1; i < threads; i++) ts.emplace_back(work);
    work();
    for (auto& t : ts) t.join();
}

struct Quadric {
    double xx, xy, xz, xw, yy, yz, yw, zz, zw, ww;
    void addPlane(double a, double b, double c, double d, double w) {
        xx += w * a * a; xy += w * a * b; xz += w * a * c; xw += w * a * d;
        yy += w * b * b; yz += w * b * c; yw += w * b * d;
        zz += w * c * c; zw += w * c * d; ww += w * d * d;
    }
    Quadric& operator+=(const Quadric& o) {
        double* p = &xx; const double* q = &o.xx;
        for (int i = 0; i < 10; i++) p[i] += q[i];
        return *this;
    }
    Quadric operator+(const Quadric& o) const { Quadric r = *this; r += o; return r; }
    Quadric operator-(const Quadric& o) const {
        Quadric r = *this; double* p = &r.xx; const double* q = &o.xx;
        for (int i = 0; i < 10; i++) p[i] -= q[i];
        return r;
    }
    double eval(double x, double y, double z) const {
        return xx * x * x + 2 * xy * x * y + 2 * xz * x * z + 2 * xw * x + yy * y * y + 2 * yz * y * z + 2 * yw * y +
               zz * z * z + 2 * zw * z + ww;
    }
    bool optimum(double& x, double& y, double& z) const {
        double c00 = yy * zz - yz * yz, c01 = xz * yz - xy * zz, c02 = xy * yz - xz * yy;
        double det = xx * c00 + xy * c01 + xz * c02;
        double tr = xx + yy + zz;
        if (!(std::fabs(det) > 1e-10 * tr * tr * tr)) return false;
        double c11 = xx * zz - xz * xz, c12 = xy * xz - xx * yz, c22 = xx * yy - xy * xy;
        double inv = 1 / det;
        x = -(c00 * xw + c01 * yw + c02 * zw) * inv;
        y = -(c01 * xw + c11 * yw + c12 * zw) * inv;
        z = -(c02 * xw + c12 * yw + c22 * zw) * inv;
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
};

enum : uint8_t { LOCKED = 1, BORDER = 2, DEAD = 4, PERMLOCK = 8 };

// 一遍处理用的全局网格（只含存活的顶点和三角形）
struct Ctx {
    std::vector<float> P;
    std::vector<Quadric> Q;
    std::vector<float> Col;   // 4 × nv，可空
    std::vector<uint8_t> fl;  // BORDER / PERMLOCK
    std::vector<uint32_t> T;
    std::vector<uint16_t> G;
    size_t nv() const { return P.size() / 3; }
    size_t nt() const { return T.size() / 3; }
};

struct HeapE { float cost; uint32_t a, b, sa, sb; };
struct HeapCmp { bool operator()(const HeapE& x, const HeapE& y) const { return x.cost > y.cost; } };

struct Part {
    const DecimateOptions* opt = nullptr;
    bool hasCol = false;
    std::vector<uint32_t> gid;
    std::vector<float> pos, col;
    std::vector<Quadric> Q;
    std::vector<uint8_t> fl, tdead;
    std::vector<uint32_t> stamp, tri, aS, aN, aC, pool;
    std::vector<uint16_t> grp;
    std::vector<HeapE> heap;
    size_t alive = 0;
    std::vector<uint32_t> Tu, Tv, Nu, Nv, tmp;

    void build(const Ctx& g, const uint32_t* tids, size_t m, const std::vector<uint8_t>& seam) {
        hasCol = !g.Col.empty();
        gid.resize(m * 3);
        for (size_t i = 0; i < m; i++)
            for (int k = 0; k < 3; k++) gid[i * 3 + k] = g.T[size_t(tids[i]) * 3 + k];
        tri = gid;
        std::sort(gid.begin(), gid.end());
        gid.erase(std::unique(gid.begin(), gid.end()), gid.end());
        gid.shrink_to_fit();
        for (auto& v : tri) v = uint32_t(std::lower_bound(gid.begin(), gid.end(), v) - gid.begin());
        size_t n = gid.size();
        pos.resize(n * 3); Q.resize(n); fl.resize(n); stamp.assign(n, 0);
        if (hasCol) col.resize(n * 4);
        for (size_t i = 0; i < n; i++) {
            uint32_t v = gid[i];
            memcpy(&pos[i * 3], &g.P[size_t(v) * 3], 12);
            Q[i] = g.Q[v];
            if (hasCol) memcpy(&col[i * 4], &g.Col[size_t(v) * 4], 16);
            fl[i] = (g.fl[v] & (BORDER | PERMLOCK)) | ((seam[v] || (g.fl[v] & PERMLOCK)) ? LOCKED : 0);
        }
        if (!g.G.empty()) { grp.resize(m); for (size_t i = 0; i < m; i++) grp[i] = g.G[tids[i]]; }
        tdead.assign(m, 0);
        alive = m;
        aN.assign(n, 0);
        for (auto v : tri) aN[v]++;
        aS.resize(n); aC.resize(n);
        size_t s = 0;
        for (size_t i = 0; i < n; i++) { aS[i] = uint32_t(s); aC[i] = aN[i] + 2; s += aC[i]; aN[i] = 0; }
        pool.resize(s);
        for (size_t t = 0; t < m; t++)
            for (int k = 0; k < 3; k++) { uint32_t v = tri[t * 3 + k]; pool[aS[v] + aN[v]++] = uint32_t(t); }
        // 初始边
        heap.clear();
        heap.reserve(n * 3);
        for (uint32_t a = 0; a < n; a++) {
            neighbors(a, Nu);
            for (uint32_t b : Nu) {
                if (b < a) continue;
                uint32_t u, v; double x[3], c;
                if (plan(a, b, u, v, x, c)) heap.push_back({float(c), a, b, 0, 0});
            }
        }
        std::make_heap(heap.begin(), heap.end(), HeapCmp());
    }

    void trisOf(uint32_t v, std::vector<uint32_t>& out) {
        out.clear();
        uint32_t* L = &pool[aS[v]];
        uint32_t n = aN[v], w = 0;
        for (uint32_t i = 0; i < n; i++) if (!tdead[L[i]]) { L[w++] = L[i]; out.push_back(L[i]); }
        aN[v] = w;
    }

    void neighbors(uint32_t v, std::vector<uint32_t>& out) {
        trisOf(v, tmp);
        out.clear();
        for (uint32_t t : tmp)
            for (int k = 0; k < 3; k++) { uint32_t w = tri[t * 3 + k]; if (w != v) out.push_back(w); }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void setAdj(uint32_t v, const std::vector<uint32_t>& L) {
        if (L.size() > aC[v]) {
            aS[v] = uint32_t(pool.size());
            aC[v] = uint32_t(L.size() * 2);
            pool.resize(pool.size() + aC[v]);
        }
        std::copy(L.begin(), L.end(), pool.begin() + aS[v]);
        aN[v] = uint32_t(L.size());
    }

    // 决定谁留下、新位置在哪、代价多少
    bool plan(uint32_t a, uint32_t b, uint32_t& u, uint32_t& v, double x[3], double& cost) const {
        uint8_t fa = fl[a], fb = fl[b];
        if ((fa & LOCKED) && (fb & LOCKED)) return false;
        u = a; v = b;
        if ((fb & LOCKED) || ((fb & BORDER) && !(fa & BORDER))) std::swap(u, v);
        if ((fl[v] & BORDER) && !(fl[u] & BORDER)) return false;  // 边界点不能并进内部的锁定点
        Quadric q = Q[u] + Q[v];
        const float* pu = &pos[size_t(u) * 3];
        const float* pv = &pos[size_t(v) * 3];
        if (fl[u] & LOCKED) {
            x[0] = pu[0]; x[1] = pu[1]; x[2] = pu[2];
        } else {
            double m[3] = {(pu[0] + pv[0]) * 0.5, (pu[1] + pv[1]) * 0.5, (pu[2] + pv[2]) * 0.5};
            double e[3] = {double(pv[0]) - pu[0], double(pv[1]) - pu[1], double(pv[2]) - pu[2]};
            double L2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
            bool ok = q.optimum(x[0], x[1], x[2]);
            if (ok) {
                double d[3] = {x[0] - m[0], x[1] - m[1], x[2] - m[2]};
                ok = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= L2;
            }
            if (!ok) {  // 退回到沿边找最小值
                double f0 = q.eval(pu[0], pu[1], pu[2]), f1 = q.eval(pv[0], pv[1], pv[2]), fh = q.eval(m[0], m[1], m[2]);
                double A = 2 * f1 + 2 * f0 - 4 * fh, B = f1 - f0 - A;
                double t = A > 0 ? std::clamp(-B / (2 * A), 0.0, 1.0) : (f0 <= f1 ? 0.0 : 1.0);
                x[0] = pu[0] + t * e[0]; x[1] = pu[1] + t * e[1]; x[2] = pu[2] + t * e[2];
            }
        }
        cost = std::max(0.0, q.eval(x[0], x[1], x[2]));
        return true;
    }

    bool flipOk(uint32_t t, uint32_t mv, const double x[3]) const {
        double p[3][3], q[3][3];
        for (int k = 0; k < 3; k++) {
            uint32_t i = tri[t * 3 + k];
            for (int j = 0; j < 3; j++) p[k][j] = q[k][j] = pos[size_t(i) * 3 + j];
            if (i == mv) for (int j = 0; j < 3; j++) q[k][j] = x[j];
        }
        auto nrm = [](double (*r)[3], double n[3]) {
            double a[3] = {r[1][0] - r[0][0], r[1][1] - r[0][1], r[1][2] - r[0][2]};
            double b[3] = {r[2][0] - r[0][0], r[2][1] - r[0][1], r[2][2] - r[0][2]};
            n[0] = a[1] * b[2] - a[2] * b[1]; n[1] = a[2] * b[0] - a[0] * b[2]; n[2] = a[0] * b[1] - a[1] * b[0];
        };
        double n0[3], n1[3];
        nrm(p, n0); nrm(q, n1);
        double l0 = std::sqrt(n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2]);
        double l1 = std::sqrt(n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2]);
        if (l0 == 0) return true;
        if (l1 == 0) return false;
        return n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] > opt->flipCos * l0 * l1;
    }

    bool has(uint32_t t, uint32_t v) const { return tri[t * 3] == v || tri[t * 3 + 1] == v || tri[t * 3 + 2] == v; }

    // 把 v 并进 u，u 移到 x。返回删掉的三角形数（0 = 拒绝）
    int collapse(uint32_t u, uint32_t v, const double x[3]) {
        trisOf(u, Tu);
        trisOf(v, Tv);
        int shared = 0;
        for (uint32_t t : Tv) shared += has(t, u);
        if (shared == 0 || shared > 2) return 0;
        if ((fl[u] & BORDER) && (fl[v] & BORDER) && shared == 2) return 0;
        neighbors(u, Nu);
        neighbors(v, Nv);
        int common = 0;
        for (size_t i = 0, j = 0; i < Nu.size() && j < Nv.size();) {
            if (Nu[i] < Nv[j]) i++;
            else if (Nu[i] > Nv[j]) j++;
            else { common++; i++; j++; }
        }
        if (common != shared) return 0;
        for (uint32_t t : Tv) if (!has(t, u) && !flipOk(t, v, x)) return 0;
        for (uint32_t t : Tu) if (!has(t, v) && !flipOk(t, u, x)) return 0;
        // 生效
        if (hasCol) {
            const float* pu = &pos[size_t(u) * 3];
            const float* pv = &pos[size_t(v) * 3];
            double e[3] = {double(pv[0]) - pu[0], double(pv[1]) - pu[1], double(pv[2]) - pu[2]};
            double L2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
            double s = L2 > 0 ? std::clamp(((x[0] - pu[0]) * e[0] + (x[1] - pu[1]) * e[1] + (x[2] - pu[2]) * e[2]) / L2, 0.0, 1.0) : 0;
            for (int j = 0; j < 4; j++) col[u * 4 + j] = float(col[u * 4 + j] * (1 - s) + col[v * 4 + j] * s);
        }
        tmp.clear();
        for (uint32_t t : Tv) {
            if (has(t, u)) { tdead[t] = 1; continue; }
            for (int k = 0; k < 3; k++) if (tri[t * 3 + k] == v) tri[t * 3 + k] = u;
            tmp.push_back(t);
        }
        for (uint32_t t : Tu) if (!tdead[t]) tmp.push_back(t);
        setAdj(u, tmp);
        aN[v] = 0;
        pos[u * 3] = float(x[0]); pos[u * 3 + 1] = float(x[1]); pos[u * 3 + 2] = float(x[2]);
        Q[u] += Q[v];
        fl[u] |= fl[v] & BORDER;
        fl[v] |= DEAD;
        stamp[u]++;
        alive -= shared;
        neighbors(u, Nu);
        for (uint32_t w : Nu) {
            uint32_t a, b; double y[3], c;
            if (plan(u, w, a, b, y, c)) {
                heap.push_back({float(c), u, w, stamp[u], stamp[w]});
                std::push_heap(heap.begin(), heap.end(), HeapCmp());
            }
        }
        return shared;
    }

    // 折叠所有代价 ≤ tau 的边，直到全局预算用完。返回删掉的三角形数。
    size_t run(double tau, std::atomic<int64_t>& budget, int batch) {
        size_t removed = 0;
        int64_t pending = 0;
        bool stop = budget.load(std::memory_order_relaxed) <= 0;
        while (!stop && !heap.empty()) {
            if (heap.front().cost > tau) break;
            HeapE e = heap.front();
            std::pop_heap(heap.begin(), heap.end(), HeapCmp());
            heap.pop_back();
            if ((fl[e.a] | fl[e.b]) & DEAD) continue;
            if (stamp[e.a] != e.sa || stamp[e.b] != e.sb) continue;
            uint32_t u, v; double x[3], c;
            if (!plan(e.a, e.b, u, v, x, c)) continue;
            int r = collapse(u, v, x);
            if (!r) continue;
            removed += r;
            pending += r;
            if (pending >= batch) {
                stop = budget.fetch_sub(pending, std::memory_order_relaxed) - pending <= 0;
                pending = 0;
            }
        }
        if (pending) budget.fetch_sub(pending, std::memory_order_relaxed);
        if (heap.size() > 4 * (alive * 3 / 2) + 1024) {  // 过期条目太多时清理
            std::erase_if(heap, [&](const HeapE& h) {
                return ((fl[h.a] | fl[h.b]) & DEAD) || stamp[h.a] != h.sa || stamp[h.b] != h.sb;
            });
            std::make_heap(heap.begin(), heap.end(), HeapCmp());
        }
        return removed;
    }
};

inline Ctx makeCtx(const Mesh& m, const DecimateOptions& o, int threads) {
    Ctx g;
    g.P = m.P;
    g.T = m.T;
    g.G = m.G;
    size_t nv = m.nv(), nt = m.nt();
    if (!m.C.empty()) {
        g.Col.resize(nv * 4);
        for (size_t i = 0; i < nv; i++) {
            uint32_t c = m.C[i];
            g.Col[i * 4] = float(c >> 16 & 255); g.Col[i * 4 + 1] = float(c >> 8 & 255);
            g.Col[i * 4 + 2] = float(c & 255); g.Col[i * 4 + 3] = float(c >> 24);
        }
    }
    // 顶点 → 三角形（CSR）
    std::vector<uint32_t> start(nv + 1, 0), list(nt * 3);
    for (auto v : g.T) start[v + 1]++;
    for (size_t i = 0; i < nv; i++) start[i + 1] += start[i];
    {
        std::vector<uint32_t> fill(start.begin(), start.end() - 1);
        for (size_t t = 0; t < nt; t++)
            for (int k = 0; k < 3; k++) list[fill[g.T[t * 3 + k]]++] = uint32_t(t);
    }
    g.Q.assign(nv, Quadric{});
    g.fl.assign(nv, 0);
    bool groups = o.groupWeight > 0 && !g.G.empty();
    parallelFor(nv, threads, 4096, [&](size_t v) {
        Quadric q{};
        struct E { uint32_t w, t; };
        E es[64];
        std::vector<E> big;
        E* ep = es;
        size_t ne = 0, deg = start[v + 1] - start[v];
        if (deg * 2 > 64) { big.resize(deg * 2); ep = big.data(); }
        const float* P = g.P.data();
        auto tnorm = [&](uint32_t t, double n[3]) {
            const float* a = P + size_t(g.T[t * 3]) * 3;
            const float* b = P + size_t(g.T[t * 3 + 1]) * 3;
            const float* c = P + size_t(g.T[t * 3 + 2]) * 3;
            double e1[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
            double e2[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
            n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        };
        for (uint32_t i = start[v]; i < start[v + 1]; i++) {
            uint32_t t = list[i];
            double n[3];
            tnorm(t, n);
            double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 0) {
                const float* a = P + size_t(g.T[t * 3]) * 3;
                double nn[3] = {n[0] / len, n[1] / len, n[2] / len};
                q.addPlane(nn[0], nn[1], nn[2], -(nn[0] * a[0] + nn[1] * a[1] + nn[2] * a[2]), len * 0.5);
            }
            for (int k = 0; k < 3; k++) if (g.T[t * 3 + k] != v) ep[ne++] = {g.T[t * 3 + k], t};
        }
        std::sort(ep, ep + ne, [](const E& x, const E& y) { return x.w < y.w; });
        const float* pv = P + v * 3;
        auto edgePlane = [&](uint32_t w, uint32_t t, double weight) {
            const float* pw = P + size_t(w) * 3;
            double e[3] = {double(pw[0]) - pv[0], double(pw[1]) - pv[1], double(pw[2]) - pv[2]};
            double n[3];
            tnorm(t, n);
            double b[3] = {e[1] * n[2] - e[2] * n[1], e[2] * n[0] - e[0] * n[2], e[0] * n[1] - e[1] * n[0]};
            double bl = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
            if (bl == 0) return;
            for (auto& c : b) c /= bl;
            double L2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
            q.addPlane(b[0], b[1], b[2], -(b[0] * pv[0] + b[1] * pv[1] + b[2] * pv[2]), weight * L2);
        };
        uint8_t f = 0;
        for (size_t i = 0; i < ne;) {
            size_t j = i;
            while (j < ne && ep[j].w == ep[i].w) j++;
            if (j - i == 1) { f |= BORDER; edgePlane(ep[i].w, ep[i].t, o.borderWeight); }
            else if (j - i > 2) f |= PERMLOCK;
            else if (groups && g.G[ep[i].t] != g.G[ep[i + 1].t]) {
                edgePlane(ep[i].w, ep[i].t, o.groupWeight);
                edgePlane(ep[i].w, ep[i + 1].t, o.groupWeight);
            }
            i = j;
        }
        g.Q[v] = q;
        g.fl[v] = f;
    });
    return g;
}

// 选格子大小，使非空格子数接近 want（用三角形重心的抽样估计）
inline double pickCell(const Ctx& g, const float* lo, const float* hi, size_t want) {
    size_t nt = g.nt(), step = std::max<size_t>(1, nt / 200000);
    std::vector<float> cs;
    for (size_t t = 0; t < nt; t += step)
        for (int j = 0; j < 3; j++)
            cs.push_back((g.P[g.T[t * 3] * 3 + j] + g.P[g.T[t * 3 + 1] * 3 + j] + g.P[g.T[t * 3 + 2] * 3 + j]) / 3);
    double ext = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-20f});
    double a = ext / 2000, b = ext * 1.01;
    std::vector<uint64_t> keys;
    for (int it = 0; it < 40; it++) {
        double c = std::sqrt(a * b);
        keys.clear();
        for (size_t i = 0; i < cs.size(); i += 3) {
            uint64_t ix = uint64_t((cs[i] - lo[0]) / c), iy = uint64_t((cs[i + 1] - lo[1]) / c), iz = uint64_t((cs[i + 2] - lo[2]) / c);
            keys.push_back(ix << 42 | iy << 21 | iz);
        }
        std::sort(keys.begin(), keys.end());
        size_t cnt = std::unique(keys.begin(), keys.end()) - keys.begin();
        if (cnt > want) a = c; else b = c;
        if (b / a < 1.02) break;
    }
    return std::sqrt(a * b);
}

}  // namespace kmd

inline Mesh decimate(const Mesh& in, const DecimateOptions& o, DecimateStats& st) {
    using namespace kmd;
    int threads = o.threads > 0 ? o.threads : int(std::max(1u, std::thread::hardware_concurrency()));
    st.threads = threads;
    auto t0 = Clock::now();
    Ctx g = makeCtx(in, o, threads);
    st.setup = secs(t0);
    size_t target = std::max<size_t>(o.targetTris, 4);

    for (int pi = 0; g.nt() > target && pi < 64; pi++) {
        auto tp = Clock::now();
        size_t nv = g.nv(), nt = g.nt();
        size_t pre = size_t(target * (1 + o.slack));
        size_t goal = threads > 1 && nt > pre ? std::max(pre, size_t(nt * o.keep)) : target;
        // ---- 切块：块数从 threads×chunksPerThread 开始，接缝点太多就少切
        std::vector<uint32_t> chunkOf(nt, 0);
        std::vector<uint8_t> seam(nv, 0);
        size_t nparts = 1, nseam = 0;
        bool single = threads == 1 || nt < 20000;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        if (!single)
            for (size_t i = 0; i < nv; i++)
                for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], g.P[i * 3 + j]); hi[j] = std::max(hi[j], g.P[i * 3 + j]); }
        for (size_t want = size_t(threads) * o.chunksPerThread; !single;) {
            double cell = pickCell(g, lo, hi, want);
            double off = (pi & 1) * 0.5 * cell;
            int dim[3];
            for (int j = 0; j < 3; j++) dim[j] = int((hi[j] - lo[j] + off) / cell) + 1;
            std::vector<uint32_t> denseOf(size_t(dim[0]) * dim[1] * dim[2], UINT32_MAX);
            nparts = 0;
            for (size_t t = 0; t < nt; t++) {
                int c[3];
                for (int j = 0; j < 3; j++) {
                    float ctr = (g.P[g.T[t * 3] * 3 + j] + g.P[g.T[t * 3 + 1] * 3 + j] + g.P[g.T[t * 3 + 2] * 3 + j]) / 3;
                    c[j] = std::clamp(int((ctr - lo[j] + off) / cell), 0, dim[j] - 1);
                }
                uint32_t& d = denseOf[(size_t(c[0]) * dim[1] + c[1]) * dim[2] + c[2]];
                if (d == UINT32_MAX) d = uint32_t(nparts++);
                chunkOf[t] = d;
            }
            std::fill(seam.begin(), seam.end(), 0);
            std::vector<uint32_t> owner(nv, UINT32_MAX);
            for (size_t t = 0; t < nt; t++)
                for (int k = 0; k < 3; k++) {
                    uint32_t v = g.T[t * 3 + k];
                    if (owner[v] == UINT32_MAX) owner[v] = chunkOf[t];
                    else if (owner[v] != chunkOf[t]) seam[v] = 1;
                }
            nseam = 0;
            for (auto s : seam) nseam += s;
            if (nseam <= o.seamShare * goal / 2) break;
            want = want * 6 / 10;
            if (want < 2) single = true;
        }
        if (single) { nparts = 1; nseam = 0; std::fill(chunkOf.begin(), chunkOf.end(), 0); std::fill(seam.begin(), seam.end(), 0); }
        PassStat ps;
        ps.fromTris = nt; ps.goal = goal; ps.parts = nparts; ps.seamVerts = nseam;
        std::vector<uint32_t> pstart(nparts + 1, 0), tids(nt);
        for (auto c : chunkOf) pstart[c + 1]++;
        for (size_t i = 0; i < nparts; i++) pstart[i + 1] += pstart[i];
        {
            std::vector<uint32_t> fill(pstart.begin(), pstart.end() - 1);
            for (size_t t = 0; t < nt; t++) tids[fill[chunkOf[t]]++] = uint32_t(t);
        }
        // 大块先做
        std::vector<uint32_t> order(nparts);
        for (size_t i = 0; i < nparts; i++) order[i] = uint32_t(i);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return pstart[a + 1] - pstart[a] > pstart[b + 1] - pstart[b]; });
        std::vector<Part> parts(nparts);
        parallelFor(nparts, threads, 1, [&](size_t i) {
            uint32_t p = order[i];
            parts[p].opt = &o;
            parts[p].build(g, &tids[pstart[p]], pstart[p + 1] - pstart[p], seam);
        });
        // ---- 一轮一轮抬阈值
        size_t cur = nt;
        double tau = 0;
        int stuck = 0;
        std::atomic<int64_t> budget{0};
        for (int round = 0; round < 200 && cur > goal; round++) {
            if (single) tau = INFINITY;
            else {
                std::vector<float> smp;
                size_t per = std::max<size_t>(16, 65536 / nparts);
                for (auto& p : parts) {
                    size_t n = p.heap.size(), step = std::max<size_t>(1, n / per);
                    for (size_t i = 0; i < n; i += step) smp.push_back(p.heap[i].cost);
                }
                if (smp.empty()) break;
                double q = std::clamp(0.6 * double(cur - goal) / 2 / (1.5 * cur), 1e-4, 0.95);
                size_t k = std::min(smp.size() - 1, size_t(q * smp.size()));
                std::nth_element(smp.begin(), smp.begin() + k, smp.end());
                tau = std::max({double(smp[k]), tau * 1.25, 1e-30});
            }
            budget.store(int64_t(cur - goal));
            int batch = int(std::clamp<int64_t>(int64_t(cur - goal) / (int64_t(threads) * 64), 1, 256));
            std::atomic<size_t> removed{0};
            parallelFor(nparts, threads, 1, [&](size_t i) { removed += parts[order[i]].run(tau, budget, batch); });
            cur -= removed;
            ps.rounds++;
            if (removed == 0) { tau *= 8; if (++stuck > 8) break; }
            else stuck = 0;
            if (single) break;
        }
        ps.time = secs(tp);
        // ---- 合并成新的全局网格
        auto tm = Clock::now();
        std::vector<uint32_t> seamId(nv, UINT32_MAX);
        uint32_t ns = 0;
        for (size_t v = 0; v < nv; v++) if (seam[v]) seamId[v] = ns++;
        std::vector<size_t> vOff(nparts + 1, ns), tOff(nparts + 1, 0);
        for (size_t p = 0; p < nparts; p++) {
            size_t a = 0;
            for (size_t i = 0; i < parts[p].gid.size(); i++) a += !(parts[p].fl[i] & DEAD) && !seam[parts[p].gid[i]];
            vOff[p + 1] = vOff[p] + a;
            tOff[p + 1] = tOff[p] + parts[p].alive;
        }
        Ctx h;
        size_t NV = vOff[nparts], NT = tOff[nparts];
        h.P.resize(NV * 3); h.Q.resize(NV); h.fl.resize(NV); h.T.resize(NT * 3);
        if (!g.Col.empty()) h.Col.resize(NV * 4);
        if (!g.G.empty()) h.G.resize(NT);
        for (size_t v = 0; v < nv; v++)
            if (seam[v]) {
                uint32_t s = seamId[v];
                memcpy(&h.P[s * 3], &g.P[v * 3], 12);
                if (!g.Col.empty()) memcpy(&h.Col[s * 4], &g.Col[v * 4], 16);
                h.Q[s] = g.Q[v];
                h.fl[s] = g.fl[v];
            }
        // 接缝点的误差累积：各块里的增量加回去
        for (size_t p = 0; p < nparts; p++)
            for (size_t i = 0; i < parts[p].gid.size(); i++) {
                uint32_t v = parts[p].gid[i];
                if (seam[v]) h.Q[seamId[v]] += parts[p].Q[i] - g.Q[v];
            }
        parallelFor(nparts, threads, 1, [&](size_t p) {
            Part& P = parts[p];
            std::vector<uint32_t> nid(P.gid.size(), UINT32_MAX);
            size_t k = vOff[p];
            for (size_t i = 0; i < P.gid.size(); i++) {
                if (P.fl[i] & DEAD) continue;
                if (seam[P.gid[i]]) { nid[i] = seamId[P.gid[i]]; continue; }
                nid[i] = uint32_t(k);
                memcpy(&h.P[k * 3], &P.pos[i * 3], 12);
                if (P.hasCol) memcpy(&h.Col[k * 4], &P.col[i * 4], 16);
                h.Q[k] = P.Q[i];
                h.fl[k] = P.fl[i] & (BORDER | PERMLOCK);
                k++;
            }
            size_t w = tOff[p];
            for (size_t t = 0; t < P.tdead.size(); t++) {
                if (P.tdead[t]) continue;
                for (int j = 0; j < 3; j++) h.T[w * 3 + j] = nid[P.tri[t * 3 + j]];
                if (!h.G.empty()) h.G[w] = P.grp[t];
                w++;
            }
            std::vector<HeapE>().swap(P.heap);  // 尽早释放
        });
        g = std::move(h);
        ps.merge = secs(tm);
        st.passes.push_back(ps);
    }

    // 输出：去掉没被用到的顶点
    Mesh out;
    std::vector<uint32_t> remap(g.nv(), UINT32_MAX);
    out.T.resize(g.T.size());
    for (size_t i = 0; i < g.T.size(); i++) {
        uint32_t v = g.T[i];
        if (remap[v] == UINT32_MAX) {
            remap[v] = uint32_t(out.nv());
            out.P.insert(out.P.end(), {g.P[v * 3], g.P[v * 3 + 1], g.P[v * 3 + 2]});
            if (!g.Col.empty()) {
                auto c8 = [&](int j) { return uint32_t(std::clamp(int(std::lround(g.Col[v * 4 + j])), 0, 255)); };
                out.C.push_back(c8(3) << 24 | c8(0) << 16 | c8(1) << 8 | c8(2));
            }
        }
        out.T[i] = remap[v];
    }
    out.G = g.G;
    out.gozHead = in.gozHead;
    return out;
}
