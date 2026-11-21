// 网格读写：OBJ（含 ZBrush #MRGB Polypaint、g 组）、GoZ（ZBrush 二进制）、二进制 STL。
// 只保留减面需要的东西：顶点、三角形、逐顶点颜色、逐面组。四边形和多边形按扇形拆成三角形。
#pragma once
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct Mesh {
    std::vector<float> P;     // 顶点 xyz
    std::vector<uint32_t> T;  // 三角形索引
    std::vector<uint32_t> C;  // 逐顶点颜色，ZBrush MRGB 打包 0xMMRRGGBB（可空）
    std::vector<uint16_t> M;  // 逐顶点 ZBrush Mask16：65535 = 无遮罩，0 = 完全遮罩（全为 65535 时清空）
    std::vector<float> UV;    // 逐三角形角 UV：nt × 6（u0,v0,u1,v1,u2,v2），可空
    std::vector<uint16_t> G;  // 逐三角形 polygroup（可空）
    std::string gozHead;      // 从 GoZ 读入时：文件头 + 名字/标志等非几何记录，写回时原样带上
    size_t nv() const { return P.size() / 3; }
    size_t nt() const { return T.size() / 3; }
};

inline std::string toUtf8(const std::wstring& w) {
    std::string s;
    for (size_t i = 0; i < w.size(); i++) {
        uint32_t c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < w.size()) c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
        if (c < 0x80) s += char(c);
        else if (c < 0x800) { s += char(0xC0 | c >> 6); s += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { s += char(0xE0 | c >> 12); s += char(0x80 | (c >> 6 & 63)); s += char(0x80 | (c & 63)); }
        else { s += char(0xF0 | c >> 18); s += char(0x80 | (c >> 12 & 63)); s += char(0x80 | (c >> 6 & 63)); s += char(0x80 | (c & 63)); }
    }
    return s;
}

// 常驻轻量线程池：用 std::atomic::wait（Windows WaitOnAddress）挂起空闲线程，避免每次并行循环反复创建/销毁数十个系统线程
struct ThreadPool {
    int maxT = int(std::max(1u, std::thread::hardware_concurrency()));
    std::atomic<uint32_t> gen{0};
    std::atomic<uint32_t> left{0};
    std::atomic<size_t> next{0};
    size_t total = 0, step = 1;
    int useT = 1;
    void (*fn)(void*, size_t, size_t) = nullptr;
    void* ctx = nullptr;

    ThreadPool() {
        for (int id = 1; id < maxT; id++) {
            std::thread([this, id] {
                uint32_t g = 0;
                for (;;) {
                    gen.wait(g, std::memory_order_acquire);
                    g = gen.load(std::memory_order_acquire);
                    if (id < useT) work();
                    if (left.fetch_sub(1, std::memory_order_acq_rel) == 1) left.notify_one();
                }
            }).detach();
        }
    }
    void work() {
        for (;;) {
            size_t s = next.fetch_add(step, std::memory_order_relaxed);
            if (s >= total) break;
            fn(ctx, s, std::min(total, s + step));
        }
    }
    template <class F>
    void run(size_t n, int threads, size_t grain, F&& f) {
        static thread_local bool busy = false;
        int t = std::clamp(threads, 1, maxT);
        if (busy || t <= 1 || n <= grain) {
            for (size_t i = 0; i < n; i++) f(i);
            return;
        }
        busy = true;
        using Fn = std::decay_t<F>;
        auto body = [](void* p, size_t s, size_t e) {
            auto& func = *static_cast<Fn*>(p);
            for (size_t i = s; i < e; i++) func(i);
        };
        next.store(0, std::memory_order_relaxed);
        total = n;
        step = grain;
        useT = t;
        fn = body;
        ctx = const_cast<void*>(static_cast<const void*>(&f));
        if (maxT > 1) {
            left.store(uint32_t(maxT - 1), std::memory_order_release);
            gen.fetch_add(1, std::memory_order_acq_rel);
            gen.notify_all();
        }
        work();
        if (maxT > 1) {
            uint32_t cur;
            while ((cur = left.load(std::memory_order_acquire)) != 0) left.wait(cur, std::memory_order_acquire);
        }
        busy = false;
    }
};

inline ThreadPool& globalPool() {
    static ThreadPool* p = new ThreadPool();
    return *p;
}

// 把 [0, n) 分成 nb 段并行处理 f(段号, 起, 止)：读写大模型用。段数固定，结果和线程调度无关
template <class F>
void ioBlocks(size_t n, size_t nb, F&& f) {
    int T = int(std::min<size_t>(nb, std::max(1u, std::thread::hardware_concurrency())));
    globalPool().run(nb, T, 1, [&](size_t b) { f(b, n * b / nb, n * (b + 1) / nb); });
}

inline std::vector<char> readAll(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) throw std::runtime_error("打不开文件：" + toUtf8(path));
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); throw std::runtime_error("读取失败：" + toUtf8(path)); }
    int64_t n = _ftelli64(f);
    if (n < 0 || _fseeki64(f, 0, SEEK_SET) != 0) { fclose(f); throw std::runtime_error("读取失败：" + toUtf8(path)); }
    std::vector<char> b(size_t(n) + 1);
    if (n && fread(b.data(), 1, size_t(n), f) != size_t(n)) { fclose(f); throw std::runtime_error("读取失败：" + toUtf8(path)); }
    fclose(f);
    b[size_t(n)] = 0;
    b.resize(size_t(n));
    return b;
}

