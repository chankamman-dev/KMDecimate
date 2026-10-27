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
    int chunksPerThread = 6;    // 每个线程至少分到的块数（越多负载越均衡，接缝越多）
    size_t partTris = 30000;    // 大模型按每块约这么多三角形切：块小、堆小，留在 CPU 缓存里（4800 万面快 1.5 倍）
    double slack = 0.15;        // 倒数第二遍停在目标 ×(1+slack)，最后一遍错开网格收尾
    double keep = 0.25;         // 每一遍最多减到当前面数的这个比例
    double seamShare = 0.15;    // 接缝点最多占这一遍目标顶点数的比例，超了就少切几块
    double borderWeight = 10;   // 开放边界约束
    double groupWeight = 0;     // >0 时保留 polygroup 边界
    double colorWeight = 0.3;   // >0 且有 Polypaint 时，颜色差大的边更晚折叠（颜色边界附近多留面）
    double colorEdge = 0.3;     // >0 时颜色边界两侧的点只能沿边界滑，不能往边界靠或离开（边界保持锐利）
    double flipCos = 0.2;       // 折叠后三角形法线与原法线夹角余弦的下限
    bool fastExit = false;      // 调用者减完就退出进程：用过的大块内存不在这里释放，留给进程退出时一起回收
                                // （单线程释放几 GB 要 0.5 秒以上，而插件只等 status.txt，不等进程退出）
};

struct PassStat {
    size_t fromTris = 0, goal = 0, parts = 0, seamVerts = 0;
    int rounds = 0;
    double time = 0, merge = 0, cut = 0, build = 0;
};

struct DecimateStats {
    double setup = 0;
    std::vector<PassStat> passes;
    int threads = 1;
};

