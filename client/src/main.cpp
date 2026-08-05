// ---------------------------------------------------------------------------
// Hanbot 存档打包上传器 —— 主程序（WebView2 宿主 + native↔JS 桥接）
//
//   形态：无边框圆角窗口 + DWM 阴影，WebView2 铺满客户区渲染 webui/index.html
//         （以资源形式内嵌），通过 WebMessage 与页面双向通信。
//   说明：所有上传/下载/检测/连通性逻辑均复用原有模块（uploader / lolfind /
//         util / config），本文件只替换“UI 渲染层”——把 Win32 控件更新改为
//         向页面 PostWebMessageAsString 推 JSON。
// ---------------------------------------------------------------------------

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>

#include <wrl.h>
#include <WebView2.h>

#include <string>
#include <vector>
#include <atomic>
#include <thread>

#include "resource.h"
#include "config.h"
#include "util.h"
#include "lol_finder.h"
#include "uploader.h"
#include "json_mini.h"

using namespace Microsoft::WRL;

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "WebView2LoaderStatic.lib")

// ===========================================================================
// 全局状态
// ===========================================================================
namespace {

HINSTANCE g_hInst = nullptr;
HWND      g_hMain = nullptr;

ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2>           g_webview;

bool g_pageReady = false;                 // 页面是否已发来 ready（可收消息）
std::vector<std::wstring> g_pending;      // 页面就绪前缓冲的待发 JSON

int  g_dpi = 96;
int  g_titleH = 42;
bool g_useRgn = true;                      // Win11 用 DWM 原生圆角；否则用 rgn

std::atomic<bool> g_busy{ false };
std::atomic<bool> g_cancel{ false };
std::atomic<bool> g_uploadDone{ false };

std::wstring g_lastResultText;            // 关闭前密码警告用
std::wstring g_lastDir;                   // 最近一次操作的目录（结果文案用）
std::wstring g_currentPwd;                 // 页面当前密码框内容（关闭时写入 uploader.ini）
std::wstring g_appIconB64;                // 启动期从资源读取的软件图标（base64，供页面头部注入）

inline int S(int v) { return ::MulDiv(v, g_dpi, 96); }

// GetDpiForWindow 是 Win10 1607+ 才有的，动态解析避免链接/运行期报错
int QueryWindowDpi(HWND hwnd) {
    HMODULE user32 = ::GetModuleHandleW(OBFW("dXNlcjMyLmRsbA=="));
    if (user32) {
        using GetDpiFn = UINT (WINAPI*)(HWND);
        auto fn = (GetDpiFn)::GetProcAddress(user32, OBFA("R2V0RHBpRm9yV2luZG93"));
        if (fn) {
            const UINT d = fn(hwnd);
            if (d >= 72 && d <= 480) return (int)d;
        }
    }
    HDC dc = ::GetDC(hwnd);
    int dpi = 96;
    if (dc) {
        const int d = ::GetDeviceCaps(dc, LOGPIXELSX);
        if (d >= 72 && d <= 480) dpi = d;
        ::ReleaseDC(hwnd, dc);
    }
    return dpi;
}

// ---------------------------------------------------------------------------
// JSON 构建（native -> page）
// ---------------------------------------------------------------------------
std::wstring JStr(const std::wstring& s) {
    std::string u8 = util::WideToUtf8(s);
    std::string esc = json::EscapeString(u8);
    return OBFW("Ig==") + util::Utf8ToWide(esc) + OBFW("Ig==");
}
std::wstring WBool(bool b) { return b ? OBFW("dHJ1ZQ==") : OBFW("ZmFsc2U="); }

void PostJson(const std::wstring& json) {
    if (g_webview && g_pageReady)
        g_webview->PostWebMessageAsString(json.c_str());
    else
        g_pending.push_back(json);
}

std::wstring BuildInit(const std::wstring& password = L"") {
    // 刻意不下发后端地址：页面日志对用户可见，地址一旦落到界面上就等于公开了服务端。
    std::wstring s = std::wstring(L"{\"type\":\"init\",\"app\":") + JStr(APP_TITLE_W) +
                     OBFW("LCJ2ZXJzaW9uIjoiMS4wLjAi");
    if (!password.empty()) s += OBFW("LCJwYXNzd29yZCI6") + JStr(password);
    s += OBFW("fQ==");
    return s;
}

// 当前本地时间字符串（YYYY-MM-DD HH:MM），用于上传结果里的「完成时间」
std::wstring NowString() {
    SYSTEMTIME st; ::GetLocalTime(&st);
    wchar_t buf[64]{};
    ::swprintf(buf, 64, OBFW("JTA0ZC0lMDJkLSUwMmQgJTAyZDolMDJk"),
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}
std::wstring BuildLog(const std::wstring& t) {
    return L"{\"type\":\"log\",\"text\":" + JStr(t) + OBFW("fQ==");
}
std::wstring BuildProgress(int permille, const std::wstring& s) {
    return L"{\"type\":\"progress\",\"permille\":" + std::to_wstring(permille) +
           OBFW("LCJzdGFnZSI6") + JStr(s) + OBFW("fQ==");
}
std::wstring BuildConn(int st, const std::wstring& t) {
    return L"{\"type\":\"conn\",\"state\":" + std::to_wstring(st) + OBFW("LCJ0ZXh0Ijo=") + JStr(t) + OBFW("fQ==");
}
std::wstring BuildBusy(bool b) {
    return L"{\"type\":\"busy\",\"busy\":" + WBool(b) + OBFW("fQ==");
}
std::wstring BuildDetectDone(const std::wstring& path, int vs, const std::wstring& vt) {
    return L"{\"type\":\"detectDone\",\"path\":" + JStr(path) +
           OBFW("LCJ2ZXJpZnlTdGF0ZSI6") + std::to_wstring(vs) + OBFW("LCJ2ZXJpZnlUZXh0Ijo=") + JStr(vt) + OBFW("fQ==");
}
std::wstring BuildDir(const std::wstring& path, int vs, const std::wstring& vt) {
    return L"{\"type\":\"dir\",\"path\":" + JStr(path) +
           OBFW("LCJ2ZXJpZnlTdGF0ZSI6") + std::to_wstring(vs) + OBFW("LCJ2ZXJpZnlUZXh0Ijo=") + JStr(vt) + OBFW("fQ==");
}
std::wstring BuildDone(bool ok, bool canceled, bool isDownload, bool pwdWrong,
                       const std::wstring& error, const std::wstring& resultText,
                       const std::wstring& resultLabel, const std::wstring& stage, bool copyEnabled) {
    return L"{\"type\":\"done\",\"ok\":" + WBool(ok) +
           OBFW("LCJjYW5jZWxlZCI6") + WBool(canceled) +
           OBFW("LCJpc0Rvd25sb2FkIjo=") + WBool(isDownload) +
           OBFW("LCJwYXNzd29yZFdyb25nIjo=") + WBool(pwdWrong) +
           OBFW("LCJlcnJvciI6") + JStr(error) +
           OBFW("LCJyZXN1bHRUZXh0Ijo=") + JStr(resultText) +
           OBFW("LCJyZXN1bHRMYWJlbCI6") + JStr(resultLabel) +
           OBFW("LCJzdGFnZSI6") + JStr(stage) +
           OBFW("LCJjb3B5RW5hYmxlZCI6") + WBool(copyEnabled) + OBFW("fQ==");
}

std::wstring BuildIcon(const std::wstring& b64) {
    return L"{\"type\":\"icon\",\"data\":\"" + b64 + OBFW("In0=");
}

// 随机生成密码的查重结果（worker 线程 -> UI 线程）
struct PwdCheckResult {
    std::wstring password;
    bool         exists = false;
};
std::wstring BuildPasswordChecked(const std::wstring& pwd, bool exists) {
    return L"{\"type\":\"passwordChecked\",\"password\":" + JStr(pwd) +
           OBFW("LCJleGlzdHMiOg==") + WBool(exists) + OBFW("fQ==");
}

// 标准 Base64 编码（用于把图标资源注入页面）
std::wstring Base64Encode(const uint8_t* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)data[i + 2];
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? tbl[v & 0x3F] : '=');
    }
    return util::Utf8ToWide(out);
}

