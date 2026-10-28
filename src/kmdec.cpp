// KMDecimate 命令行原型：读模型 → 减面 → 写模型，并打印各阶段耗时。
// 用法：kmdec 输入.obj|.GoZ|.stl 输出.obj|.GoZ [--percent 10 | --tris N | --verts N]
//        [--threads N] [--chunks K] [--part T] [--slack S] [--border W] [--groups W] [--color W] [--coloredge W]
#include "decimate.h"
#include "update.h"
#include <windows.h>

static void usage() {
    printf("用法：kmdec 输入 输出 [--percent P | --tris N | --verts N] [--threads N] [--chunks K] [--part T] [--slack S] [--border W] [--groups W] [--color W] [--coloredge W]\n"
           "  --threads 1 = 经典单线程 QEM（对照组）；默认用全部逻辑核\n");
}

// 前台窗口属于 ZBrush（或者是 ZBrush“没有响应”时 Windows 换上的 Ghost 窗口）：这时按的 Esc 才算取消
static bool zbrushInFront() {
    HWND h = GetForegroundWindow();
    if (!h) return false;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"Ghost") == 0) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return true;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    bool zb = QueryFullProcessImageNameW(p, 0, path, &n) && n >= 10 && _wcsicmp(path + n - 10, L"zbrush.exe") == 0;
    CloseHandle(p);
    return zb;
}

// 监视线程：每 0.1 秒写一次 progress.bin（3 个 float：阶段 0 读 / 1 减面 / 2 写，完成比例 0–1，心跳计数），
// ZBrush 的 ZScript 读它显示进度条，心跳不动了就知道 exe 出事了。ZBrush 在前台时按 Esc → kmd::gCancel
struct Monitor {
    std::wstring dir;
    std::atomic<int> stage{0};
    std::atomic<size_t> toRemove{1};
    std::atomic<bool> done{false};
    std::thread th;
    explicit Monitor(const std::wstring& d) : dir(d) {
        GetAsyncKeyState(VK_ESCAPE);  // 忘掉点按钮之前按过的 Esc
        // 测试用：环境变量 KMDEC_TEST_CANCEL=秒数，到时间就当作按了 Esc
        const wchar_t* tc = _wgetenv(L"KMDEC_TEST_CANCEL");
        double cancelAt = tc ? _wtof(tc) : -1;
        th = std::thread([this, cancelAt] {
            float tick = 0;
            auto t0 = kmd::Clock::now();
            while (!done.load()) {
                if ((GetAsyncKeyState(VK_ESCAPE) & 0x8001) && zbrushInFront()) kmd::gCancel = true;
                if (cancelAt >= 0 && kmd::secs(t0) >= cancelAt) kmd::gCancel = true;
                int s = stage.load();
                double frac = s == 0 ? 0 : s == 2 ? 1 : std::min(1.0, double(kmd::gRemoved.load()) / double(std::max<size_t>(1, toRemove.load())));
                float rec[3] = {float(s), float(frac), tick += 1};
                std::wstring tmp = dir + L"progress.tmp", dst = dir + L"progress.bin";
                if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
                    fwrite(rec, sizeof rec, 1, f);
                    fclose(f);
                    MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);  // 整个换掉，ZBrush 读不到写了一半的文件
                }
                Sleep(100);
            }
        });
    }
    ~Monitor() {
        done = true;
        th.join();
    }
};

// ZScript 的 VarSave 存下的数字数组：e9 03 00 00、元素个数（u32），每个元素是类型字节 'F' 加一个 float32
static std::vector<float> readZvr(const std::wstring& path) {
    auto b = readAll(path);
    auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, b.data() + o, 4); return v; };
    if (b.size() < 8 || u32(0) != 1001) throw std::runtime_error("不是 ZScript 存的 zvr 文件");
    uint32_t n = u32(4);
    std::vector<float> v;
    for (size_t i = 0, p = 8; i < n; i++, p += 5) {
        if (p + 5 > b.size() || b[p] != 'F') throw std::runtime_error("zvr 里有不认识的元素");
        float f;
        memcpy(&f, b.data() + p + 1, 4);
        v.push_back(f);
    }
    return v;
}