inline void writeAll(const std::wstring& path, const std::string& s) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) throw std::runtime_error("写不了文件：" + toUtf8(path));
    bool ok = s.empty() || fwrite(s.data(), 1, s.size(), f) == s.size();
    if (fclose(f) != 0) ok = false;
    if (!ok) throw std::runtime_error("写入失败（磁盘可能已满）：" + toUtf8(path));
}

inline std::wstring lowerExt(const std::wstring& p) {
    auto d = p.find_last_of(L'.');
    std::wstring e = d == std::wstring::npos ? L"" : p.substr(d);
    for (auto& c : e) c = towlower(c);
    return e;
}

// ---------------------------------------------------------------- OBJ
inline Mesh readObj(const std::vector<char>& buf) {
    Mesh m;
    const char* p = buf.data();
    const char* e = p + buf.size();
    std::unordered_map<std::string, uint16_t> groups;
    uint16_t curGroup = 0;
    bool anyGroup = false, anyUV = false;
    std::vector<float> vt;
    std::vector<int64_t> poly, polyUV;
    auto skipSp = [&] { while (p < e && (*p == ' ' || *p == '\t')) p++; };
    auto eol = [&] { while (p < e && *p != '\n') p++; if (p < e) p++; };
    while (p < e) {
        if (p[0] == 'v' && p + 1 < e && (p[1] == ' ' || p[1] == '\t')) {
            p += 2;
            for (int k = 0; k < 3; k++) {
                skipSp();
                float x = 0;
                auto r = std::from_chars(p, e, x);
                p = r.ptr;
                m.P.push_back(x);
            }
            eol();
        } else if (p[0] == 'v' && p + 2 < e && p[1] == 't' && (p[2] == ' ' || p[2] == '\t')) {
            p += 3;
            float u = 0, v = 0;
            skipSp();
            auto r1 = std::from_chars(p, e, u); p = r1.ptr;
            skipSp();
            auto r2 = std::from_chars(p, e, v); p = r2.ptr;
            vt.push_back(u);
            vt.push_back(v);
            eol();
        } else if (p[0] == 'f' && p + 1 < e && (p[1] == ' ' || p[1] == '\t')) {
            p += 2;
            poly.clear();
            polyUV.clear();
            size_t nvt = vt.size() / 2;
            for (;;) {
                skipSp();
                if (p >= e || *p == '\n' || *p == '\r' || *p == '#') break;
                int64_t i = 0;
                auto r = std::from_chars(p, e, i);
                if (r.ptr == p) break;
                p = r.ptr;
                poly.push_back(i < 0 ? int64_t(m.nv()) + i : i - 1);
                int64_t ti = -1;
                if (p < e && *p == '/') {
                    p++;
                    if (p < e && *p != '/' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
                        int64_t j = 0;
                        auto r2 = std::from_chars(p, e, j);
                        if (r2.ptr != p) {
                            p = r2.ptr;
                            ti = j < 0 ? int64_t(nvt) + j : j - 1;
                        }
                    }
                }
                polyUV.push_back(ti);
                while (p < e && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;  // 跳过 /vn
            }
            size_t nv = m.nv();
            for (size_t k = 2; k < poly.size(); k++) {
                int64_t a = poly[0], b = poly[k - 1], c = poly[k];
                if (a < 0 || b < 0 || c < 0 || size_t(a) >= nv || size_t(b) >= nv || size_t(c) >= nv) continue;
                if (a == b || b == c || a == c) continue;
                m.T.push_back(uint32_t(a));
                m.T.push_back(uint32_t(b));
                m.T.push_back(uint32_t(c));
                m.G.push_back(curGroup);
                int64_t ta = polyUV[0], tb = polyUV[k - 1], tc = polyUV[k];
                bool okUV = ta >= 0 && tb >= 0 && tc >= 0 && size_t(ta) < nvt && size_t(tb) < nvt && size_t(tc) < nvt;
                if (okUV) {
                    if (!anyUV) { anyUV = true; m.UV.resize((m.nt() - 1) * 6, 0.0f); }
                    m.UV.insert(m.UV.end(), {vt[size_t(ta) * 2], vt[size_t(ta) * 2 + 1],
                                             vt[size_t(tb) * 2], vt[size_t(tb) * 2 + 1],
                                             vt[size_t(tc) * 2], vt[size_t(tc) * 2 + 1]});
                } else if (anyUV) {
                    m.UV.insert(m.UV.end(), {0.f, 0.f, 0.f, 0.f, 0.f, 0.f});
                }
            }
            eol();
        } else if (p[0] == '#' && e - p > 6 && memcmp(p, "#MRGB ", 6) == 0) {
            p += 6;
            while (p + 8 <= e && isxdigit((unsigned char)*p)) {
                uint32_t v = 0;
                std::from_chars(p, p + 8, v, 16);
                m.C.push_back(v);
                p += 8;
            }
            eol();
        } else if (p[0] == 'g' && p + 1 < e && (p[1] == ' ' || p[1] == '\t')) {
            p += 2;
            skipSp();
            const char* s = p;
            while (p < e && *p != '\n' && *p != '\r') p++;
            auto it = groups.emplace(std::string(s, p), uint16_t(groups.size())).first;
            curGroup = it->second;
            anyGroup = true;
            eol();
        } else {
            eol();
        }
    }
    if (!anyGroup) m.G.clear();
    if (!anyUV) m.UV.clear();
    if (m.C.size() != m.nv()) m.C.clear();
    return m;
}

inline void appendFloat(std::string& s, float x) {
    char b[32];
    auto r = std::to_chars(b, b + 32, x);
    s.append(b, r.ptr);
}

inline std::string writeObj(const Mesh& m) {
    std::string s;
    bool hasUV = !m.UV.empty() && m.UV.size() == m.nt() * 6;
    s.reserve(m.nv() * 36 + m.nt() * (hasUV ? 46 : 30) + m.C.size() * 9);
    s += "# KMDecimate\n";
    for (size_t i = 0; i < m.nv(); i++) {
        s += "v ";
        appendFloat(s, m.P[i * 3]); s += ' ';
        appendFloat(s, m.P[i * 3 + 1]); s += ' ';
        appendFloat(s, m.P[i * 3 + 2]); s += '\n';
    }
    std::vector<uint32_t> uvIdx;
    if (hasUV) {
        std::unordered_map<uint64_t, uint32_t> uvm;
        uvm.reserve(m.nv() * 2);
        uvIdx.resize(m.nt() * 3);
        uint32_t nextUV = 1;
        for (size_t i = 0; i < m.nt() * 3; i++) {
            float u = m.UV[i * 2], v = m.UV[i * 2 + 1];
            uint32_t bu = 0, bv = 0;
            memcpy(&bu, &u, 4); memcpy(&bv, &v, 4);
            if (bu == 0x80000000u) bu = 0;
            if (bv == 0x80000000u) bv = 0;
            uint64_t key = uint64_t(bu) << 32 | bv;
            auto [it, ins] = uvm.emplace(key, nextUV);
            if (ins) {
                nextUV++;
                s += "vt ";
                appendFloat(s, u); s += ' ';
                appendFloat(s, v); s += '\n';
            }
            uvIdx[i] = it->second;
        }
    }
    if (!m.C.empty()) {
        char hex[9];
        for (size_t i = 0; i < m.C.size(); i += 64) {
            s += "#MRGB ";
            for (size_t k = i; k < std::min(m.C.size(), i + 64); k++) { snprintf(hex, 9, "%08x", m.C[k]); s.append(hex, 8); }
            s += '\n';
        }
    }
    int lastG = -1;
    char b[16];
    for (size_t t = 0; t < m.nt(); t++) {
        if (!m.G.empty() && m.G[t] != lastG) {
            lastG = m.G[t];
            s += "g group_";
            s += std::to_string(lastG);
            s += '\n';
        }
        s += 'f';
        for (int k = 0; k < 3; k++) {
            s += ' ';
            auto r = std::to_chars(b, b + 16, m.T[t * 3 + k] + 1);
            s.append(b, r.ptr);
            if (hasUV) {
                s += '/';
                auto r2 = std::to_chars(b, b + 16, uvIdx[t * 3 + k]);
                s.append(b, r2.ptr);
            }
        }
        s += '\n';
    }
    return s;
}

// ---------------------------------------------------------------- GoZ
namespace goz {
constexpr uint32_t Name = 1, Flags = 5001, Points = 10001, Faces = 20001, Uv4 = 25001, Mask16 = 30002, Mrgb = 35001, Groups = 40001;
}

inline void dropEmptyMask(Mesh& m) {
    if (m.M.size() != m.nv()) { m.M.clear(); return; }
    std::atomic<bool> any{false};
    ioBlocks(m.M.size(), 64, [&](size_t, size_t s, size_t e) {
        for (size_t i = s; i < e; i++)
            if (m.M[i] < 65535) { any.store(true, std::memory_order_relaxed); break; }
    });
    if (!any.load()) m.M.clear();
}

inline void validateMeshIndices(const Mesh& m) {
    size_t nv = m.nv();
    std::atomic<bool> bad{false};
    ioBlocks(m.T.size(), 64, [&](size_t, size_t s, size_t e) {
        for (size_t i = s; i < e; i++)
            if (m.T[i] >= nv) { bad.store(true, std::memory_order_relaxed); break; }
    });
    if (bad.load()) throw std::runtime_error("网格面索引越界（文件可能已损坏）");
}

inline Mesh readGoz(const std::vector<char>& d) {
    Mesh m;
    if (d.size() < 32 || memcmp(d.data(), "GoZb", 4) != 0) throw std::runtime_error("不是 GoZ 文件");
    size_t pos = 32;
    m.gozHead.assign(d.data(), 32);
    std::vector<uint16_t> faceGroups;
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, d.data() + o, 4); return v; };
    auto u64 = [&](size_t o) { uint64_t v; memcpy(&v, d.data() + o, 8); return v; };
    std::vector<uint8_t> trisPerFace;          // 每个面拆成几个三角形（1 或 2）
    const size_t fb = 256;                     // 面按段并行处理，fOff[段] = 这段第一个三角形的序号
    std::vector<size_t> fOff(fb + 1, 0);
    while (pos + 8 <= d.size()) {
        uint32_t tag = u32(pos), size = u32(pos + 4);
        if (tag == 0 || size < 8) break;
        if (size > d.size() - pos) throw std::runtime_error("GoZ 文件不完整（数据块越界）");
        size_t body = pos + 8, payload = size - 8;
        if (tag == goz::Points) {
            if (payload < 8) throw std::runtime_error("GoZ 顶点块损坏");
            uint64_t n = u64(body);
            if (n > (payload - 8) / 12) throw std::runtime_error("GoZ 顶点块越界");
            m.P.resize(size_t(n) * 3);
            if (n) memcpy(m.P.data(), d.data() + body + 8, size_t(n) * 12);
        } else if (tag == goz::Faces) {
            if (payload < 8) throw std::runtime_error("GoZ 面块损坏");
            uint64_t n = u64(body);
            if (n > (payload - 8) / 16) throw std::runtime_error("GoZ 面块越界");
            const char* f = d.data() + body + 8;
            // 四边形拆成两个三角形。并行：分段数三角形，前缀和定位，再并行写
            trisPerFace.resize(size_t(n));
            fOff.assign(fb + 1, 0);
            ioBlocks(size_t(n), fb, [&](size_t b, size_t s, size_t e) {
                size_t k = 0;
                for (size_t i = s; i < e; i++) {
                    uint32_t q3;
                    memcpy(&q3, f + i * 16 + 12, 4);
                    trisPerFace[i] = q3 != 0xFFFFFFFFu ? 2 : 1;
                    k += trisPerFace[i];
                }
                fOff[b + 1] = k;
            });
            for (size_t b = 0; b < fb; b++) fOff[b + 1] += fOff[b];
            m.T.resize(fOff[fb] * 3);
            ioBlocks(size_t(n), fb, [&](size_t b, size_t s, size_t e) {
                uint32_t* t = m.T.data() + fOff[b] * 3;
                for (size_t i = s; i < e; i++) {
                    uint32_t q[4];
                    memcpy(q, f + i * 16, 16);
                    *t++ = q[0]; *t++ = q[1]; *t++ = q[2];
                    if (trisPerFace[i] == 2) { *t++ = q[0]; *t++ = q[2]; *t++ = q[3]; }
                }
            });
        } else if (tag == goz::Uv4) {
            if (payload < 8) throw std::runtime_error("GoZ UV 块损坏");
            uint64_t n = u64(body);
            if (n > (payload - 8) / 32) throw std::runtime_error("GoZ UV 块越界");
            if (n == trisPerFace.size() && n > 0) {
                const char* uvp = d.data() + body + 8;
                m.UV.resize(m.nt() * 6);
                ioBlocks(size_t(n), fb, [&](size_t b, size_t s, size_t e) {
                    float* dst = m.UV.data() + fOff[b] * 6;
                    for (size_t i = s; i < e; i++) {
                        float u[8];
                        memcpy(u, uvp + i * 32, 32);
                        dst[0] = u[0]; dst[1] = u[1]; dst[2] = u[2]; dst[3] = u[3]; dst[4] = u[4]; dst[5] = u[5];
                        dst += 6;
                        if (trisPerFace[i] == 2) {
                            dst[0] = u[0]; dst[1] = u[1]; dst[2] = u[4]; dst[3] = u[5]; dst[4] = u[6]; dst[5] = u[7];
                            dst += 6;
                        }
                    }
                });
            }
        } else if (tag == goz::Mask16) {
            if (payload < 8) throw std::runtime_error("GoZ 遮罩块损坏");
            uint64_t n = u64(body);
            if (n > (payload - 8) / 2) throw std::runtime_error("GoZ 遮罩块越界");
            m.M.resize(size_t(n));
            if (n) memcpy(m.M.data(), d.data() + body + 8, size_t(n) * 2);
        } else if (tag == goz::Mrgb) {
            if (payload < 8) throw std::runtime_error("GoZ 顶点色块损坏");
            uint32_t n = u32(body);
            if (size_t(n) > (payload - 8) / 4) throw std::runtime_error("GoZ 顶点色块越界");
            const unsigned char* c = (const unsigned char*)d.data() + body + 8;
            m.C.resize(n);
            ioBlocks(n, 64, [&](size_t, size_t s, size_t e) {
                for (size_t i = s; i < e; i++)  // 文件里是 B G R M
                    m.C[i] = uint32_t(c[i * 4 + 3]) << 24 | uint32_t(c[i * 4 + 2]) << 16 | uint32_t(c[i * 4 + 1]) << 8 | c[i * 4];
            });
        } else if (tag == goz::Groups) {
            if (payload < 8) throw std::runtime_error("GoZ 组块损坏");
            uint64_t n = u64(body);
            if (n > (payload - 8) / 2) throw std::runtime_error("GoZ 组块越界");
            faceGroups.resize(size_t(n));
            if (n) memcpy(faceGroups.data(), d.data() + body + 8, size_t(n) * 2);
        } else if (tag < goz::Points) {
            m.gozHead.append(d.data() + pos, size);
        }
        pos += size;
    }
    if (!faceGroups.empty() && faceGroups.size() == trisPerFace.size()) {
        m.G.resize(m.nt());
        ioBlocks(trisPerFace.size(), fb, [&](size_t b, size_t s, size_t e) {
            uint16_t* gp = m.G.data() + fOff[b];
            for (size_t i = s; i < e; i++) {
                *gp++ = faceGroups[i];
                if (trisPerFace[i] == 2) *gp++ = faceGroups[i];
            }
        });
    }
    if (m.C.size() != m.nv()) m.C.clear();
    if (!m.M.empty()) dropEmptyMask(m);
    if (!m.T.empty()) validateMeshIndices(m);
    return m;
}