// 从 RCDATA 资源读取软件图标（PNG），base64 编码后供 WebView2 头部图标使用
std::wstring LoadAppIconBase64() {
    HRSRC hRes = ::FindResourceW(g_hInst, MAKEINTRESOURCE(IDR_APP_ICON_PNG), RT_RCDATA);
    if (!hRes) return L"";
    HGLOBAL hGlob = ::LoadResource(g_hInst, hRes);
    if (!hGlob) return L"";
    DWORD size = ::SizeofResource(g_hInst, hRes);
    const uint8_t* data = (const uint8_t*)::LockResource(hGlob);
    if (!data || size == 0) return L"";
    return Base64Encode(data, (size_t)size);
}

// ---------------------------------------------------------------------------
// 跨线程投递（worker 线程 -> UI 线程）
// ---------------------------------------------------------------------------
void PostLog(const std::wstring& text) {
    if (!g_hMain) return;
    ::PostMessageW(g_hMain, WM_APP_LOG, 0, (LPARAM)new std::wstring(text));
}
void PostProgress(int permille, const std::wstring& stage) {
    if (!g_hMain) return;
    ::PostMessageW(g_hMain, WM_APP_PROGRESS, (WPARAM)permille,
                   (LPARAM)new std::wstring(stage));
}

// ---------------------------------------------------------------------------
// 选择文件夹对话框
// ---------------------------------------------------------------------------
std::wstring PickFolder(HWND owner) {
    std::wstring result;

    IFileDialog* dlg = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts)))
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(OBFW("6K+36YCJ5oupIExlYWd1ZSBvZiBMZWdlbmRzIOeahCBzYXZlcyDnm67lvZU="));

        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                    result = psz;
                    ::CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        dlg->Release();
        return result;
    }

    BROWSEINFOW bi{};
    wchar_t buf[MAX_PATH]{};
    bi.hwndOwner = owner;
    bi.pszDisplayName = buf;
    bi.lpszTitle = OBFW("6K+36YCJ5oupIHNhdmVzIOebruW9lQ==");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = ::SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH]{};
        if (::SHGetPathFromIDListW(pidl, path)) result = path;
        ::CoTaskMemFree(pidl);
    }
    return result;
}

// ---------------------------------------------------------------------------
// 工作线程（逻辑全部复用，仅把结果经 WM_APP_* 回 UI 线程）
// ---------------------------------------------------------------------------
void DetectThread() {
    g_cancel.store(false);
    ::PostMessageW(g_hMain, WM_APP_SET_BUSY, 1, 0);
    PostProgress(0, OBFW("5q2j5Zyo5qOA5rWL55uu5b2VLi4u"));
    PostLog(OBFW("5byA5aeL5qOA5rWLIHNhdmVzIOebruW9lS4uLg=="));

    auto results = lolfind::FindAll(
        [](const std::wstring& s) { PostLog(s); },
        &g_cancel, 60);

    auto* payload = new std::vector<lolfind::Candidate>(std::move(results));
    ::PostMessageW(g_hMain, WM_APP_DETECT_DONE, 0, (LPARAM)payload);
}