namespace kmd {

using Clock = std::chrono::steady_clock;
inline double secs(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

// 进度和取消（KMDecimate.exe 的监视线程读写）：gRemoved 是已经删掉的三角形数；
// gCancel 置 true 后各块在下一批折叠后停下，decimate 抛出 Cancelled
inline std::atomic<size_t> gRemoved{0};
inline std::atomic<bool> gCancel{false};
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("已取消") {}
};

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

// 把 [0, n) 切成 nb 段并行处理：f(段号, 起, 止)。段数固定，分段结果和线程调度无关
template <class F>
void parallelBlocks(size_t n, int threads, size_t nb, F&& f) {
    parallelFor(nb, threads, 1, [&](size_t b) { f(b, n * b / nb, n * (b + 1) / nb); });
}

// fastExit 时用：把对象挪到堆上再也不释放，进程退出时由系统一起回收
template <class T>
void keepUntilExit(T&& x) { new std::decay_t<T>(std::move(x)); }

// 按 64 位键的高 32 位（只看低 bits 位）做 LSD 基数排序，稳定
inline void radixHigh(std::vector<uint64_t>& a, std::vector<uint64_t>& tmp, int bits) {
    tmp.resize(a.size());
    for (int shift = 32; shift < 32 + bits; shift += 11) {
        uint32_t cnt[2049] = {};
        for (uint64_t k : a) cnt[((k >> shift) & 2047) + 1]++;
        for (int i = 0; i < 2048; i++) cnt[i + 1] += cnt[i];
        for (uint64_t k : a) tmp[cnt[(k >> shift) & 2047]++] = k;
        a.swap(tmp);
    }
}

// resize 时不清零的分配器：大数组（上亿个元素）随后会被并行写满，先单线程清零一遍纯属浪费
template <class T>
struct NoInit : std::allocator<T> {
    template <class U> struct rebind { using other = NoInit<U>; };
    NoInit() = default;
    template <class U> NoInit(const NoInit<U>&) noexcept {}
    template <class U> void construct(U* p) noexcept { ::new (static_cast<void*>(p)) U; }
    template <class U, class... A> void construct(U* p, A&&... a) { ::new (static_cast<void*>(p)) U(std::forward<A>(a)...); }
};
template <class T> using Buf = std::vector<T, NoInit<T>>;

template <class T, class A>
void parCopy(Buf<T>& dst, const std::vector<T, A>& src, int threads) {
    dst.resize(src.size());
    parallelBlocks(src.size(), threads, size_t(threads) * 4,
                   [&](size_t, size_t b, size_t e) { std::copy(src.begin() + b, src.begin() + e, dst.begin() + b); });
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
    Buf<float> P;
    Buf<Quadric> Q;
    Buf<float> Col;   // 4 × nv，可空
    Buf<uint8_t> fl;  // BORDER / PERMLOCK
    Buf<uint32_t> T;
    Buf<uint16_t> G;
    size_t nv() const { return P.size() / 3; }
    size_t nt() const { return T.size() / 3; }
};

struct HeapE { float cost; uint32_t a, b, sa, sb; };
// 4 叉最小堆：比 std 的二叉堆浅一半，4 个孩子挨着放，弹出时少一半缓存缺失
inline void heapDown(std::vector<HeapE>& h, size_t i) {
    size_t n = h.size();
    HeapE x = h[i];
    for (;;) {
        size_t c = i * 4 + 1;
        if (c >= n) break;
        size_t e = std::min(c + 4, n), m = c;
        for (size_t j = c + 1; j < e; j++) if (h[j].cost < h[m].cost) m = j;
        if (!(h[m].cost < x.cost)) break;
        h[i] = h[m];
        i = m;
    }
    h[i] = x;
}
inline void heapPush(std::vector<HeapE>& h, const HeapE& x) {
    size_t i = h.size();
    h.push_back(x);
    while (i > 0) {
        size_t p = (i - 1) / 4;
        if (!(x.cost < h[p].cost)) break;
        h[i] = h[p];
        i = p;
    }
    h[i] = x;
}
inline void heapPop(std::vector<HeapE>& h) {
    h.front() = h.back();
    h.pop_back();
    if (!h.empty()) heapDown(h, 0);
}
inline void heapMake(std::vector<HeapE>& h) {
    for (size_t i = h.size() / 4 + 1; i-- > 0;) if (i < h.size()) heapDown(h, i);
}

struct Part {
    const DecimateOptions* opt = nullptr;
    bool hasCol = false;
    std::vector<uint32_t> gid;
    std::vector<float> pos, col;
    std::vector<Quadric> Q;
    std::vector<uint8_t> fl, tdead;
    std::vector<uint32_t> stamp, tri, aS, aN, aC, pool;
    std::vector<uint16_t> grp;
    // 堆里只放这一轮会处理的边（代价 ≤ tauCur），其余放在不排序的 over 里，每轮开始时扫一遍：
    // 过期和已删顶点的条目直接扔掉，够便宜的进堆。以前全放堆里，87% 的出堆是这种垃圾，每次出堆
    // 都要在超出缓存的堆里下沉，占了折叠循环将近一半的时间。处理顺序和以前一样（只是不再排序永远轮不到的边）。
    std::vector<HeapE> heap, over;
    double tauCur = -1;
    size_t alive = 0;

    bool valid(const HeapE& e) const { return !((fl[e.a] | fl[e.b]) & DEAD) && stamp[e.a] == e.sa && stamp[e.b] == e.sb; }
    void push(const HeapE& e) {
        if (e.cost <= tauCur) heapPush(heap, e);
        else over.push_back(e);
    }
    std::vector<uint32_t> Tu, Tv, Nu, tmp;
    std::vector<uint32_t> mark;  // nbrsOf / 连接条件用的访问标记，值是 gen
    uint32_t gen = 0;
    std::vector<uint64_t> rk, rk2;  // build 里编局部号用