// 流式读 GoZ：不把整个几百 MB–1 GB 文件整块缓存在内存里，逐块直接读进 Mesh，降低加载峰值内存
inline Mesh readGozFile(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) throw std::runtime_error("打不开文件：" + toUtf8(path));
    struct Closer { FILE* fp; ~Closer() { if (fp) fclose(fp); } } cl{f};
    if (_fseeki64(f, 0, SEEK_END) != 0) throw std::runtime_error("读取失败：" + toUtf8(path));
    int64_t fileLen = _ftelli64(f);
    if (fileLen < 32 || _fseeki64(f, 0, SEEK_SET) != 0) throw std::runtime_error("不是 GoZ 文件");
    Mesh m;
    char hdr[32];
    if (fread(hdr, 1, 32, f) != 32 || memcmp(hdr, "GoZb", 4) != 0) throw std::runtime_error("不是 GoZ 文件");
    m.gozHead.assign(hdr, 32);
    std::vector<uint16_t> faceGroups;
    std::vector<uint8_t> trisPerFace;
    std::vector<size_t> fOff;
    constexpr size_t CHUNK_FACES = 1 << 20;        // 超大模型（>512 MB 面块）分块流式回退用
    constexpr size_t SINGLE_PASS_FACES = 32 << 20; // 3200 万四边面（512 MB）以内走单遍快读，读完立即释放临时面块
    constexpr size_t CHUNK_MRGB = 4 << 20;         // 16 MB 临时缓冲
    int64_t pos = 32;
    auto readExact = [&](void* dst, size_t n) {
        if (n && fread(dst, 1, n, f) != n) throw std::runtime_error("GoZ 读取失败：" + toUtf8(path));
    };
    while (pos + 8 <= fileLen) {
        if (_fseeki64(f, pos, SEEK_SET) != 0) throw std::runtime_error("GoZ 定位失败");
        uint32_t th[2];
        readExact(th, 8);
        uint32_t tag = th[0], size = th[1];
        if (tag == 0 || size < 8) break;
        if (int64_t(size) > fileLen - pos) throw std::runtime_error("GoZ 文件不完整（数据块越界）");
        size_t payload = size - 8;
        if (tag == goz::Points) {
            if (payload < 8) throw std::runtime_error("GoZ 顶点块损坏");
            uint64_t n = 0;
            readExact(&n, 8);
            if (n > (payload - 8) / 12) throw std::runtime_error("GoZ 顶点块越界");
            m.P.resize(size_t(n) * 3);
            readExact(m.P.data(), size_t(n) * 12);
        } else if (tag == goz::Faces) {
            if (payload < 8) throw std::runtime_error("GoZ 面块损坏");
            uint64_t n64 = 0;
            readExact(&n64, 8);
            if (n64 > (payload - 8) / 16) throw std::runtime_error("GoZ 面块越界");
            size_t n = size_t(n64);
            int64_t dataPos = pos + 16;
            trisPerFace.resize(n);
            if (n <= SINGLE_PASS_FACES) {
                // 单遍快路径：一次顺序读入 Faces，并行统计 + 并行拆包，随后立即释放 fbuf（加载总内存远低于减面期峰值）
                std::vector<uint32_t> fbuf(n * 4);
                readExact(fbuf.data(), n * 16);
                const size_t fb = 256;
                fOff.assign(fb + 1, 0);
                ioBlocks(n, fb, [&](size_t b, size_t a, size_t e) {
                    size_t k = 0;
                    for (size_t i = a; i < e; i++) {
                        uint8_t nt = fbuf[i * 4 + 3] != 0xFFFFFFFFu ? 2 : 1;
                        trisPerFace[i] = nt;
                        k += nt;
                    }
                    fOff[b + 1] = k;
                });
                for (size_t b = 0; b < fb; b++) fOff[b + 1] += fOff[b];
                m.T.resize(fOff[fb] * 3);
                ioBlocks(n, fb, [&](size_t b, size_t a, size_t e) {
                    uint32_t* t = m.T.data() + fOff[b] * 3;
                    for (size_t i = a; i < e; i++) {
                        const uint32_t* q = &fbuf[i * 4];
                        *t++ = q[0]; *t++ = q[1]; *t++ = q[2];
                        if (trisPerFace[i] == 2) { *t++ = q[0]; *t++ = q[2]; *t++ = q[3]; }
                    }
                });
            } else {
                size_t nblk = (n + CHUNK_FACES - 1) / CHUNK_FACES;
                fOff.assign(nblk + 1, 0);
                std::vector<uint32_t> fbuf(std::min(n, CHUNK_FACES) * 4);
                for (size_t b = 0; b < nblk; b++) {
                    size_t s = b * CHUNK_FACES, cnt = std::min(CHUNK_FACES, n - s);
                    readExact(fbuf.data(), cnt * 16);
                    std::atomic<size_t> blkTris{0};
                    ioBlocks(cnt, 32, [&](size_t, size_t a, size_t e) {
                        size_t k = 0;
                        for (size_t i = a; i < e; i++) {
                            uint8_t nt = fbuf[i * 4 + 3] != 0xFFFFFFFFu ? 2 : 1;
                            trisPerFace[s + i] = nt;
                            k += nt;
                        }
                        blkTris.fetch_add(k, std::memory_order_relaxed);
                    });
                    fOff[b + 1] = fOff[b] + blkTris.load();
                }
                m.T.resize(fOff[nblk] * 3);
                if (_fseeki64(f, dataPos, SEEK_SET) != 0) throw std::runtime_error("GoZ 定位失败");
                const size_t sb = 32;
                std::vector<size_t> subOff(sb + 1, 0);
                for (size_t b = 0; b < nblk; b++) {
                    size_t s = b * CHUNK_FACES, cnt = std::min(CHUNK_FACES, n - s);
                    readExact(fbuf.data(), cnt * 16);
                    subOff.assign(sb + 1, 0);
                    ioBlocks(cnt, sb, [&](size_t k, size_t a, size_t e) {
                        size_t c = 0;
                        for (size_t i = a; i < e; i++) c += trisPerFace[s + i];
                        subOff[k + 1] = c;
                    });
                    for (size_t k = 0; k < sb; k++) subOff[k + 1] += subOff[k];
                    size_t base = fOff[b];
                    ioBlocks(cnt, sb, [&](size_t k, size_t a, size_t e) {
                        uint32_t* t = m.T.data() + (base + subOff[k]) * 3;
                        for (size_t i = a; i < e; i++) {
                            const uint32_t* q = &fbuf[i * 4];
                            *t++ = q[0]; *t++ = q[1]; *t++ = q[2];
                            if (trisPerFace[s + i] == 2) { *t++ = q[0]; *t++ = q[2]; *t++ = q[3]; }
                        }
                    });
                }
            }
        } else if (tag == goz::Uv4) {
            if (payload < 8) throw std::runtime_error("GoZ UV 块损坏");
            uint64_t n64 = 0;
            readExact(&n64, 8);
            if (n64 > (payload - 8) / 32) throw std::runtime_error("GoZ UV 块越界");
            size_t n = size_t(n64);
            if (n == trisPerFace.size() && n > 0) {
                m.UV.resize(m.nt() * 6);
                constexpr size_t CHUNK_UV = 512 * 1024;  // 52 万面 = 16 MB 临时缓冲
                std::vector<float> ubuf(std::min(n, CHUNK_UV) * 8);
                size_t triBase = 0;
                const size_t sb = 32;
                std::vector<size_t> subOff(sb + 1, 0);
                for (size_t s = 0; s < n; s += CHUNK_UV) {
                    size_t cnt = std::min(CHUNK_UV, n - s);
                    readExact(ubuf.data(), cnt * 32);
                    subOff.assign(sb + 1, 0);
                    ioBlocks(cnt, sb, [&](size_t k, size_t a, size_t e) {
                        size_t c = 0;
                        for (size_t i = a; i < e; i++) c += trisPerFace[s + i];
                        subOff[k + 1] = c;
                    });
                    for (size_t k = 0; k < sb; k++) subOff[k + 1] += subOff[k];
                    ioBlocks(cnt, sb, [&](size_t k, size_t a, size_t e) {
                        float* dst = m.UV.data() + (triBase + subOff[k]) * 6;
                        for (size_t i = a; i < e; i++) {
                            const float* u = &ubuf[i * 8];
                            dst[0] = u[0]; dst[1] = u[1]; dst[2] = u[2]; dst[3] = u[3]; dst[4] = u[4]; dst[5] = u[5];
                            dst += 6;
                            if (trisPerFace[s + i] == 2) {
                                dst[0] = u[0]; dst[1] = u[1]; dst[2] = u[4]; dst[3] = u[5]; dst[4] = u[6]; dst[5] = u[7];
                                dst += 6;
                            }
                        }
                    });
                    triBase += subOff[sb];
                }
            }
        } else if (tag == goz::Mask16) {
            if (payload < 8) throw std::runtime_error("GoZ 遮罩块损坏");
            uint64_t n = 0;
            readExact(&n, 8);
            if (n > (payload - 8) / 2) throw std::runtime_error("GoZ 遮罩块越界");
            m.M.resize(size_t(n));
            readExact(m.M.data(), size_t(n) * 2);
        } else if (tag == goz::Mrgb) {
            if (payload < 8) throw std::runtime_error("GoZ 顶点色块损坏");
            uint32_t mh[2];
            readExact(mh, 8);
            size_t n = mh[0];
            if (n > (payload - 8) / 4) throw std::runtime_error("GoZ 顶点色块越界");
            m.C.resize(n);
            std::vector<unsigned char> cbuf(std::min(n, CHUNK_MRGB) * 4);
            for (size_t s = 0; s < n; s += CHUNK_MRGB) {
                size_t cnt = std::min(CHUNK_MRGB, n - s);
                readExact(cbuf.data(), cnt * 4);
                ioBlocks(cnt, 32, [&](size_t, size_t a, size_t e) {
                    const unsigned char* c = cbuf.data();
                    for (size_t i = a; i < e; i++)
                        m.C[s + i] = uint32_t(c[i * 4 + 3]) << 24 | uint32_t(c[i * 4 + 2]) << 16 | uint32_t(c[i * 4 + 1]) << 8 | c[i * 4];
                });
            }
        } else if (tag == goz::Groups) {
            if (payload < 8) throw std::runtime_error("GoZ 组块损坏");
            uint64_t n = 0;
            readExact(&n, 8);
            if (n > (payload - 8) / 2) throw std::runtime_error("GoZ 组块越界");
            faceGroups.resize(size_t(n));
            readExact(faceGroups.data(), size_t(n) * 2);
        } else if (tag < goz::Points) {
            size_t old = m.gozHead.size();
            m.gozHead.resize(old + size);
            memcpy(&m.gozHead[old], th, 8);
            readExact(&m.gozHead[old + 8], payload);
        }
        pos += size;
    }
    if (!faceGroups.empty() && faceGroups.size() == trisPerFace.size()) {
        m.G.resize(m.nt());
        size_t n = trisPerFace.size();
        if (n <= SINGLE_PASS_FACES) {
            const size_t fb = 256;
            ioBlocks(n, fb, [&](size_t b, size_t s, size_t e) {
                uint16_t* gp = m.G.data() + fOff[b];
                for (size_t i = s; i < e; i++) {
                    *gp++ = faceGroups[i];
                    if (trisPerFace[i] == 2) *gp++ = faceGroups[i];
                }
            });
        } else {
            size_t nblk = fOff.size() > 1 ? fOff.size() - 1 : 0;
            ioBlocks(nblk, std::max<size_t>(1, nblk), [&](size_t, size_t b0, size_t b1) {
                for (size_t b = b0; b < b1; b++) {
                    size_t s = b * CHUNK_FACES, e = std::min(n, s + CHUNK_FACES);
                    uint16_t* gp = m.G.data() + fOff[b];
                    for (size_t i = s; i < e; i++) {
                        *gp++ = faceGroups[i];
                        if (trisPerFace[i] == 2) *gp++ = faceGroups[i];
                    }
                }
            });
        }
    }
    if (m.C.size() != m.nv()) m.C.clear();
    if (!m.M.empty()) dropEmptyMask(m);
    if (!m.T.empty()) validateMeshIndices(m);
    return m;
}