void UploadThread(std::wstring dir, std::wstring password) {
    g_cancel.store(false);
    ::PostMessageW(g_hMain, WM_APP_SET_BUSY, 1, 0);
    g_lastDir = dir;

    uploader::Outcome result = uploader::Run(
        dir, password,
        [](const std::wstring& s) { PostLog(s); },
        [](int permille, const std::wstring& stage) { PostProgress(permille, stage); },
        &g_cancel);

    auto* payload = new uploader::Outcome(std::move(result));
    ::PostMessageW(g_hMain, WM_APP_UPLOAD_DONE, 0, (LPARAM)payload);
}

void DownloadThread(std::wstring dir, std::wstring password) {
    g_cancel.store(false);
    ::PostMessageW(g_hMain, WM_APP_SET_BUSY, 1, 0);
    g_lastDir = dir;

    uploader::Outcome result = uploader::Download(
        dir, password,
        [](const std::wstring& s) { PostLog(s); },
        [](int permille, const std::wstring& stage) { PostProgress(permille, stage); },
        &g_cancel);

    auto* payload = new uploader::Outcome(std::move(result));
    ::PostMessageW(g_hMain, WM_APP_UPLOAD_DONE, 0, (LPARAM)payload);
}

void ConnThread() {
    uploader::HealthResult res = uploader::CheckBackend(
        [](const std::wstring& s) { PostLog(s); });
    auto* payload = new uploader::HealthResult(std::move(res));
    ::PostMessageW(g_hMain, WM_APP_CONN, 0, (LPARAM)payload);
}

// 随机生成密码查重：问后端该密码是否已被使用
void CheckPwdThread(std::wstring password) {
    bool exists = uploader::PasswordExists(password);
    auto* payload = new PwdCheckResult{ std::move(password), exists };
    ::PostMessageW(g_hMain, WM_APP_PWD_CHECK, 0, (LPARAM)payload);
}

// ---------------------------------------------------------------------------
// 内嵌 HTML 资源读取
// ---------------------------------------------------------------------------
std::wstring LoadAppHtml() {
    // RC 里 HTML 是预定义类型 RT_HTML(23)，必须用宏而非字符串 "HTML" 才能匹配
    HRSRC hRes = ::FindResourceW(g_hInst, MAKEINTRESOURCE(IDR_APP_HTML), RT_HTML);
    if (!hRes) {
        ::OutputDebugStringW(OBFW("W0xvYWRBcHBIdG1sXSBGaW5kUmVzb3VyY2VXKElEPUlEUl9BUFBfSFRNTCwgUlRfSFRNTCkg5aSx6LSl77yM6LWE5rqQ5pyq5bWM5YWlIGV4ZeOAggo="));
        return L"";
    }
    HGLOBAL hGlob = ::LoadResource(g_hInst, hRes);
    if (!hGlob) {
        ::OutputDebugStringW(OBFW("W0xvYWRBcHBIdG1sXSBMb2FkUmVzb3VyY2Ug5aSx6LSl44CCCg=="));
        return L"";
    }
    DWORD size = ::SizeofResource(g_hInst, hRes);
    if (size == 0) {
        ::OutputDebugStringW(OBFW("W0xvYWRBcHBIdG1sXSBTaXplb2ZSZXNvdXJjZSDov5Tlm54gMO+8jEhUTUwg6LWE5rqQ5Li656m644CCCg=="));
        return L"";
    }
    const char* data = (const char*)::LockResource(hGlob);
    if (!data) {
        ::OutputDebugStringW(OBFW("W0xvYWRBcHBIdG1sXSBMb2NrUmVzb3VyY2Ug5aSx6LSl44CCCg=="));
        return L"";
    }
    std::string utf8(data, (size_t)size);
    std::wstring html = util::Utf8ToWide(utf8);
    if (html.empty()) {
        ::OutputDebugStringW(OBFW("W0xvYWRBcHBIdG1sXSBVdGY4VG9XaWRlIOe7k+aenOS4uuepuuOAggo="));
    } else {
        std::wstring msg = OBFW("W0xvYWRBcHBIdG1sXSDmiJDlip/or7vlj5YgSFRNTO+8jOWFsSA=") + std::to_wstring(html.size()) + OBFW("IOS4quWuveWtl+espuOAggo=");
        ::OutputDebugStringW(msg.c_str());
    }
    return html;
}

// 将 HTML 写入临时文件并返回 file:// URI；失败返回空串
static std::wstring WriteHtmlToTemp(const std::wstring& html) {
    wchar_t tmpDir[MAX_PATH];
    if (!::GetTempPathW(MAX_PATH, tmpDir)) return L"";
    wchar_t tmpFile[MAX_PATH];
    if (!::GetTempFileNameW(tmpDir, OBFW("aGJ1aQ=="), 0, tmpFile)) return L"";

    std::wstring path(tmpFile);
    auto pos = path.rfind(L'.');
    if (pos != std::wstring::npos) path = path.substr(0, pos) + OBFW("Lmh0bWw=");
    else path += OBFW("Lmh0bWw=");

    std::string u8 = util::WideToUtf8(html);
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD written = 0;
    BOOL ok = ::WriteFile(h, u8.data(), (DWORD)u8.size(), &written, nullptr);
    ::CloseHandle(h);
    if (!ok || written != u8.size()) return L"";

    std::wstring url = OBFW("ZmlsZTovLy8=");
    for (wchar_t c : path) url.push_back(c == L'\\' ? L'/' : c);
    return url;
}

