// 检查更新：读 GitHub 上 KMDecimate 最新的 Release，和插件版本比较，弹窗告诉用户结果（有新版本时可以打开下载页）。
// 插件用 VarSave 存 work\update.zvr：0 格式号 1 / 1–3 插件版本（0.3.1 = 0, 3, 1），再用它启动 KMDecimate.exe。
// 结果另写一行到同目录的 update.txt（状态 最新版本 下载页），给离线测试用。
// 测试用环境变量：KMDEC_UPDATE_REPO=owner/name 换一个仓库，KMDEC_TEST_NOBOX=1 不弹窗。
#pragma once
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace kmupd {

inline const wchar_t* REPO = L"chankamman-dev/KMDecimate";

inline std::wstring envOr(const wchar_t* name, const std::wstring& def) {
    wchar_t buf[512];
    DWORD n = GetEnvironmentVariableW(name, buf, 512);
    return (n > 0 && n < 512) ? std::wstring(buf, n) : def;
}

inline std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// GET https://api.github.com<path>；返回 HTTP 状态码，0 = 连不上（err 里是原因）
inline int httpGet(const std::wstring& path, std::string& body, std::string& err) {
    int status = 0;
    HINTERNET s = WinHttpOpen(L"KMDecimate-update-check", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"KMDecimate-update-check", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r) WinHttpSetTimeouts(r, 10000, 10000, 10000, 15000);
    const wchar_t* hdr = L"Accept: application/vnd.github+json\r\n";
    if (r && WinHttpSendRequest(r, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        status = (int)code;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got) || got == 0) break;
            body.append(chunk.data(), got);
        }
    } else {
        char t[64];
        snprintf(t, sizeof t, "网络错误 %lu", GetLastError());
        err = t;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return status;
}

// JSON 里第一个 "key":"值"（Release 的 tag_name / html_url 都在作者信息之前，取第一个就是 Release 自己的）
inline std::string jsonString(const std::string& j, const std::string& key) {
    size_t p = j.find("\"" + key + "\"");
    if (p == std::string::npos) return "";
    p = j.find(':', p);
    if (p == std::string::npos) return "";
    p = j.find('"', p);
    if (p == std::string::npos) return "";
    std::string v;
    for (size_t i = p + 1; i < j.size() && j[i] != '"'; i++) {
        if (j[i] == '\\' && i + 1 < j.size()) i++;
        v += j[i];
    }
    return v;
}

// "v0.3.10" -> {0, 3, 10}
inline std::vector<int> parseVersion(const std::string& s) {
    std::vector<int> v;
    int cur = -1;
    for (char ch : s) {
        if (ch >= '0' && ch <= '9') cur = (cur < 0 ? 0 : cur * 10) + (ch - '0');
        else if (cur >= 0) { v.push_back(cur); cur = -1; if (ch != '.') break; }
    }
    if (cur >= 0) v.push_back(cur);
    return v;
}

// a < b：缺的位数当 0
inline bool versionLess(std::vector<int> a, std::vector<int> b) {
    size_t n = std::max(a.size(), b.size());
    a.resize(n, 0);
    b.resize(n, 0);
    return a < b;
}

inline int box(const std::wstring& text, UINT type) {
    if (envOr(L"KMDEC_TEST_NOBOX", L"") == L"1") return IDNO;
    return MessageBoxW(GetForegroundWindow(), text.c_str(), L"KM减面大王：检查更新", type | MB_TOPMOST | MB_SETFOREGROUND);
}

inline void openUrl(const std::wstring& url) {
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

inline int checkUpdate(const std::wstring& zvrPath, const std::vector<float>& v) {
    char cur[64];
    snprintf(cur, sizeof cur, "%d.%d.%d", v.size() > 1 ? (int)v[1] : 0, v.size() > 2 ? (int)v[2] : 0, v.size() > 3 ? (int)v[3] : 0);
    std::wstring repo = envOr(L"KMDEC_UPDATE_REPO", REPO);
    std::wstring page = L"https://github.com/" + repo + L"/releases";
    std::string body, err;
    int code = httpGet(L"/repos/" + repo + L"/releases/latest", body, err);
    std::string tag = code == 200 ? jsonString(body, "tag_name") : "";
    std::string url = code == 200 ? jsonString(body, "html_url") : "";
    std::string state;
    std::wstring wcur = widen(cur);
    if (code == 200 && !tag.empty()) {
        std::wstring wtag = widen(tag);
        if (versionLess(parseVersion(cur), parseVersion(tag))) {
            state = "new";
            if (box(L"发现新版本 " + wtag + L"（现在装的是 " + wcur + L"）。\n\n打开下载页面吗？", MB_YESNO | MB_ICONINFORMATION) == IDYES)
                openUrl(url.empty() ? page : widen(url));
        } else {
            state = "latest";
            box(L"已经是最新版本（" + wcur + L"）。", MB_OK | MB_ICONINFORMATION);
        }
    } else {
        state = "error";
        std::wstring why = code == 0 ? L"连不上 GitHub（" + widen(err) + L"），请检查网络或代理。"
                         : code == 404 ? L"GitHub 上还没有发布的版本，或者仓库不是公开的。"
                         : code == 403 ? L"GitHub 暂时限制了访问次数，过一会儿再试。"
                         : L"GitHub 返回了 " + std::to_wstring(code) + L"。";
        if (box(L"检查更新失败：" + why + L"\n\n现在装的是 " + wcur + L"。要打开发布页面自己看看吗？", MB_YESNO | MB_ICONWARNING) == IDYES)
            openUrl(page);
    }
    std::wstring out = zvrPath.substr(0, zvrPath.find_last_of(L"\\/") + 1) + L"update.txt";
    if (FILE* f = _wfopen(out.c_str(), L"wb")) {
        fprintf(f, "%s %d %s %s %s\r\n", state.c_str(), code, cur, tag.empty() ? "-" : tag.c_str(), url.empty() ? "-" : url.c_str());
        fclose(f);
    }
    return state == "error" ? 2 : 0;
}

}  // namespace kmupd