    void build(const Ctx& g, const uint32_t* tids, size_t m, const Buf<uint8_t>& seam) {
        hasCol = !g.Col.empty();
        // 局部编号：(全局号, 角号) 按全局号基数排序，一遍扫出不重复的顶点，局部号按全局号从小到大。
        // 以前是排序 + 每个角二分查找，占了建堆一半的时间
        rk.resize(m * 3);
        for (size_t i = 0; i < m; i++)
            for (int k = 0; k < 3; k++) rk[i * 3 + k] = uint64_t(g.T[size_t(tids[i]) * 3 + k]) << 32 | uint32_t(i * 3 + k);
        int bits = 1;
        while ((size_t(1) << bits) < g.nv()) bits++;
        radixHigh(rk, rk2, bits);
        gid.clear();
        tri.resize(m * 3);
        for (uint64_t key : rk) {
            uint32_t v = uint32_t(key >> 32);
            if (gid.empty() || gid.back() != v) gid.push_back(v);
            tri[uint32_t(key)] = uint32_t(gid.size() - 1);
        }
        size_t n = gid.size();
        pos.resize(n * 3); Q.resize(n); fl.resize(n); stamp.assign(n, 0); mark.assign(n, 0); gen = 0;
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
        // 初始边：先全放进 over，第一轮 run 再挑出够便宜的建堆
        heap.clear();
        over.clear();
        over.reserve(n * 3);
        tauCur = -1;
        for (uint32_t a = 0; a < n; a++) {
            trisOf(a, Tu);
            nbrsOf(a, Tu, Nu);
            for (uint32_t b : Nu) {
                if (b < a) continue;
                uint32_t u, v; double x[3], c;
                if (plan(a, b, u, v, x, c)) over.push_back({float(c), a, b, 0, 0});
            }
        }
    }

    void trisOf(uint32_t v, std::vector<uint32_t>& out) {
        out.clear();
        uint32_t* L = &pool[aS[v]];
        uint32_t n = aN[v], w = 0;
        for (uint32_t i = 0; i < n; i++) if (!tdead[L[i]]) { L[w++] = L[i]; out.push_back(L[i]); }
        aN[v] = w;
    }