// 先尝试 NavigateToString；若失败，把 HTML 落盘临时文件再用 file:// 导航
static HRESULT NavigateWithFallback(ICoreWebView2* webview, const std::wstring& html) {
    HRESULT hr = webview->NavigateToString(html.c_str());
    if (SUCCEEDED(hr)) return hr;

    ::OutputDebugStringW(OBFW("W1dlYlZpZXcyXSBOYXZpZ2F0ZVRvU3RyaW5nIOWksei0pe+8jOWwneivleWGmeWFpeS4tOaXtuaWh+S7tueUqCBmaWxlOi8vIOWbnumAgOOAggo="));
    std::wstring url = WriteHtmlToTemp(html);
    if (!url.empty()) {
        hr = webview->Navigate(url.c_str());
        if (FAILED(hr)) {
            ::OutputDebugStringW(OBFW("W1dlYlZpZXcyXSBmaWxlOi8vIOWbnumAgOS5n+Wksei0peOAggo="));
        }
    } else {
        ::OutputDebugStringW(OBFW("W1dlYlZpZXcyXSDml6Dms5XliJvlu7rkuLTml7YgSFRNTCDmlofku7bjgIIK"));
    }
    return hr;
}

// ---------------------------------------------------------------------------
// 桥接：页面 -> native
// ---------------------------------------------------------------------------
void HandlePageMessage(const std::wstring& msg) {
    std::string u8 = util::WideToUtf8(msg);
    auto j = json::Parse(u8);
    if (!j || !j->IsObject()) return;
    std::string type = j->GetStr("type");

    if (type == "ready") {
        g_pageReady = true;
        for (auto& p : g_pending)
            if (g_webview) g_webview->PostWebMessageAsString(p.c_str());
        g_pending.clear();
        // 启动日志打印构建时间戳（由 CMake 注入 BUILD_TIMESTAMP 宏），
        // 便于一眼区分当前运行的是哪次构建的 exe——
        // 排查「到底换没换 exe」的利器：时间戳对不上就是还在跑旧构建。
#ifdef BUILD_TIMESTAMP
        PostLog(std::wstring(L"构建时间：") + util::Utf8ToWide(BUILD_TIMESTAMP));
#endif
        // 载入记住的目录与密码，随 init 一并回传，方便重开软件直接下载
        config::AppState st; config::LoadState(st);
        PostJson(BuildInit(st.password));
        // 把软件图标（base64）推给页面，供头部图标使用
        if (!g_appIconB64.empty()) PostJson(BuildIcon(g_appIconB64));
        // 回填上次记住的 saves 目录（若仍有效）
        if (!st.savesDir.empty()) {
            bool has = lolfind::HasMarker(st.savesDir);
            if (has) g_lastDir = st.savesDir;   // 记住回填的目录，关闭时不会被覆盖成空
            PostJson(BuildDir(st.savesDir, has ? 1 : 2,
                has ? (OBFW("4oiaIOW3suiusOS9j+S4iuasoeebruW9le+8mg==") + st.savesDir)
                    : (OBFW("w5cg6K6w5L2P55qE55uu5b2V5bey5aSx5pWI77yM6K+36YeN5paw6YCJ5oup"))));
        }
    }
    else if (type == "health") {
        std::thread(ConnThread).detach();
    }
    else if (type == "detect") {
        std::thread(DetectThread).detach();
    }
    else if (type == "pick") {
        std::wstring p = PickFolder(g_hMain);
        if (!p.empty()) {
            bool has = lolfind::HasMarker(p);
            if (has) g_lastDir = p;   // 手动选择的目录也要记住，关闭时写入 ini
            int vs = has ? 1 : 2;
            std::wstring vt = has ? (L"√ 校验通过，已找到 " MARKER_FILE_W)
                                  : (L"× 目录中没有 " MARKER_FILE_W L"，不是有效的 saves 目录");
            PostJson(BuildDir(p, vs, vt));
            PostLog(OBFW("5bey5omL5Yqo6YCJ5oup77ya") + p);
            if (!has) PostLog(L"注意：该目录里没有 " MARKER_FILE_W L"，无法上传");
        }
    }
    else if (type == "upload") {
        std::wstring dir = util::Utf8ToWide(j->GetStr("dir"));
        std::wstring pwd = util::Utf8ToWide(j->GetStr("password"));
        g_lastDir = dir;
        g_currentPwd = pwd;
        if (dir.empty() || !lolfind::HasMarker(dir)) {
            PostJson(BuildDone(false, false, false, false,
                L"当前目录无效，必须是包含 " MARKER_FILE_W L" 的 saves 目录",
                L"", OBFW("5peg5rOV5LiK5Lyg"), OBFW("5peg5rOV5LiK5Lyg"), false));
            return;
        }
        if (!util::IsValidPassword(pwd)) {
            PostJson(BuildDone(false, false, false, false,
                OBFW("6K+35YWI5Zyo5a+G56CB5qGG6L6T5YWlIDQtMjQg5L2N5a2X5q+N5oiW5pWw5a2X5a+G56CB"),
                L"", OBFW("57y65bCR5a+G56CB"), OBFW("57y65bCR5a+G56CB"), false));
            return;
        }
        std::thread(UploadThread, dir, pwd).detach();
    }
    else if (type == "download") {
        std::wstring dir = util::Utf8ToWide(j->GetStr("dir"));
        std::wstring pwd = util::Utf8ToWide(j->GetStr("password"));
        g_lastDir = dir;
        g_currentPwd = pwd;
        if (dir.empty() || !util::DirectoryExists(dir)) {
            PostJson(BuildDone(false, false, true, false,
                OBFW("6K+35YWI6YCJ5oup6KaB6Kej5Y6L5Yiw55qEIHNhdmVzIOebruW9lQ=="), L"", OBFW("57y65bCR55uu5b2V"), OBFW("57y65bCR55uu5b2V"), false));
            return;
        }
        if (!lolfind::HasMarker(dir)) {
            PostJson(BuildDone(false, false, true, false,
                L"该目录没有 " MARKER_FILE_W L"，不是有效的 saves 目录",
                L"", OBFW("55uu5b2V5peg5pWI"), OBFW("55uu5b2V5peg5pWI"), false));
            return;
        }
        if (!util::IsValidPassword(pwd)) {
            PostJson(BuildDone(false, false, true, false,
                OBFW("6K+35YWI6L6T5YWlIDQtMjQg5L2N5a2X5q+N5oiW5pWw5a2X5a+G56CB"), L"", OBFW("57y65bCR5a+G56CB"), OBFW("57y65bCR5a+G56CB"), false));
            return;
        }
        std::thread(DownloadThread, dir, pwd).detach();
    }
    else if (type == "copy") {
        std::wstring text = util::Utf8ToWide(j->GetStr("text"));
        util::CopyTextToClipboard(g_hMain, text);
        PostLog(OBFW("57uT5p6c5bey5aSN5Yi25Yiw5Ymq6LS05p2/"));
    }
    else if (type == "minimize") {
        ::ShowWindow(g_hMain, SW_MINIMIZE);
    }
    else if (type == "close") {
        ::PostMessageW(g_hMain, WM_CLOSE, 0, 0);
    }
    else if (type == "maximize") {
        if (::IsZoomed(g_hMain)) ::ShowWindow(g_hMain, SW_RESTORE);
        else ::ShowWindow(g_hMain, SW_MAXIMIZE);
    }
    else if (type == "drag") {
        ::ReleaseCapture();
        ::SendMessageW(g_hMain, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
    }
    else if (type == OBFA("Y2hlY2tQYXNzd29yZA==")) {
        // 仅「随机生成密码」按钮会发此消息：生成一个后端确认未使用过的密码。
        // 手动输入密码不再查重（同密码上传 = 覆盖更新原存档）。
        std::wstring pwd = util::Utf8ToWide(j->GetStr("password"));
        if (!pwd.empty()) std::thread(CheckPwdThread, pwd).detach();
    }
    else if (type == "password") {
        // 页面密码框每次变化都回传，关闭时据此写入 uploader.ini（为空即写空）
        g_currentPwd = util::Utf8ToWide(j->GetStr("password"));
    }
}

// ---------------------------------------------------------------------------
// 结果处理（Outcome -> 页面）
// ---------------------------------------------------------------------------
void HandleOutcome(uploader::Outcome* r) {
    if (!r) return;

    std::wstring resultText, resultLabel, stage;
    bool ok = r->ok, canceled = r->canceled, isDownload = r->isDownload, pwdWrong = r->passwordWrong;
    bool copyEnabled = false;

    if (canceled) {
        stage = OBFW("5bey5Y+W5raI");
    } else if (!ok) {
        if (pwdWrong) {
            stage = OBFW("5a+G56CB6ZSZ6K+v");
            resultText = OBFW("5a+G56CB5LiN5q2j56Gu77yM5peg5rOV6Kej5a+G6K+l5Y6L57yp5YyF44CCDQror7fnoa7orqTkuIvovb3ml7bkvb/nlKjnmoTlr4bnoIHkuI7kuIrkvKDml7bkuIDoh7TjgII=");
        } else {
            stage = isDownload ? OBFW("5LiL6L295aSx6LSl") : OBFW("5LiK5Lyg5aSx6LSl");
            resultText = r->error;
        }
    } else if (isDownload) {
        resultText  = OBFW("5bey6Kej5Y6L5Yiw77yaIA==") + g_lastDir + OBFW("DQo=");
        resultText += OBFW("5oGi5aSN5paH5Lu277yaIA==") + std::to_wstring((unsigned long long)r->extractedFiles) + OBFW("IOS4qg0K");
        resultText += OBFW("5LiL6L295aSn5bCP77yaIA==") + util::FormatSize(r->downloadedBytes) + OBFW("DQo=");
        resultText += OBFW("6Kej5Y6L5aSn5bCP77yaIA==") + util::FormatSize(r->rawBytes);
        resultLabel = OBFW("5LiL6L2957uT5p6c");
        stage = OBFW("5LiL6L296Kej5Y6L5a6M5oiQ");
        copyEnabled = true;
    } else {
        // 上传结果只展示必要信息：密码 + 压缩包大小（含原始/文件数），
        // 以及备份目录、完成时间。对象 Key / 下载链接 / 有效期 不展示——
        // 下载链接在用户凭密码下载时才由服务端按需生成，无需提前给出。
        resultText  = OBFW("5a+G56CB77yaICAgICAgIA==") + r->password + OBFW("DQo=");
        resultText += OBFW("5Y6L57yp5YyF5aSn5bCP77yaIA==") + util::FormatSize(r->zipBytes) +
                      OBFW("77yI5Y6f5aeLIA==") + util::FormatSize(r->rawBytes) + OBFW("77yM5YWxIA==") +
                      std::to_wstring((unsigned long long)r->fileCount) + OBFW("IOS4quaWh+S7tu+8iQ0K");
        resultText += OBFW("5aSH5Lu955uu5b2V77yaICAg") + g_lastDir + OBFW("DQo=");
        resultText += OBFW("5a6M5oiQ5pe26Ze077yaICAg") + NowString();
        resultLabel = OBFW("5LiK5Lyg57uT5p6c77yI6K+35L+d5a2Y5a+G56CB77yJ");
        stage = OBFW("5LiK5Lyg5a6M5oiQ");
        copyEnabled = true;
    }

    if (ok && !canceled) {
        g_lastResultText = resultText; g_uploadDone.store(true);
    }

    if (ok && !canceled && !isDownload) {
        util::CopyTextToClipboard(g_hMain, resultText);
        PostLog(OBFW("PT09PT0g5LiK5Lyg5oiQ5Yqf77yM6K+356uL5Y2z5L+d5a2Y5LiL5pa55a+G56CBID09PT09"));
        PostLog(OBFW("57uT5p6c5bey6Ieq5Yqo5aSN5Yi25Yiw5Ymq6LS05p2/"));
    } else if (ok && !canceled && isDownload) {
        PostLog(OBFW("PT09PT0g5LiL6L295bm26Kej5Y6L5a6M5oiQ77yM5paH5Lu25bey6KaG55uW6Iez55uu5qCH55uu5b2VID09PT09"));
    } else if (!ok && !canceled) {
        if (pwdWrong) PostLog(OBFW("5a+G56CB6ZSZ6K+v77yM5peg5rOV6Kej5a+G5Y6L57yp5YyF"));
        else PostLog(OBFW("5pON5L2c5aSx6LSl77ya") + r->error);
    } else if (canceled) {
        PostLog(OBFW("5pON5L2c5bey5Y+W5raI"));
    }

    PostJson(BuildDone(ok, canceled, isDownload, pwdWrong, r->error,
                       resultText, resultLabel, stage, copyEnabled));
    delete r;
    PostJson(BuildBusy(false));
}

// ---------------------------------------------------------------------------
// WebView2 初始化
// ---------------------------------------------------------------------------
void InitWebView2(HWND hwnd) {
    // userDataFolder 必须存在，但绝不放程序目录（避免污染安装位置、被误删、或被便携化工具打包带走）。
    // 重定向到系统临时目录：%TEMP%\HanbotWebView2。WebView2 运行时数据（缓存/Cookie/GPU 缓存等）都在此，
    // 程序目录不再生成任何 webview2_cache 文件夹。注：WebView2 必须有 userDataFolder，无法彻底"不生成"，
    // 只能移走；临时目录下的该文件夹可被系统清理策略安全回收。
    std::wstring udf = util::GetTempDir() + OBFW("XEhhbmJvdFdlYlZpZXcy");
    ::CreateDirectoryW(udf.c_str(), nullptr);

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, udf.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr)) {
                    ::MessageBoxW(hwnd,
                        OBFW("V2ViVmlldzIg5Yid5aeL5YyW5aSx6LSl77ya5pyq6IO95Yib5bu6546v5aKD44CCDQror7fnoa7orqTns7vnu5/lt7Llronoo4UgV2ViVmlldzIgUnVudGltZe+8iEVkZ2Ug5rWP6KeI5Zmo6Ieq5bim77yM5oiW5Yiw5b6u6L2v5a6Y572R5LiL6L295a6J6KOF77yJ44CC"),
                        APP_TITLE_W, MB_OK | MB_ICONERROR);
                    return hr;
                }
                env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(hr) || !ctrl) {
                                ::MessageBoxW(hwnd, OBFW("V2ViVmlldzIg5Yid5aeL5YyW5aSx6LSl77ya5pyq6IO95Yib5bu65o6n5Yi25Zmo44CC"),
                                               APP_TITLE_W, MB_OK | MB_ICONERROR);
                                return hr;
                            }
                            g_controller = ctrl;
                            ctrl->get_CoreWebView2(&g_webview);
                            if (!g_webview) return E_FAIL;

                            // 精装环境选项：禁右键/DevTools/状态栏/缩放
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webview->get_Settings(&settings)) && settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);

                                // 以下三项在派生接口（Settings4/5），需 As() 做 QueryInterface；
                                // 旧版 SDK 若没有这些接口则跳过，不影响核心功能。
                                ComPtr<ICoreWebView2Settings4> s4;
                                if (SUCCEEDED(settings.As(&s4)) && s4) {
                                    s4->put_IsGeneralAutofillEnabled(FALSE);
                                    s4->put_IsPasswordAutosaveEnabled(FALSE);
                                }
                                // 注：AreBrowserExtensionsEnabled 在最新 SDK 中已挪到 ICoreWebView2EnvironmentOptions6
                                // （环境级选项，需在创建环境时传入 options，而非运行时 setting），且对应用无意义，故不调用。
                            }
                            ctrl->put_ZoomFactor(1.0);
                            ctrl->put_IsVisible(TRUE);

                            // 消息桥接
                            EventRegistrationToken token{};
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        // 新版 WebView2 SDK 用 TryGetWebMessageAsString 取代老版 get_WebMessageAsString
                                        // （两者均为单参数 LPWSTR*）。用 __if_exists 编译期适配两套 SDK。
                                        __if_exists(ICoreWebView2WebMessageReceivedEventArgs::TryGetWebMessageAsString) {
                                            args->TryGetWebMessageAsString(&raw);
                                        }
                                        __if_not_exists(ICoreWebView2WebMessageReceivedEventArgs::TryGetWebMessageAsString) {
                                            args->get_WebMessageAsString(&raw);
                                        }
                                        std::wstring msg(raw ? raw : L"");
                                        if (raw) ::CoTaskMemFree(raw);
                                        HandlePageMessage(msg);
                                        return S_OK;
                                    }).Get(), &token);

                            // 铺满客户区
                            RECT rc{};
                            ::GetClientRect(hwnd, &rc);
                            ctrl->put_Bounds(rc);

                            // 载入内嵌 HTML
                            std::wstring html = LoadAppHtml();
                            HRESULT navHr = E_FAIL;
                            if (!html.empty()) {
                                navHr = NavigateWithFallback(g_webview.Get(), html);
                            }
                            if (html.empty() || FAILED(navHr)) {
                                std::wstring err = OBFW("5pyq6IO95Yqg6L295bqU55So55WM6Z2i44CCCg==");
                                if (html.empty())
                                    err += OBFW("5Y6f5Zug77ya5peg5rOV5LuOIGV4ZSDlhoXltYzotYTmupDor7vlj5YgSFRNTO+8iElEUl9BUFBfSFRNTCDlj6/og73nvLrlpLHmiJbkuLrnqbrvvInjgIIK");
                                else
                                    err += OBFW("5Y6f5Zug77yaTmF2aWdhdGVUb1N0cmluZyDkuI7kuLTml7bmlofku7blm57pgIDlnYflpLHotKXjgIIK");
                                err += OBFW("6K+35bCd6K+V6YeN5paw5p6E5bu677yM5oiW6IGU57O75oqA5pyv5pSv5oyB44CC");
                                ::MessageBoxW(hwnd, err.c_str(), APP_TITLE_W, MB_OK | MB_ICONERROR);
                            }

                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    (void)hr;
}