inline std::string writeGoz(const Mesh& m, const std::string& name) {
    std::string s;
    auto put = [&](const void* p, size_t n) { s.append((const char*)p, n); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    auto u64 = [&](uint64_t v) { put(&v, 8); };
    auto chunkSz = [](size_t bytes) -> uint32_t {
        if (bytes > UINT32_MAX) throw std::runtime_error("GoZ 单块超过 4 GiB 上限");
        return uint32_t(bytes);
    };
    if (m.gozHead.size() > 32) {
        s += m.gozHead;
    } else {
        std::string hdr = "GoZb 1.0 ZBrush GoZ Binary";
        hdr.resize(32, '.');
        s += hdr;
        u32(goz::Name); u32(chunkSz(16 + name.size())); u64(1); s += name;
        u32(goz::Flags); u32(20); u64(1); u32(0x800);
    }
    u32(goz::Points); u32(chunkSz(16 + m.nv() * 12)); u64(m.nv()); put(m.P.data(), m.nv() * 12);
    u32(goz::Faces); u32(chunkSz(16 + m.nt() * 16)); u64(m.nt());
    // 面和颜色先占好位置再并行填（大模型逐个追加要一秒多）
    size_t off = s.size();
    s.resize(off + m.nt() * 16);
    ioBlocks(m.nt(), 64, [&](size_t, size_t b, size_t e) {
        char* d = s.data() + off + b * 16;
        const uint32_t none = 0xFFFFFFFFu;
        for (size_t t = b; t < e; t++, d += 16) { memcpy(d, &m.T[t * 3], 12); memcpy(d + 12, &none, 4); }
    });
    if (!m.UV.empty() && m.UV.size() == m.nt() * 6) {
        u32(goz::Uv4); u32(chunkSz(16 + m.nt() * 32)); u64(m.nt());
        off = s.size();
        s.resize(off + m.nt() * 32);
        ioBlocks(m.nt(), 64, [&](size_t, size_t b, size_t e) {
            float* d = (float*)(s.data() + off) + b * 8;
            for (size_t t = b; t < e; t++, d += 8) {
                memcpy(d, &m.UV[t * 6], 24);
                d[6] = 0.0f; d[7] = 0.0f;
            }
        });
    }
    if (!m.C.empty()) {
        u32(goz::Mrgb); u32(chunkSz(16 + m.nv() * 4)); u32(uint32_t(m.nv()));
        float one = 1.f; put(&one, 4);
        off = s.size();
        s.resize(off + m.C.size() * 4);
        ioBlocks(m.C.size(), 64, [&](size_t, size_t b, size_t e) {
            unsigned char* d = (unsigned char*)s.data() + off + b * 4;
            for (size_t i = b; i < e; i++, d += 4) {
                uint32_t c = m.C[i];
                d[0] = uint8_t(c); d[1] = uint8_t(c >> 8); d[2] = uint8_t(c >> 16); d[3] = uint8_t(c >> 24);
            }
        });
    }
    if (!m.M.empty() && m.M.size() == m.nv()) {
        u32(goz::Mask16); u32(chunkSz(16 + m.nv() * 2)); u64(m.nv());
        put(m.M.data(), m.nv() * 2);
    }
    if (!m.G.empty()) { u32(goz::Groups); u32(chunkSz(16 + m.nt() * 2)); u64(m.nt()); put(m.G.data(), m.nt() * 2); }
    s.append(16, '\0');
    return s;
}

// 流式写 GoZ：直接分段写盘，避免给大输出模型再分配几百 MB 的整块 std::string，同时校验写盘错误
inline void writeGozFile(const std::wstring& path, const Mesh& m, const std::string& name) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) throw std::runtime_error("写不了文件：" + toUtf8(path));
    struct Closer { FILE* fp; ~Closer() { if (fp) fclose(fp); } } cl{f};
    auto put = [&](const void* p, size_t n) {
        if (n && fwrite(p, 1, n, f) != n) throw std::runtime_error("写入失败（磁盘可能已满）：" + toUtf8(path));
    };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    auto u64 = [&](uint64_t v) { put(&v, 8); };
    auto chunkSz = [](size_t bytes) -> uint32_t {
        if (bytes > UINT32_MAX) throw std::runtime_error("GoZ 单块超过 4 GiB 上限");
        return uint32_t(bytes);
    };
    if (m.gozHead.size() > 32) {
        put(m.gozHead.data(), m.gozHead.size());
    } else {
        std::string hdr = "GoZb 1.0 ZBrush GoZ Binary";
        hdr.resize(32, '.');
        put(hdr.data(), 32);
        u32(goz::Name); u32(chunkSz(16 + name.size())); u64(1); put(name.data(), name.size());
        u32(goz::Flags); u32(20); u64(1); u32(0x800);
    }
    u32(goz::Points); u32(chunkSz(16 + m.nv() * 12)); u64(m.nv());
    put(m.P.data(), m.nv() * 12);

    u32(goz::Faces); u32(chunkSz(16 + m.nt() * 16)); u64(m.nt());
    constexpr size_t CHUNK_TRIS = 1 << 20;  // 16 MB
    std::vector<uint32_t> fbuf(std::min(m.nt(), CHUNK_TRIS) * 4);
    for (size_t s = 0; s < m.nt(); s += CHUNK_TRIS) {
        size_t cnt = std::min(CHUNK_TRIS, m.nt() - s);
        ioBlocks(cnt, 32, [&](size_t, size_t a, size_t e) {
            for (size_t i = a; i < e; i++) {
                uint32_t* d = &fbuf[i * 4];
                const uint32_t* src = &m.T[(s + i) * 3];
                d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; d[3] = 0xFFFFFFFFu;
            }
        });
        put(fbuf.data(), cnt * 16);
    }
    if (!m.UV.empty() && m.UV.size() == m.nt() * 6) {
        u32(goz::Uv4); u32(chunkSz(16 + m.nt() * 32)); u64(m.nt());
        constexpr size_t CHUNK_UV = 512 * 1024;  // 16 MB
        std::vector<float> ubuf(std::min(m.nt(), CHUNK_UV) * 8);
        for (size_t s = 0; s < m.nt(); s += CHUNK_UV) {
            size_t cnt = std::min(CHUNK_UV, m.nt() - s);
            ioBlocks(cnt, 32, [&](size_t, size_t a, size_t e) {
                for (size_t i = a; i < e; i++) {
                    float* d = &ubuf[i * 8];
                    memcpy(d, &m.UV[(s + i) * 6], 24);
                    d[6] = 0.0f; d[7] = 0.0f;
                }
            });
            put(ubuf.data(), cnt * 32);
        }
    }
    if (!m.C.empty()) {
        u32(goz::Mrgb); u32(chunkSz(16 + m.nv() * 4)); u32(uint32_t(m.nv()));
        float one = 1.f; put(&one, 4);
        constexpr size_t CHUNK_COL = 4 << 20;  // 16 MB
        std::vector<unsigned char> cbuf(std::min(m.C.size(), CHUNK_COL) * 4);
        for (size_t s = 0; s < m.C.size(); s += CHUNK_COL) {
            size_t cnt = std::min(CHUNK_COL, m.C.size() - s);
            ioBlocks(cnt, 32, [&](size_t, size_t a, size_t e) {
                for (size_t i = a; i < e; i++) {
                    uint32_t c = m.C[s + i];
                    unsigned char* d = &cbuf[i * 4];
                    d[0] = uint8_t(c); d[1] = uint8_t(c >> 8); d[2] = uint8_t(c >> 16); d[3] = uint8_t(c >> 24);
                }
            });
            put(cbuf.data(), cnt * 4);
        }
    }
    if (!m.M.empty() && m.M.size() == m.nv()) {
        u32(goz::Mask16); u32(chunkSz(16 + m.nv() * 2)); u64(m.nv());
        put(m.M.data(), m.nv() * 2);
    }
    if (!m.G.empty()) {
        u32(goz::Groups); u32(chunkSz(16 + m.nt() * 2)); u64(m.nt());
        put(m.G.data(), m.nt() * 2);
    }
    char z[16] = {};
    put(z, 16);
    FILE* fp = cl.fp;
    cl.fp = nullptr;
    if (fclose(fp) != 0) throw std::runtime_error("写入失败（磁盘可能已满）：" + toUtf8(path));
}

