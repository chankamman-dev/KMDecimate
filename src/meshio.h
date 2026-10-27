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

// 把 [0, n) 分成 nb 段并行处理 f(段号, 起, 止)：读写大模型用。段数固定，结果和线程调度无关
template <class F>
void ioBlocks(size_t n, size_t nb, F&& f) {
    size_t T = std::min<size_t>(nb, std::max(1u, std::thread::hardware_concurrency()));
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t b; (b = next++) < nb;) f(b, n * b / nb, n * (b + 1) / nb);
    };
    std::vector<std::thread> ts;
    for (size_t i = 1; i < T; i++) ts.emplace_back(work);
    work();
    for (auto& t : ts) t.join();
}

inline std::vector<char> readAll(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) throw std::runtime_error("打不开文件：" + toUtf8(path));
    _fseeki64(f, 0, SEEK_END);
    int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
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
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
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
    bool anyGroup = false;
    std::vector<int64_t> poly;
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
        } else if (p[0] == 'f' && p + 1 < e && (p[1] == ' ' || p[1] == '\t')) {
            p += 2;
            poly.clear();
            for (;;) {
                skipSp();
                if (p >= e || *p == '\n' || *p == '\r' || *p == '#') break;
                int64_t i = 0;
                auto r = std::from_chars(p, e, i);
                if (r.ptr == p) break;
                p = r.ptr;
                poly.push_back(i < 0 ? int64_t(m.nv()) + i : i - 1);
                while (p < e && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;  // 跳过 /vt/vn
            }
            for (size_t k = 2; k < poly.size(); k++) {
                m.T.push_back(uint32_t(poly[0]));
                m.T.push_back(uint32_t(poly[k - 1]));
                m.T.push_back(uint32_t(poly[k]));
                m.G.push_back(curGroup);
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
    s.reserve(m.nv() * 36 + m.nt() * 30 + m.C.size() * 9);
    s += "# KMDecimate\n";
    for (size_t i = 0; i < m.nv(); i++) {
        s += "v ";
        appendFloat(s, m.P[i * 3]); s += ' ';
        appendFloat(s, m.P[i * 3 + 1]); s += ' ';
        appendFloat(s, m.P[i * 3 + 2]); s += '\n';
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
        }
        s += '\n';
    }
    return s;
}

// ---------------------------------------------------------------- GoZ
namespace goz {
constexpr uint32_t Name = 1, Flags = 5001, Points = 10001, Faces = 20001, Mrgb = 35001, Groups = 40001;
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
        size_t body = pos + 8;
        if (tag == goz::Points) {
            uint64_t n = u64(body);
            m.P.resize(n * 3);
            memcpy(m.P.data(), d.data() + body + 8, n * 12);
        } else if (tag == goz::Faces) {
            uint64_t n = u64(body);
            const char* f = d.data() + body + 8;
            // 四边形拆成两个三角形。并行：分段数三角形，前缀和定位，再并行写
            trisPerFace.resize(n);
            ioBlocks(n, fb, [&](size_t b, size_t s, size_t e) {
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
            ioBlocks(n, fb, [&](size_t b, size_t s, size_t e) {
                uint32_t* t = m.T.data() + fOff[b] * 3;
                for (size_t i = s; i < e; i++) {
                    uint32_t q[4];
                    memcpy(q, f + i * 16, 16);
                    *t++ = q[0]; *t++ = q[1]; *t++ = q[2];
                    if (trisPerFace[i] == 2) { *t++ = q[0]; *t++ = q[2]; *t++ = q[3]; }
                }
            });
        } else if (tag == goz::Mrgb) {
            uint32_t n = u32(body);
            const unsigned char* c = (const unsigned char*)d.data() + body + 8;
            m.C.resize(n);
            ioBlocks(n, 64, [&](size_t, size_t s, size_t e) {
                for (size_t i = s; i < e; i++)  // 文件里是 B G R M
                    m.C[i] = uint32_t(c[i * 4 + 3]) << 24 | uint32_t(c[i * 4 + 2]) << 16 | uint32_t(c[i * 4 + 1]) << 8 | c[i * 4];
            });
        } else if (tag == goz::Groups) {
            uint64_t n = u64(body);
            faceGroups.resize(n);
            memcpy(faceGroups.data(), d.data() + body + 8, n * 2);
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
    return m;
}

inline std::string writeGoz(const Mesh& m, const std::string& name) {
    std::string s;
    auto put = [&](const void* p, size_t n) { s.append((const char*)p, n); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    auto u64 = [&](uint64_t v) { put(&v, 8); };
    if (m.gozHead.size() > 32) {
        s += m.gozHead;
    } else {
        std::string hdr = "GoZb 1.0 ZBrush GoZ Binary";
        hdr.resize(32, '.');
        s += hdr;
        u32(goz::Name); u32(uint32_t(16 + name.size())); u64(1); s += name;
        u32(goz::Flags); u32(20); u64(1); u32(0x800);
    }
    u32(goz::Points); u32(uint32_t(16 + m.nv() * 12)); u64(m.nv()); put(m.P.data(), m.nv() * 12);
    u32(goz::Faces); u32(uint32_t(16 + m.nt() * 16)); u64(m.nt());
    // 面和颜色先占好位置再并行填（大模型逐个追加要一秒多）
    size_t off = s.size();
    s.resize(off + m.nt() * 16);
    ioBlocks(m.nt(), 64, [&](size_t, size_t b, size_t e) {
        char* d = s.data() + off + b * 16;
        const uint32_t none = 0xFFFFFFFFu;
        for (size_t t = b; t < e; t++, d += 16) { memcpy(d, &m.T[t * 3], 12); memcpy(d + 12, &none, 4); }
    });
    if (!m.C.empty()) {
        u32(goz::Mrgb); u32(uint32_t(16 + m.nv() * 4)); u32(uint32_t(m.nv()));
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
    if (!m.G.empty()) { u32(goz::Groups); u32(uint32_t(16 + m.nt() * 2)); u64(m.nt()); put(m.G.data(), m.nt() * 2); }
    s.append(16, '\0');
    return s;
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
    auto buf = readAll(path);
    auto e = lowerExt(path);
    if (e == L".obj") return readObj(buf);
    if (e == L".goz") return readGoz(buf);
    if (e == L".stl") return readStl(buf);
    throw std::runtime_error("不认识的格式：" + toUtf8(e));
}

inline void saveMesh(const std::wstring& path, const Mesh& m) {
    auto e = lowerExt(path);
    if (e == L".goz") writeAll(path, writeGoz(m, "KMDecimate"));
    else writeAll(path, writeObj(m));
}