// ---------------------------------------------------------------------------
// 圆角窗口区域
// ---------------------------------------------------------------------------
void UpdateWindowRgn(HWND hwnd) {
    if (!g_useRgn) return;
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    HRGN rgn = ::CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom, S(16), S(16));
    if (rgn) {
        ::SetWindowRgn(hwnd, rgn, TRUE);
        ::DeleteObject(rgn);
    }
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {

    case WM_CREATE: {
        g_hMain = hwnd;
        g_dpi = QueryWindowDpi(hwnd);
        if (g_dpi <= 0) g_dpi = 96;
        g_titleH = S(42);

        // Win11：用 DWM 原生圆角（自带阴影）；Win10：用 rgn + CS_DROPSHADOW
        int pref = 2; // DWMWCP_ROUND
        if (SUCCEEDED(::DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                                             &pref, sizeof(pref))))
            g_useRgn = false;

        UpdateWindowRgn(hwnd);

        // 启动工作线程：后端连接检测 / saves 目录检测
        // WebView2 初始化移到 ShowWindow 之后，确保父窗口已可见，避免部分机器上控制器创建后白屏
        std::thread(ConnThread).detach();
        // 本机已记住有效 saves 目录 → 跳过全盘扫描（避免每次打开软件都扫盘）；
        // 只有没记住或记住的目录已失效时才自动检测。
        config::AppState st0; config::LoadState(st0);
        bool haveSaved = !st0.savesDir.empty() && lolfind::HasMarker(st0.savesDir);
        if (!haveSaved) {
            std::thread(DetectThread).detach();
        }
        return 0;
    }

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        g_titleH = S(42);
        RECT* nr = (RECT*)lp;
        ::SetWindowPos(hwnd, nullptr, nr->left, nr->top,
                       nr->right - nr->left, nr->bottom - nr->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateWindowRgn(hwnd);
        return 0;
    }

    case WM_SIZE:
        if (g_controller) {
            RECT rc{};
            ::GetClientRect(hwnd, &rc);
            g_controller->put_Bounds(rc);
        }
        UpdateWindowRgn(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(680);
        mmi->ptMinTrackSize.y = S(640);
        return 0;
    }

    case WM_APP_LOG: {
        auto* s = (std::wstring*)lp;
        if (s) { PostJson(BuildLog(*s)); delete s; }
        return 0;
    }

    case WM_APP_PROGRESS: {
        int permille = (int)wp;
        if (permille < 0) permille = 0;
        if (permille > 1000) permille = 1000;
        std::wstring stage;
        auto* s = (std::wstring*)lp;
        if (s) { stage = *s; delete s; }
        PostJson(BuildProgress(permille, stage));
        return 0;
    }

    case WM_APP_DETECT_DONE: {
        auto* results = (std::vector<lolfind::Candidate>*)lp;
        std::wstring path; int vs = 0; std::wstring vt;
        if (!results || results->empty()) {
            vs = 2; vt = OBFW("w5cg6Ieq5Yqo5qOA5rWL5pyq5om+5Yiw55uu5b2V77yM6K+35omL5Yqo6YCJ5oup");
        } else {
            path = (*results)[0].savesPath;
            if (!path.empty()) g_lastDir = path;   // 记住扫描到的目录，关闭时写入 ini，避免下次再全盘扫
            vs = lolfind::HasMarker(path) ? 1 : 2;
            vt = vs == 1 ? (L"√ 校验通过，已找到 " MARKER_FILE_W)
                         : (OBFW("w5cg6K+l55uu5b2V5pyq6YCa6L+H5qCh6aqM"));
        }
        delete results;
        PostJson(BuildDetectDone(path, vs, vt));
        PostJson(BuildBusy(false));
        return 0;
    }

    case WM_APP_UPLOAD_DONE:
        HandleOutcome((uploader::Outcome*)lp);
        return 0;

    case WM_APP_SET_BUSY:
        PostJson(BuildBusy(wp != 0));
        return 0;

    case WM_APP_CONN: {
        auto* r = (uploader::HealthResult*)lp;
        if (r) {
            PostJson(BuildConn(r->ok ? 1 : 2, (r->ok ? OBFW("4pyTIA==") : OBFW("w5cg")) + r->message));
            delete r;
        }
        return 0;
    }

    case WM_APP_PWD_CHECK: {
        auto* p = (PwdCheckResult*)lp;
        if (p) {
            PostJson(BuildPasswordChecked(p->password, p->exists));
            delete p;
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_busy.load()) {
            const int r = ::MessageBoxW(hwnd,
                OBFW("5Lu75Yqh5q2j5Zyo6L+b6KGM5Lit77yM56Gu5a6a6KaB5Lit5q2i5bm26YCA5Ye65ZCX77yf"),
                OBFW("56Gu6K6k6YCA5Ye6"), MB_YESNO | MB_ICONQUESTION);
            if (r != IDYES) return 0;
            g_cancel.store(true);
            ::Sleep(200);
        }
        // 关闭时把当前目录与密码写入 uploader.ini 的 [State] 段（密码为空则写空），方便重开直接下载
        config::SaveState(g_lastDir, g_currentPwd);
        if (g_uploadDone.load() && !g_lastResultText.empty()) {
            const int r = ::MessageBoxW(hwnd,
                OBFW("5b2T5YmN55uu5b2V5LiO5a+G56CB5bey6Ieq5Yqo5L+d5a2Y5Yiw5pys5py677yM56Gu6K6k6YCA5Ye677yf"),
                OBFW("56Gu6K6k6YCA5Ye6"), MB_YESNO | MB_ICONINFORMATION);
            if (r != IDYES) return 0;
        }
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_cancel.store(true);
        ::PostQuitMessage(0);
        return 0;

    default: break;
    }

    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// DPI 感知（运行时探测，兼容老系统）