// ---------------------------------------------------------------- STL（二进制，按坐标焊接顶点）
inline Mesh readStl(const std::vector<char>& d) {
    if (d.size() < 84) throw std::runtime_error("STL 太短");
    uint32_t n;
    memcpy(&n, d.data() + 80, 4);
    if (84 + size_t(n) * 50 != d.size()) throw std::runtime_error("只支持二进制 STL");
    struct K { float x, y, z; uint32_t i; };
    std::vector<K> ks(size_t(n) * 3);
    for (size_t t = 0; t < n; t++)
        for (int k = 0; k < 3; k++) {
            float v[3];
            memcpy(v, d.data() + 84 + t * 50 + 12 + k * 12, 12);
            ks[t * 3 + k] = {v[0], v[1], v[2], uint32_t(t * 3 + k)};
        }
    std::sort(ks.begin(), ks.end(), [](const K& a, const K& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    });
    Mesh m;
    m.T.resize(size_t(n) * 3);
    for (size_t i = 0; i < ks.size(); i++) {
        if (i == 0 || ks[i].x != ks[i - 1].x || ks[i].y != ks[i - 1].y || ks[i].z != ks[i - 1].z)
            m.P.insert(m.P.end(), {ks[i].x, ks[i].y, ks[i].z});
        m.T[ks[i].i] = uint32_t(m.nv() - 1);
    }
    // 去掉焊接后退化的三角形
    size_t w = 0;
    for (size_t t = 0; t < n; t++) {
        uint32_t a = m.T[t * 3], b = m.T[t * 3 + 1], c = m.T[t * 3 + 2];
        if (a == b || b == c || a == c) continue;
        m.T[w * 3] = a; m.T[w * 3 + 1] = b; m.T[w * 3 + 2] = c; w++;
    }
    m.T.resize(w * 3);
    return m;
}

inline Mesh loadMesh(const std::wstring& path) {
    auto e = lowerExt(path);
    if (e == L".goz") return readGozFile(path);
    auto buf = readAll(path);
    if (e == L".obj") return readObj(buf);
    if (e == L".stl") return readStl(buf);
    throw std::runtime_error("不认识的格式：" + toUtf8(e));
}

inline void saveMesh(const std::wstring& path, const Mesh& m) {
    auto e = lowerExt(path);
    if (e == L".goz") writeGozFile(path, m, "KMDecimate");
    else writeAll(path, writeObj(m));
}
