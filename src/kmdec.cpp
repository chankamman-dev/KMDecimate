// KMDecimate 命令行原型：读模型 → 减面 → 写模型，并打印各阶段耗时。
// 用法：kmdec 输入.obj|.GoZ|.stl 输出.obj|.GoZ [--percent 10 | --tris N | --verts N]
//        [--threads N] [--chunks K] [--slack S] [--border W] [--groups W]
#include "decimate.h"
#include <windows.h>

static void usage() {
    printf("用法：kmdec 输入 输出 [--percent P | --tris N | --verts N] [--threads N] [--chunks K] [--slack S] [--border W] [--groups W]\n"
           "  --threads 1 = 经典单线程 QEM（对照组）；默认用全部逻辑核\n");
}

// ZBrush 插件调用：参数只有一个 job.txt（ASCII 行：KMDEC1 / 输入 / 输出 / 百分比 / 保留组 / 线程数 / END）。
// 同目录写 status.txt（1 成功，9 失败）、note.txt（给 ZBrush 显示的 UTF-8 消息），并追加 log.txt。
static int runJob(const std::wstring& jobPath) {
    std::wstring dir = jobPath.substr(0, jobPath.find_last_of(L"\\/") + 1);
    auto finish = [&](const char* status, const std::string& note, const std::string& log) {
        writeAll(dir + L"note.txt", note);
        FILE* f = _wfopen((dir + L"log.txt").c_str(), L"ab");
        if (f) { fwrite(log.data(), 1, log.size(), f); fclose(f); }
        writeAll(dir + L"status.txt", status);  // 最后写，ZBrush 一看到它就去读结果
    };
    try {
        auto buf = readAll(jobPath);
        std::vector<std::string> L;
        std::string cur;
        for (char c : buf) {
            if (c == '\n') { L.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        if (!cur.empty()) L.push_back(cur);
        if (L.size() < 6 || L[0] != "KMDEC1") throw std::runtime_error("job.txt 格式不对");
        auto wide = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };  // 交换目录是纯 ASCII
        std::wstring in = wide(L[1]), out = wide(L[2]);
        double percent = atof(L[3].c_str());
        DecimateOptions o;
        o.groupWeight = atoi(L[4].c_str()) ? 10 : 0;
        o.threads = atoi(L[5].c_str());

        auto t0 = kmd::Clock::now();
        Mesh m = loadMesh(in);
        double tLoad = kmd::secs(t0);
        if (m.nt() == 0) throw std::runtime_error("导出的模型是空的");
        o.targetTris = size_t(std::max(4.0, m.nt() * percent / 100));
        DecimateStats st;
        auto t1 = kmd::Clock::now();
        Mesh r = decimate(m, o, st);
        double tDec = kmd::secs(t1);
        auto t2 = kmd::Clock::now();
        saveMesh(out, r);
        double tSave = kmd::secs(t2);
        char note[512], log[512];
        snprintf(note, sizeof note, "KM 减面：%zu → %zu 面（%zu 点），计算 %.2f 秒（读 %.2f，写 %.2f，%d 线程）", m.nt(), r.nt(), r.nv(),
                 tDec, tLoad, tSave, st.threads);
        snprintf(log, sizeof log, "%s percent=%g groups=%d in_tris=%zu out_tris=%zu load=%.3f decimate=%.3f save=%.3f passes=%zu threads=%d\r\n",
                 L[1].c_str(), percent, o.groupWeight > 0, m.nt(), r.nt(), tLoad, tDec, tSave, st.passes.size(), st.threads);
        finish("1", note, log);
        return 0;
    } catch (std::exception& e) {
        std::string msg = std::string("KM 减面出错：") + e.what();
        finish("9", msg, msg + "\r\n");
        return 2;
    }
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    if (argc == 2 && lowerExt(argv[1]) == L".txt") return runJob(argv[1]);
    if (argc < 3) { usage(); return 1; }
    std::wstring inPath = argv[1], outPath = argv[2];
    double percent = 10, tris = 0, verts = 0;
    DecimateOptions o;
    for (int i = 3; i + 1 < argc; i += 2) {
        std::wstring k = argv[i];
        double v = _wtof(argv[i + 1]);
        if (k == L"--percent") percent = v;
        else if (k == L"--tris") tris = v;
        else if (k == L"--verts") verts = v;
        else if (k == L"--threads") o.threads = int(v);
        else if (k == L"--chunks") o.chunksPerThread = int(v);
        else if (k == L"--slack") o.slack = v;
        else if (k == L"--keep") o.keep = v;
        else if (k == L"--seam") o.seamShare = v;
        else if (k == L"--border") o.borderWeight = v;
        else if (k == L"--groups") o.groupWeight = v;
        else { usage(); return 1; }
    }
    try {
        auto t0 = kmd::Clock::now();
        Mesh m = loadMesh(inPath);
        double tLoad = kmd::secs(t0);
        if (verts > 0) o.targetTris = size_t(m.nt() * (verts / m.nv()));
        else if (tris > 0) o.targetTris = size_t(tris);
        else o.targetTris = size_t(m.nt() * percent / 100);
        printf("读入 %s：%zu 顶点，%zu 三角形%s%s，%.3f 秒\n", toUtf8(inPath).c_str(), m.nv(), m.nt(),
               m.C.empty() ? "" : "，有 Polypaint", m.G.empty() ? "" : "，有组", tLoad);
        DecimateStats st;
        auto t1 = kmd::Clock::now();
        Mesh r = decimate(m, o, st);
        double tDec = kmd::secs(t1);
        auto t2 = kmd::Clock::now();
        saveMesh(outPath, r);
        double tSave = kmd::secs(t2);
        printf("减面（%d 线程）：%.3f 秒，其中准备 %.3f\n", st.threads, tDec, st.setup);
        for (size_t i = 0; i < st.passes.size(); i++) {
            auto& p = st.passes[i];
            printf("  第%zu遍 %zu→%zu 面：%.3f 秒（%zu 块，接缝点 %zu，%d 轮，合并 %.3f）\n", i + 1, p.fromTris, p.goal, p.time, p.parts,
                   p.seamVerts, p.rounds, p.merge);
        }
        printf("结果：%zu 顶点，%zu 三角形（目标 %zu）\n写出 %s：%.3f 秒\n", r.nv(), r.nt(), o.targetTris, toUtf8(outPath).c_str(), tSave);
        printf("TIMING load=%.3f decimate=%.3f save=%.3f in_tris=%zu out_tris=%zu out_verts=%zu\n", tLoad, tDec, tSave, m.nt(), r.nt(), r.nv());
    } catch (std::exception& e) {
        printf("出错：%s\n", e.what());
        return 2;
    }
    return 0;
}