    // v 的邻点（不重复），T 是 v 现在的三角形（trisOf 的结果）。用 mark 去重，不排序
    void nbrsOf(uint32_t v, const std::vector<uint32_t>& T, std::vector<uint32_t>& out) {
        out.clear();
        uint32_t g = ++gen;
        for (uint32_t t : T)
            for (int k = 0; k < 3; k++) {
                uint32_t w = tri[t * 3 + k];
                if (w != v && mark[w] != g) { mark[w] = g; out.push_back(w); }
            }
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
        double dc = 0;  // 两端颜色差²（0–255 单位）
        if (hasCol && opt->colorWeight > 0) {
            const float* cu = &col[size_t(u) * 4];
            const float* cv = &col[size_t(v) * 4];
            for (int j = 0; j < 3; j++) dc += (double(cu[j]) - cv[j]) * (double(cu[j]) - cv[j]);
        }
        if (fl[u] & LOCKED) {
            x[0] = pu[0]; x[1] = pu[1]; x[2] = pu[2];
        } else if (dc > 32.0 * 32.0) {
            // 跨颜色边界：只能落在某个端点上，并带走它的颜色（collapse 里按投影插值，正好取到端点色），
            // 不在中间新造一个混色的点，否则黑白边界会一圈圈糊成灰色
            bool toV = q.eval(pv[0], pv[1], pv[2]) < q.eval(pu[0], pu[1], pu[2]);
            const float* p = toV ? pv : pu;
            x[0] = p[0]; x[1] = p[1]; x[2] = p[2];
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
        if (dc > 0) {
            // 颜色代价 = 权重 × 颜色差² × 边长⁴：和几何代价（面积 × 距离²）同量纲，
            // 相当于把颜色差 1（0–1 归一）的边看成偏离了 √权重 × 边长
            double e[3] = {double(pv[0]) - pu[0], double(pv[1]) - pu[1], double(pv[2]) - pu[2]};
            double L2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
            cost += opt->colorWeight * dc / (255.0 * 255.0) * L2 * L2;
        }
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
        // 连接条件：u、v 的公共邻点数必须等于共有三角形数。先标出 u 的邻点，再数 v 的邻点里有几个被标过
        nbrsOf(u, Tu, Nu);
        uint32_t gu = gen, gv = ++gen;
        int common = 0;
        for (uint32_t t : Tv)
            for (int k = 0; k < 3; k++) {
                uint32_t w = tri[t * 3 + k];
                if (w == v || w == u || mark[w] == gv) continue;
                common += mark[w] == gu;
                mark[w] = gv;
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
        nbrsOf(u, tmp, Nu);  // tmp 正好是 u 现在的三角形
        for (uint32_t w : Nu) {
            uint32_t a, b; double y[3], c;
            if (plan(u, w, a, b, y, c)) {
                push({float(c), u, w, stamp[u], stamp[w]});
            }
        }
        return shared;
    }

    // 折叠所有代价 ≤ tau 的边，直到全局预算用完。返回删掉的三角形数。
    size_t run(double tau, std::atomic<int64_t>& budget, int batch) {
        size_t removed = 0;
        int64_t pending = 0;
        bool stop = budget.load(std::memory_order_relaxed) <= 0;
        if (tau > tauCur) {
            // 新一轮：over 里过期的扔掉，够便宜的进堆（τ 只升不降，堆里剩下的仍然 ≤ τ）
            tauCur = tau;
            size_t w = 0, h0 = heap.size();
            for (const HeapE& e : over) {
                if (!valid(e)) continue;
                if (e.cost <= tauCur) heap.push_back(e);
                else over[w++] = e;
            }
            over.resize(w);
            if (heap.size() > h0) heapMake(heap);
        }
        while (!stop && !heap.empty()) {
            if (heap.front().cost > tau) break;
            HeapE e = heap.front();
            heapPop(heap);
            if (!valid(e)) continue;
            uint32_t u, v; double x[3], c;
            if (!plan(e.a, e.b, u, v, x, c)) continue;
            int r = collapse(u, v, x);
            if (!r) continue;
            removed += r;
            pending += r;
            if (pending >= batch) {
                stop = budget.fetch_sub(pending, std::memory_order_relaxed) - pending <= 0;
                gRemoved.fetch_add(pending, std::memory_order_relaxed);
                stop = stop || gCancel.load(std::memory_order_relaxed);
                pending = 0;
            }
        }
        if (pending) {
            budget.fetch_sub(pending, std::memory_order_relaxed);
            gRemoved.fetch_add(pending, std::memory_order_relaxed);
        }
        if (heap.size() > 4 * (alive * 3 / 2) + 1024) {  // 过期条目太多时清理
            std::erase_if(heap, [&](const HeapE& h) { return !valid(h); });
            heapMake(heap);
        }
        return removed;
    }
};

inline Ctx makeCtx(const Mesh& m, const DecimateOptions& o, int threads) {
    Ctx g;
    parCopy(g.P, m.P, threads);
    parCopy(g.T, m.T, threads);
    parCopy(g.G, m.G, threads);
    size_t nv = m.nv(), nt = m.nt();
    if (!m.C.empty()) {
        g.Col.resize(nv * 4);
        parallelFor(nv, threads, 65536, [&](size_t i) {
            uint32_t c = m.C[i];
            g.Col[i * 4] = float(c >> 16 & 255); g.Col[i * 4 + 1] = float(c >> 8 & 255);
            g.Col[i * 4 + 2] = float(c & 255); g.Col[i * 4 + 3] = float(c >> 24);
        });
    }
    // 顶点 → 三角形（CSR）：并行计数、并行填（原子加），再把每个顶点的列表排序，顺序和串行填的一样
    std::vector<uint32_t> start(nv + 1, 0);
    Buf<uint32_t> list(nt * 3);
    parallelFor(nt * 3, threads, 65536, [&](size_t i) { std::atomic_ref<uint32_t>(start[g.T[i] + 1]).fetch_add(1, std::memory_order_relaxed); });
    for (size_t i = 0; i < nv; i++) start[i + 1] += start[i];
    {
        Buf<uint32_t> fill(nv);
        parallelFor(nv, threads, 65536, [&](size_t v) { fill[v] = start[v]; });
        parallelFor(nt * 3, threads, 65536, [&](size_t i) {
            list[std::atomic_ref<uint32_t>(fill[g.T[i]]).fetch_add(1, std::memory_order_relaxed)] = uint32_t(i / 3);
        });
        parallelFor(nv, threads, 4096, [&](size_t v) { std::sort(list.begin() + start[v], list.begin() + start[v + 1]); });
    }
    g.Q.resize(nv);   // 下面的并行循环会写满
    g.fl.resize(nv);
    bool groups = o.groupWeight > 0 && !g.G.empty();
    bool colorEdge = o.colorEdge > 0 && !g.Col.empty();
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
            if (colorEdge) {
                // 这条边跨颜色边界：加一个法向沿边（切平面内）的平面，点往边界方向挪就有代价
                uint32_t w = ep[i].w;
                double dc = 0;
                for (int c = 0; c < 3; c++) dc += (double(g.Col[v * 4 + c]) - g.Col[w * 4 + c]) * (double(g.Col[v * 4 + c]) - g.Col[w * 4 + c]);
                if (dc > 32.0 * 32.0) {
                    const float* pw = P + size_t(w) * 3;
                    double e[3] = {double(pw[0]) - pv[0], double(pw[1]) - pv[1], double(pw[2]) - pv[2]};
                    double n[3];
                    tnorm(ep[i].t, n);
                    double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    double en = nl > 0 ? (e[0] * n[0] + e[1] * n[1] + e[2] * n[2]) / (nl * nl) : 0;
                    double a[3] = {e[0] - en * n[0], e[1] - en * n[1], e[2] - en * n[2]};
                    double al = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                    if (al > 0) {
                        for (auto& c : a) c /= al;
                        double L2 = e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
                        q.addPlane(a[0], a[1], a[2], -(a[0] * pv[0] + a[1] * pv[1] + a[2] * pv[2]), o.colorEdge * L2 * dc / (255.0 * 255.0));
                    }
                }
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
    // 抽样数随块数走（每块约 40 个点就估得准），二分 40 次都要排序，抽太多是小模型的主要固定开销
    size_t nt = g.nt(), step = std::max<size_t>(1, nt / std::clamp<size_t>(want * 40, 20000, 200000));
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
    // 这些跨遍复用（块的数组按容量复用，不用每遍释放再重新申请、重新触发缺页）
    std::vector<Part> parts;
    Buf<uint32_t> chunkOf, owner, tids, seamId;
    Buf<uint8_t> seam;

    for (int pi = 0; g.nt() > target && pi < 64; pi++) {
        if (gCancel.load()) throw Cancelled();
        auto tp = Clock::now();
        size_t nv = g.nv(), nt = g.nt();
        size_t pre = size_t(target * (1 + o.slack));
        size_t goal = threads > 1 && nt > pre ? std::max(pre, size_t(nt * o.keep)) : target;
        // ---- 切块：块数从 threads×chunksPerThread 开始，接缝点太多就少切
        // 下面每一步都按三角形 / 顶点并行，结果和线程调度无关
        chunkOf.resize(nt);
        seam.resize(nv);
        size_t nparts = 1, nseam = 0, nb = size_t(threads) * 4;
        bool single = threads == 1 || nt < 20000;
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        if (!single) {
            std::vector<float> bb(nb * 6);
            parallelBlocks(nv, threads, nb, [&](size_t b, size_t s, size_t e) {
                float l[3] = {1e30f, 1e30f, 1e30f}, h[3] = {-1e30f, -1e30f, -1e30f};
                for (size_t i = s; i < e; i++)
                    for (int j = 0; j < 3; j++) { l[j] = std::min(l[j], g.P[i * 3 + j]); h[j] = std::max(h[j], g.P[i * 3 + j]); }
                for (int j = 0; j < 3; j++) { bb[b * 6 + j] = l[j]; bb[b * 6 + 3 + j] = h[j]; }
            });
            for (size_t b = 0; b < nb; b++)
                for (int j = 0; j < 3; j++) { lo[j] = std::min(lo[j], bb[b * 6 + j]); hi[j] = std::max(hi[j], bb[b * 6 + 3 + j]); }
            owner.resize(nv);
        }
        for (size_t want = std::max(size_t(threads) * o.chunksPerThread, nt / std::max<size_t>(o.partTris, 1)); !single;) {
            double cell = pickCell(g, lo, hi, want);
            double off = (pi & 1) * 0.5 * cell;
            int dim[3];
            for (int j = 0; j < 3; j++) dim[j] = int((hi[j] - lo[j] + off) / cell) + 1;
            size_t ncell = size_t(dim[0]) * dim[1] * dim[2];
            if (ncell >= UINT32_MAX) throw std::runtime_error("切块失败：格子太多");
            // 三角形重心所在的格子；用到的格子按格子编号顺序编成块号
            std::vector<uint32_t> denseOf(ncell, UINT32_MAX);
            parallelFor(nt, threads, 65536, [&](size_t t) {
                int c[3];
                for (int j = 0; j < 3; j++) {
                    float ctr = (g.P[g.T[t * 3] * 3 + j] + g.P[g.T[t * 3 + 1] * 3 + j] + g.P[g.T[t * 3 + 2] * 3 + j]) / 3;
                    c[j] = std::clamp(int((ctr - lo[j] + off) / cell), 0, dim[j] - 1);
                }
                uint32_t k = uint32_t((size_t(c[0]) * dim[1] + c[1]) * dim[2] + c[2]);
                chunkOf[t] = k;
                std::atomic_ref<uint32_t>(denseOf[k]).store(0, std::memory_order_relaxed);
            });
            nparts = 0;
            for (auto& d : denseOf) if (d != UINT32_MAX) d = uint32_t(nparts++);
            parallelFor(nt, threads, 65536, [&](size_t t) { chunkOf[t] = denseOf[chunkOf[t]]; });
            // 接缝点：碰到两个以上块的顶点。owner 记第一个抢到的块，之后来的不是同一块就标接缝
            parallelFor(nv, threads, 65536, [&](size_t v) { owner[v] = UINT32_MAX; seam[v] = 0; });
            parallelFor(nt, threads, 65536, [&](size_t t) {
                uint32_t c = chunkOf[t];
                for (int k = 0; k < 3; k++) {
                    uint32_t v = g.T[t * 3 + k];
                    std::atomic_ref<uint32_t> ow(owner[v]);
                    uint32_t cur = ow.load(std::memory_order_relaxed);
                    if (cur == c) continue;
                    if (cur == UINT32_MAX && ow.compare_exchange_strong(cur, c, std::memory_order_relaxed)) continue;
                    if (cur != c) std::atomic_ref<uint8_t>(seam[v]).store(1, std::memory_order_relaxed);
                }
            });
            std::vector<size_t> cnt(nb, 0);
            parallelBlocks(nv, threads, nb, [&](size_t b, size_t s, size_t e) {
                size_t k = 0;
                for (size_t v = s; v < e; v++) k += seam[v];
                cnt[b] = k;
            });
            nseam = 0;
            for (size_t k : cnt) nseam += k;
            if (nseam <= o.seamShare * goal / 2) break;
            want = want * 6 / 10;
            if (want < 2) single = true;
        }
        if (single) {
            nparts = 1;
            nseam = 0;
            parallelFor(nt, threads, 65536, [&](size_t t) { chunkOf[t] = 0; });
            parallelFor(nv, threads, 65536, [&](size_t v) { seam[v] = 0; });
        }
        PassStat ps;
        ps.fromTris = nt; ps.goal = goal; ps.parts = nparts; ps.seamVerts = nseam;
        // 按块号做计数排序（分段计数再并行填），每块里的三角形仍按编号从小到大
        std::vector<uint32_t> pstart(nparts + 1, 0);
        tids.resize(nt);
        {
            std::vector<uint32_t> cnt(nb * nparts, 0);
            parallelBlocks(nt, threads, nb, [&](size_t b, size_t s, size_t e) {
                uint32_t* c = &cnt[b * nparts];
                for (size_t t = s; t < e; t++) c[chunkOf[t]]++;
            });
            uint32_t run = 0;
            for (size_t p = 0; p < nparts; p++) {
                pstart[p] = run;
                for (size_t b = 0; b < nb; b++) { uint32_t k = cnt[b * nparts + p]; cnt[b * nparts + p] = run; run += k; }
            }
            pstart[nparts] = run;
            parallelBlocks(nt, threads, nb, [&](size_t b, size_t s, size_t e) {
                uint32_t* c = &cnt[b * nparts];
                for (size_t t = s; t < e; t++) tids[c[chunkOf[t]]++] = uint32_t(t);
            });
        }
        // 大块先做
        std::vector<uint32_t> order(nparts);
        for (size_t i = 0; i < nparts; i++) order[i] = uint32_t(i);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return pstart[a + 1] - pstart[a] > pstart[b + 1] - pstart[b]; });
        ps.cut = secs(tp);
        auto tb = Clock::now();
        if (parts.size() < nparts) parts.resize(nparts);
        parallelFor(nparts, threads, 1, [&](size_t i) {
            uint32_t p = order[i];
            parts[p].opt = &o;
            parts[p].build(g, &tids[pstart[p]], pstart[p + 1] - pstart[p], seam);
        });
        ps.build = secs(tb);
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
                for (size_t pi2 = 0; pi2 < nparts; pi2++) {
                    const Part& p = parts[pi2];
                    size_t nh = p.heap.size(), n = nh + p.over.size(), step = std::max<size_t>(1, n / per);
                    for (size_t i = 0; i < n; i += step) smp.push_back(i < nh ? p.heap[i].cost : p.over[i - nh].cost);
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
            if (gCancel.load()) throw Cancelled();
            cur -= removed;
            ps.rounds++;
            if (removed == 0) { tau *= 8; if (++stuck > 8) break; }
            else stuck = 0;
            if (single) break;
        }
        ps.time = secs(tp);
        // ---- 合并成新的全局网格
        auto tm = Clock::now();
        seamId.resize(nv);  // 只有接缝点的位置有意义
        uint32_t ns = 0;
        for (size_t v = 0; v < nv; v++) if (seam[v]) seamId[v] = ns++;
        std::vector<size_t> vOff(nparts + 1, ns), tOff(nparts + 1, 0);
        parallelFor(nparts, threads, 1, [&](size_t p) {
            size_t a = 0;
            for (size_t i = 0; i < parts[p].gid.size(); i++) a += !(parts[p].fl[i] & DEAD) && !seam[parts[p].gid[i]];
            vOff[p + 1] = a;
        });
        for (size_t p = 0; p < nparts; p++) {
            vOff[p + 1] += vOff[p];
            tOff[p + 1] = tOff[p] + parts[p].alive;
        }
        Ctx h;
        size_t NV = vOff[nparts], NT = tOff[nparts];
        h.P.resize(NV * 3); h.Q.resize(NV); h.fl.resize(NV); h.T.resize(NT * 3);
        if (!g.Col.empty()) h.Col.resize(NV * 4);
        if (!g.G.empty()) h.G.resize(NT);
        parallelFor(nv, threads, 65536, [&](size_t v) {
            if (!seam[v]) return;
            uint32_t s = seamId[v];
            memcpy(&h.P[s * 3], &g.P[v * 3], 12);
            if (!g.Col.empty()) memcpy(&h.Col[s * 4], &g.Col[v * 4], 16);
            h.Q[s] = g.Q[v];
            h.fl[s] = g.fl[v];
        });
        // 接缝点的误差累积：各块里的增量加回去（并行算增量，再按块号顺序加，和串行的加法顺序一样）
        std::vector<std::vector<std::pair<uint32_t, Quadric>>> dq(nparts);
        parallelFor(nparts, threads, 1, [&](size_t p) {
            for (size_t i = 0; i < parts[p].gid.size(); i++) {
                uint32_t v = parts[p].gid[i];
                if (seam[v]) dq[p].push_back({seamId[v], parts[p].Q[i] - g.Q[v]});
            }
        });
        for (auto& L : dq)
            for (auto& [s, d] : L) h.Q[s] += d;
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
        });
        std::swap(g, h);
        if (o.fastExit) keepUntilExit(std::move(h));  // 旧网格（第一遍时有几 GB）不在这里释放
        ps.merge = secs(tm);
        st.passes.push_back(ps);
    }