// ---------------------------------------------------------------------------
void EnableDpiAwareness() {
    HMODULE user32 = ::GetModuleHandleW(OBFW("dXNlcjMyLmRsbA=="));
    if (user32) {
        using SetCtxFn = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setCtx = (SetCtxFn)::GetProcAddress(user32, OBFA("U2V0UHJvY2Vzc0RwaUF3YXJlbmVzc0NvbnRleHQ="));
        if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }

    HMODULE shcore = ::LoadLibraryW(OBFW("c2hjb3JlLmRsbA=="));
    if (shcore) {
        using SetAwareFn = HRESULT (WINAPI*)(int);
        auto setAware = (SetAwareFn)::GetProcAddress(shcore, OBFA("U2V0UHJvY2Vzc0RwaUF3YXJlbmVzcw=="));
        if (setAware) { setAware(2 /* PROCESS_PER_MONITOR_DPI_AWARE */); ::FreeLibrary(shcore); return; }
        ::FreeLibrary(shcore);
    }

    ::SetProcessDPIAware();
}

} // namespace

// ===========================================================================
// 入口
// ===========================================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    g_hInst = hInst;

    // 预读取软件图标（base64），待页面 ready 后注入头部
    g_appIconB64 = LoadAppIconBase64();

    EnableDpiAwareness();
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_WIN95_CLASSES };
    ::InitCommonControlsEx(&icc);

    config::Load();
    config::WriteTemplateIfMissing();

    // 只允许运行一个实例
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, OBFW("R2xvYmFsXEhhbmJvdFNhdmVzVXBsb2FkZXJfU2luZ2xlSW5zdGFuY2U="));
    if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND exist = ::FindWindowW(OBFW("SGFuYm90VXBsb2FkZXJXbmRDbGFzcw=="), nullptr);
        if (exist) {
            ::ShowWindow(exist, SW_RESTORE);
            ::SetForegroundWindow(exist);
        }
        return 0;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = OBFW("SGFuYm90VXBsb2FkZXJXbmRDbGFzcw==");
    wc.hIcon         = ::LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm       = ::LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP_ICON));

    if (!::RegisterClassExW(&wc)) {
        ::MessageBoxW(nullptr, OBFW("56qX5Y+j5rOo5YaM5aSx6LSl"), APP_TITLE_W, MB_OK | MB_ICONERROR);
        return 1;
    }

    // 依据主显示器 DPI 预估初始窗口大小
    {
        HDC screen = ::GetDC(nullptr);
        if (screen) {
            const int dpi = ::GetDeviceCaps(screen, LOGPIXELSX);
            if (dpi > 0) g_dpi = dpi;
            ::ReleaseDC(nullptr, screen);
        }
    }

    // 期望的客户区尺寸（逻辑像素，S() 按 DPI 放大）。
    // 高度给到 800：页面内容（连接条 + 目录 + 密码 + 双按钮 + 进度 + 5 行日志 + 结果区 + 页脚）
    // 大约需要 700px，留一点余量，避免下半部被挤压后互相重叠。
    RECT wr{ 0, 0, S(760), S(800) };
    ::AdjustWindowRectEx(&wr, WS_POPUP | WS_SYSMENU, FALSE, 0);

    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    // 夹到桌面工作区内：150% / 175% 缩放的小屏笔记本上，S(800) 可能比屏幕还高，
    // 不夹取的话窗口会跑出屏幕，页面被压扁，控件就叠在一起了。
    int x = 0, y = 0;
    RECT wa{};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0) &&
        wa.right > wa.left && wa.bottom > wa.top) {
        const int availW = wa.right - wa.left;
        const int availH = wa.bottom - wa.top;
        if (winW > availW) winW = availW;
        if (winH > availH) winH = availH;
        x = wa.left + (availW - winW) / 2;
        y = wa.top + (availH - winH) / 2;
    } else {
        x = (::GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
        y = (::GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    }

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, APP_TITLE_W,
                                  WS_POPUP | WS_SYSMENU,
                                  x >= 0 ? x : CW_USEDEFAULT,
                                  y >= 0 ? y : CW_USEDEFAULT,
                                  winW, winH,
                                  nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        ::MessageBoxW(nullptr, OBFW("56qX5Y+j5Yib5bu65aSx6LSl"), APP_TITLE_W, MB_OK | MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    // 窗口显示后再初始化 WebView2：确保父窗口已可见，规避部分环境下控制器创建后白屏
    InitWebView2(hwnd);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(hwnd, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    if (mutex) ::CloseHandle(mutex);
    ::CoUninitialize();

    return (int)msg.wParam;
}