// 一次减面的参数，来自 job.zvr（0.3.1 起）或 job.txt
struct Job {
    std::wstring in, out;
    double percent = 10, detail = 1;  // detail：0 = 不保留顶点色，1–10 = 颜色细节
    double target = 0;                // 目标三角形数：大于 0 时按它减，不看百分比（重拓扑前的预减面用）
    int groups = 0, threads = 0;
    std::string src;                  // 从哪读的，写进 log
};

// job.txt（ASCII 行）：KMDEC1 / 输入 / 输出 / 百分比 / 保留组 / 线程数 / 顶点色 / END。
// 顶点色：0 = 不保留；1–10 = 保留，数字是“颜色细节”。0.2 的 job 没有这一行，按 1 处理。
static Job readJobTxt(const std::wstring& path) {
    auto buf = readAll(path);
    std::vector<std::string> L;
    std::string cur;
    for (char c : buf) {
        if (c == '\n') { L.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    if (!cur.empty()) L.push_back(cur);
    if (L.size() < 6 || L[0] != "KMDEC1") throw std::runtime_error("job.txt 格式不对");
    auto wide = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };  // 交换目录是纯 ASCII
    Job j;
    j.in = wide(L[1]);
    j.out = wide(L[2]);
    j.percent = atof(L[3].c_str());
    j.groups = atoi(L[4].c_str());
    j.threads = atoi(L[5].c_str());
    j.detail = L.size() < 7 || L[6] == "END" ? 1 : atof(L[6].c_str());
    j.src = "txt";
    return j;
}

// job.zvr（插件用 VarSave 存）：0 格式号 2 / 1 百分比 / 2 保留组 / 3 线程数 / 4 顶点色（0 = 不保留，1–10 = 颜色细节）/
// 5 插件同时写 job.txt 时 MemSaveToFile 的返回值（排查用）/ 6 这次减面的编号 / 7 目标三角形数（0 = 按百分比）。
// 输入输出固定是同目录的 in.GoZ、out.GoZ。
// ZBrush 2026 出现过 MemSaveToFile 写不进去、VarSave 正常的情况，所以插件改用 VarSave 交任务；zvr 读不了时退回同目录的 job.txt
// （插件每次开始都会删掉旧的 job.txt 和 job.zvr，所以不会读到上一次的任务）。
static Job readJob(const std::wstring& path, const std::wstring& dir) {
    if (lowerExt(path) != L".zvr") return readJobTxt(path);
    std::string why;
    try {
        auto v = readZvr(path);
        if (v.size() >= 5 && v[0] == 2) {
            Job j;
            j.in = dir + L"in.GoZ";
            j.out = dir + L"out.GoZ";
            j.percent = v[1];
            j.groups = int(v[2]);
            j.threads = int(v[3]);
            j.detail = v[4];
            j.target = v.size() > 7 ? v[7] : 0;
            char t[64];
            snprintf(t, sizeof t, "zvr jobtxt=%g", v.size() > 5 ? double(v[5]) : -999.0);
            j.src = t;
            return j;
        }
        why = "格式号不对";
    } catch (std::exception& e) {
        why = e.what();
    }
    std::wstring txt = dir + L"job.txt";
    if (GetFileAttributesW(txt.c_str()) == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("读不了 job.zvr（" + why + "），也没有 job.txt");
    Job j = readJobTxt(txt);
    j.src = "txt(zvr: " + why + ")";
    return j;
}

// 上一次减面 ZBrush 端的计时：插件在导入后用 VarSave 存成 zb_last.zvr，这里追加到 zb_times.txt 再删掉。
// zb_last.zvr：0 格式号 3 / 1 导出 / 2 计算（含等待）/ 3 导入合计 / 4 是否二次导入 / 5 合计 / 6 是否复制子工具 / 7 顶点色 /
// 8 百分比 / 9 job.txt 的 MemSaveToFile 返回值 / 10 第一次导入 / 11 第二次导入 / 12 减面编号（秒）
static void logZbTimes(const std::wstring& dir) {
    std::wstring p = dir + L"zb_last.zvr";
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    try {
        auto v = readZvr(p);
        if (v.size() >= 13 && v[0] == 3) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            char line[512];
            snprintf(line, sizeof line,
                     "export=%.2f compute=%.2f import=%.2f import1=%.2f import2=%.2f reimport=%g total=%.2f copy=%g color=%g percent=%g "
                     "jobtxt=%g run=%.0f logged=%02d:%02d:%02d\r\n",
                     v[1], v[2], v[3], v[10], v[11], v[4], v[5], v[6], v[7], v[8], v[9], v[12], t.wHour, t.wMinute, t.wSecond);
            if (FILE* f = _wfopen((dir + L"zb_times.txt").c_str(), L"ab")) {
                fwrite(line, 1, strlen(line), f);
                fclose(f);
            }
        }
    } catch (std::exception&) {
    }
    DeleteFileW(p.c_str());
}

// ZBrush 插件调用：参数只有一个任务文件（job.zvr，旧插件是 job.txt），见 readJob。
// 有 C:\Users\Public\KMDecimate\keep_input 这个文件时，把输入另存为 work\last_in.GoZ（拿用户的真实模型调参用）。
// 同目录写 status.txt（1 成功，8 已取消，9 失败）、note.txt（给 ZBrush 显示的 UTF-8 消息），并追加 log.txt；
// 计算期间写 progress.bin（见 Monitor）。
static int runJob(const std::wstring& jobPath) {
    std::wstring dir = jobPath.substr(0, jobPath.find_last_of(L"\\/") + 1);
    logZbTimes(dir);
    auto finish = [&](const char* status, const std::string& note, const std::string& log) {
        writeAll(dir + L"note.txt", note);
        FILE* f = _wfopen((dir + L"log.txt").c_str(), L"ab");
        if (f) { fwrite(log.data(), 1, log.size(), f); fclose(f); }
        writeAll(dir + L"status.txt", status);  // 最后写，ZBrush 一看到它就去读结果
    };
    kmd::gRemoved = 0;
    kmd::gCancel = false;
    Monitor mon(dir);
    try {
        Job job = readJob(jobPath, dir);
        const std::wstring& in = job.in;
        const std::wstring& out = job.out;
        double percent = job.percent;
        DecimateOptions o;
        o.fastExit = true;  // 减完写出结果就退出
        o.groupWeight = job.groups ? 10 : 0;
        o.threads = job.threads;
        double detail = job.detail;
        bool keepColor = detail > 0;
        if (keepColor) {
            // 1 → 0.3（0.3 版的取值），10 → 30，按对数均匀；见 README 的“颜色细节”表
            detail = std::clamp(detail, 1.0, 10.0);
            o.colorWeight = o.colorEdge = 0.3 * std::pow(10.0, (detail - 1) / 4.5);
        }
        if (GetFileAttributesW(L"C:\\Users\\Public\\KMDecimate\\keep_input") != INVALID_FILE_ATTRIBUTES)
            CopyFileW(in.c_str(), (dir + L"last_in.GoZ").c_str(), FALSE);

        auto t0 = kmd::Clock::now();
        Mesh m = loadMesh(in);
        double tLoad = kmd::secs(t0);
        if (m.nt() == 0) throw std::runtime_error("导出的模型是空的");
        bool hadColor = !m.C.empty();
        if (!keepColor) m.C.clear();  // 只按形状减，结果不带 Polypaint
        if (kmd::gCancel) throw kmd::Cancelled();
        o.targetTris = job.target > 0 ? size_t(job.target) : size_t(std::max(4.0, m.nt() * percent / 100));
        mon.toRemove = m.nt() > o.targetTris ? m.nt() - o.targetTris : 1;
        mon.stage = 1;
        DecimateStats st;
        auto t1 = kmd::Clock::now();
        Mesh r = decimate(m, o, st);
        double tDec = kmd::secs(t1);
        if (kmd::gCancel) throw kmd::Cancelled();
        mon.stage = 2;
        auto t2 = kmd::Clock::now();
        saveMesh(out, r);
        double tSave = kmd::secs(t2);
        char note[512], log[512];
        snprintf(note, sizeof note, "KM减面大王：%zu → %zu 面（%zu 点），计算 %.2f 秒（读 %.2f，写 %.2f，%d 线程）%s", m.nt(), r.nt(), r.nv(),
                 tDec, tLoad, tSave, st.threads, !hadColor ? "" : keepColor ? "，保留了顶点色" : "，去掉了顶点色");
        snprintf(log, sizeof log,
                 "%s percent=%g groups=%d color=%g had_color=%d in_tris=%zu out_tris=%zu load=%.3f decimate=%.3f save=%.3f passes=%zu threads=%d target=%.0f job=%s\r\n",
                 toUtf8(in).c_str(), percent, o.groupWeight > 0, keepColor ? detail : 0.0, hadColor, m.nt(), r.nt(), tLoad, tDec, tSave, st.passes.size(),
                 st.threads, job.target, job.src.c_str());
        finish("1", note, log);
        return 0;
    } catch (kmd::Cancelled&) {
        finish("8", "KM减面大王：已取消（按了 Esc），模型没有改动。", "cancelled\r\n");
        return 3;
    } catch (std::exception& e) {
        std::string msg = std::string("KM减面大王出错：") + e.what();
        finish("9", msg, msg + "\r\n");
        return 2;
    }
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    // 插件的“检查更新”按钮：参数是 work\update.zvr
    if (argc == 2) {
        std::wstring a = argv[1];
        if (a.size() >= 10 && _wcsicmp(a.c_str() + a.size() - 10, L"update.zvr") == 0) {
            std::vector<float> v;
            try { v = readZvr(a); } catch (std::exception&) {}
            return kmupd::checkUpdate(a, v);
        }
    }
    if (argc == 2 && (lowerExt(argv[1]) == L".txt" || lowerExt(argv[1]) == L".zvr")) return runJob(argv[1]);
    if (argc < 3) { usage(); return 1; }
    std::wstring inPath = argv[1], outPath = argv[2];
    double percent = 10, tris = 0, verts = 0;
    DecimateOptions o;
    o.fastExit = true;  // 命令行减完写出结果就退出
    for (int i = 3; i + 1 < argc; i += 2) {
        std::wstring k = argv[i];
        double v = _wtof(argv[i + 1]);
        if (k == L"--percent") percent = v;
        else if (k == L"--tris") tris = v;
        else if (k == L"--verts") verts = v;
        else if (k == L"--threads") o.threads = int(v);
        else if (k == L"--chunks") o.chunksPerThread = int(v);
        else if (k == L"--part") o.partTris = size_t(v);
        else if (k == L"--slack") o.slack = v;
        else if (k == L"--keep") o.keep = v;
        else if (k == L"--seam") o.seamShare = v;
        else if (k == L"--border") o.borderWeight = v;
        else if (k == L"--groups") o.groupWeight = v;
        else if (k == L"--color") o.colorWeight = v;
        else if (k == L"--coloredge") o.colorEdge = v;
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
            printf("  第%zu遍 %zu→%zu 面：%.3f 秒（%zu 块，接缝点 %zu，%d 轮，切块 %.3f，建堆 %.3f，合并 %.3f）\n", i + 1, p.fromTris, p.goal, p.time,
                   p.parts, p.seamVerts, p.rounds, p.cut, p.build, p.merge);
        }
        printf("结果：%zu 顶点，%zu 三角形（目标 %zu）\n写出 %s：%.3f 秒\n", r.nv(), r.nt(), o.targetTris, toUtf8(outPath).c_str(), tSave);
        printf("TIMING load=%.3f decimate=%.3f save=%.3f in_tris=%zu out_tris=%zu out_verts=%zu\n", tLoad, tDec, tSave, m.nt(), r.nt(), r.nv());
    } catch (std::exception& e) {
        printf("出错：%s\n", e.what());
        return 2;
    }
    return 0;
}