    // 输出：去掉没被用到的顶点（并行：标出用到的点，按原来的编号顺序编新号）
    Mesh out;
    size_t gnv = g.nv(), nb = size_t(threads) * 4;
    Buf<uint32_t> remap(gnv);
    parallelFor(gnv, threads, 65536, [&](size_t v) { remap[v] = 0; });
    parallelFor(g.T.size(), threads, 65536, [&](size_t i) { std::atomic_ref<uint32_t>(remap[g.T[i]]).store(1, std::memory_order_relaxed); });
    std::vector<size_t> used(nb + 1, 0);
    parallelBlocks(gnv, threads, nb, [&](size_t b, size_t s, size_t e) {
        size_t k = 0;
        for (size_t v = s; v < e; v++) k += remap[v];
        used[b + 1] = k;
    });
    for (size_t b = 0; b < nb; b++) used[b + 1] += used[b];
    out.P.resize(used[nb] * 3);
    if (!g.Col.empty()) out.C.resize(used[nb]);
    parallelBlocks(gnv, threads, nb, [&](size_t b, size_t s, size_t e) {
        uint32_t k = uint32_t(used[b]);
        for (size_t v = s; v < e; v++) {
            if (!remap[v]) { remap[v] = UINT32_MAX; continue; }
            remap[v] = k;
            memcpy(&out.P[size_t(k) * 3], &g.P[v * 3], 12);
            if (!g.Col.empty()) {
                auto c8 = [&](int j) { return uint32_t(std::clamp(int(std::lround(g.Col[v * 4 + j])), 0, 255)); };
                out.C[k] = c8(3) << 24 | c8(0) << 16 | c8(1) << 8 | c8(2);
            }
            k++;
        }
    });
    out.T.resize(g.T.size());
    parallelFor(g.T.size(), threads, 65536, [&](size_t i) { out.T[i] = remap[g.T[i]]; });
    out.G.assign(g.G.begin(), g.G.end());
    out.gozHead = in.gozHead;
    if (o.fastExit) {
        keepUntilExit(std::move(parts));
        keepUntilExit(std::move(chunkOf)); keepUntilExit(std::move(owner)); keepUntilExit(std::move(tids));
        keepUntilExit(std::move(seamId)); keepUntilExit(std::move(seam)); keepUntilExit(std::move(g));
    }
    return out;
}
