// ---------------------------------------------------------------------------
// Hanbot 存档打包上传器 —— 主程序（纯 Win32 API，无第三方 UI 框架）
//   二次元风格：自定义圆角窗口 + 樱粉/薰衣草渐变背景 + 自绘圆角按钮
// ---------------------------------------------------------------------------

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wingdi.h>

#include <string>
#include <vector>
#include <atomic>
#include <thread>

// 老版本 SDK 兜底定义
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
#ifndef FW_SEMIBOLD
#define FW_SEMIBOLD 600
#endif

#include "resource.h"
#include "config.h"
#include "util.h"
#include "lol_finder.h"
#include "uploader.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "msimg32.lib")

// Common Controls v6 的依赖声明写在 res/app.manifest 里，
// 由资源脚本统一嵌入（CMake 已关闭链接器自动生成清单）。

// ===========================================================================
// 全局状态
// ===========================================================================
namespace {

HINSTANCE g_hInst = nullptr;
HWND      g_hMain = nullptr;

HWND g_hHeader = nullptr, g_hSubHeader = nullptr;
HWND g_hPathLabel = nullptr, g_hPathEdit = nullptr;
HWND g_hBtnDetect = nullptr, g_hBtnBrowse = nullptr;
HWND g_hVerify = nullptr;
HWND g_hPwdLabel = nullptr, g_hPwdEdit = nullptr, g_hPwdHint = nullptr;
HWND g_hBtnUpload = nullptr, g_hBtnDownload = nullptr;
HWND g_hProgress = nullptr, g_hStage = nullptr;
HWND g_hLog = nullptr;
HWND g_hResultLabel = nullptr, g_hResult = nullptr;
HWND g_hBtnCopy = nullptr;
HWND g_hFooter = nullptr;

HFONT g_fontBase = nullptr;
HFONT g_fontTitle = nullptr;
HFONT g_fontTitleBar = nullptr;
HFONT g_fontButton = nullptr;
HFONT g_fontMono = nullptr;

HBRUSH g_brushBg = nullptr;
HBRUSH g_brushCard = nullptr;

int  g_dpi = 96;
int  g_titleH = 42;          // 标题栏高度（随 DPI 缩放）

std::atomic<bool> g_busy{ false };
std::atomic<bool> g_cancel{ false };
std::atomic<bool> g_uploadDone{ false };

std::wstring g_lastResultText;

// 标题栏按钮悬停
bool g_closeHot = false, g_minHot = false, g_titleTracking = false;

// ===========================================================================
// 二次元配色
// ===========================================================================
const COLORREF kColBgTop    = RGB(255, 224, 240);   // 樱粉
const COLORREF kColBgMid    = RGB(233, 217, 255);   // 薰衣草
const COLORREF kColBgBottom = RGB(214, 240, 255);   // 天空蓝

const COLORREF kColText    = RGB(58, 42, 77);       // 梅紫
const COLORREF kColSubText = RGB(124, 108, 146);    // 柔和紫灰
const COLORREF kColOk      = RGB(43, 182, 115);     // 薄荷绿
const COLORREF kColWarn    = RGB(229, 84, 122);     // 樱红

// 上传按钮（粉 -> 紫）
const COLORREF kUpTop = RGB(255, 158, 194), kUpBot = RGB(193, 139, 255);
const COLORREF kUpTopH = RGB(255, 182, 212), kUpBotH = RGB(207, 162, 255);
// 下载按钮（蓝 -> 紫）
const COLORREF kDlTop = RGB(155, 220, 255), kDlBot = RGB(185, 140, 255);
const COLORREF kDlTopH = RGB(178, 232, 255), kDlBotH = RGB(202, 162, 255);
// 幽灵按钮（白底紫边）
const COLORREF kGhostFill = RGB(255, 255, 255), kGhostFillH = RGB(255, 236, 246);
const COLORREF kGhostBorder = RGB(206, 176, 224), kGhostText = RGB(96, 64, 120);

inline int S(int v) { return ::MulDiv(v, g_dpi, 96); }

COLORREF Darken(COLORREF c, int f) {
    return RGB(GetRValue(c) * f / 100, GetGValue(c) * f / 100, GetBValue(c) * f / 100);
}

// GetDpiForWindow 是 Win10 1607+ 才有的，动态解析避免链接期/运行期报错
int QueryWindowDpi(HWND hwnd) {
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiFn = UINT (WINAPI*)(HWND);
        auto fn = (GetDpiFn)::GetProcAddress(user32, "GetDpiForWindow");
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
// 渐变 / 圆角绘制工具
// ---------------------------------------------------------------------------
bool VGrad(HDC dc, const RECT& r, COLORREF c1, COLORREF c2) {
    TRIVERTEX tv[2]{};
    tv[0].x = r.left;  tv[0].y = r.top;
    tv[0].Red = GetRValue(c1) << 8;  tv[0].Green = GetGValue(c1) << 8;  tv[0].Blue = GetBValue(c1) << 8;  tv[0].Alpha = 0xff00;
    tv[1].x = r.right; tv[1].y = r.bottom;
    tv[1].Red = GetRValue(c2) << 8;  tv[1].Green = GetGValue(c2) << 8;  tv[1].Blue = GetBValue(c2) << 8;  tv[1].Alpha = 0xff00;
    GRADIENT_RECT gr{}; gr.UpperLeft = 0; gr.LowerRight = 1;
    return ::GradientFill(dc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V) != 0;
}

void VGrad3(HDC dc, const RECT& rc, COLORREF c1, COLORREF c2, COLORREF c3) {
    const int mid = rc.top + (rc.bottom - rc.top) / 2;
    RECT r1 = { rc.left, rc.top, rc.right, mid };
    RECT r2 = { rc.left, mid, rc.right, rc.bottom };
    VGrad(dc, r1, c1, c2);
    VGrad(dc, r2, c2, c3);
}

void FillRoundRect(HDC dc, const RECT& r, int rad, COLORREF fill, COLORREF border) {
    HRGN rgn = ::CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, rad, rad);
    if (rgn) {
        HBRUSH fb = ::CreateSolidBrush(fill);
        ::FillRgn(dc, rgn, fb);
        if (border != CLR_NONE) {
            HPEN pen = ::CreatePen(PS_SOLID, 1, border);
            HGDIOBJ op = ::SelectObject(dc, pen);
            HGDIOBJ ob = (HGDIOBJ)::GetStockObject(NULL_BRUSH);
            ::SelectObject(dc, ob);
            ::RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
            ::SelectObject(dc, op);
            ::DeleteObject(pen);
        }
        ::DeleteObject(fb);
        ::DeleteObject(rgn);
    }
}

// 自绘渐变圆角按钮
void DrawGradientButton(HDC dc, const RECT& r, bool pressed, bool disabled,
                        COLORREF t, COLORREF b, COLORREF tH, COLORREF bH,
                        const wchar_t* text, HFONT font) {
    COLORREF ft = disabled ? Darken(t, 70) : (pressed ? Darken(t, 88) : tH);
    COLORREF fb = disabled ? Darken(b, 70) : (pressed ? Darken(b, 88) : bH);

    HRGN rgn = ::CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, S(12), S(12));
    if (rgn) {
        ::SelectClipRgn(dc, rgn);
        VGrad(dc, r, ft, fb);
        ::SelectClipRgn(dc, nullptr);
        ::DeleteObject(rgn);
    }

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, disabled ? RGB(255, 255, 255) : RGB(255, 255, 255));
    HGDIOBJ of = ::SelectObject(dc, font ? font : g_fontButton);
    RECT tr = r;
    if (pressed) ::OffsetRect(&tr, 0, S(1));
    ::DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, of);
}

// 自绘幽灵按钮（白底紫边）
void DrawGhostButton(HDC dc, const RECT& r, bool pressed, bool disabled,
                     const wchar_t* text, HFONT font) {
    COLORREF fill = disabled ? RGB(245, 240, 248) : (pressed ? kGhostFillH : kGhostFill);
    FillRoundRect(dc, r, S(10), fill, disabled ? RGB(225, 218, 232) : kGhostBorder);

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, disabled ? RGB(180, 170, 190) : kGhostText);
    HGDIOBJ of = ::SelectObject(dc, font ? font : g_fontButton);
    RECT tr = r;
    if (pressed) ::OffsetRect(&tr, 0, S(1));
    ::DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, of);
}

// 标题栏上的圆形小按钮（最小/关闭）
RECT CloseRect(int W) {
    const int bw = S(26), bh = S(24), top = S(9), pad = S(14);
    return { W - pad - bw, top, W - pad, top + bh };
}
RECT MinRect(int W) {
    const int bw = S(26), bh = S(24), top = S(9), pad = S(14), gap = S(8);
    return { W - pad - bw * 2 - gap, top, W - pad - bw - gap, top + bh };
}

void DrawCaptionButton(HDC dc, const RECT& r, bool hover, bool isClose) {
    FillRoundRect(dc, r, S(7),
                  hover ? (isClose ? RGB(255, 180, 200) : RGB(225, 210, 240))
                        : RGB(255, 255, 255),
                  hover ? (isClose ? RGB(240, 150, 175) : RGB(200, 180, 225))
                        : RGB(225, 218, 232));

    // 画图标
    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2;
    int d = S(5);
    HPEN pen = ::CreatePen(PS_SOLID, S(2), isClose ? RGB(200, 70, 100) : RGB(110, 80, 140));
    HGDIOBJ op = ::SelectObject(dc, pen);
    if (isClose) {
        ::MoveToEx(dc, cx - d, cy - d, nullptr);
        ::LineTo(dc, cx + d, cy + d);
        ::MoveToEx(dc, cx + d, cy - d, nullptr);
        ::LineTo(dc, cx - d, cy + d);
    } else {
        ::MoveToEx(dc, cx - d, cy, nullptr);
        ::LineTo(dc, cx + d, cy);
    }
    ::SelectObject(dc, op);
    ::DeleteObject(pen);
}

// ---------------------------------------------------------------------------
// 跨线程投递
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
// UI 小工具
// ---------------------------------------------------------------------------
void AppendLog(const std::wstring& text) {
    if (!g_hLog) return;

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t ts[32]{};
    ::swprintf(ts, 32, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

    std::wstring line = ts + text + L"\r\n";

    const int len = ::GetWindowTextLengthW(g_hLog);
    ::SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    ::SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    ::SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

void SetVerifyState(int state, const std::wstring& text) {
    // state: 0 未知 / 1 通过 / 2 失败
    ::SetWindowTextW(g_hVerify, text.c_str());
    ::SetWindowLongPtrW(g_hVerify, GWLP_USERDATA, (LONG_PTR)state);
    ::InvalidateRect(g_hVerify, nullptr, TRUE);
}

void SetPwdHint(int state, const std::wstring& text) {
    // state: 0 提示 / 1 有效 / 2 无效
    ::SetWindowTextW(g_hPwdHint, text.c_str());
    ::SetWindowLongPtrW(g_hPwdHint, GWLP_USERDATA, (LONG_PTR)state);
    ::InvalidateRect(g_hPwdHint, nullptr, TRUE);
}

std::wstring GetPathFromEdit() {
    const int len = ::GetWindowTextLengthW(g_hPathEdit);
    if (len <= 0) return L"";
    std::wstring s((size_t)len + 1, L'\0');
    ::GetWindowTextW(g_hPathEdit, &s[0], len + 1);
    s.resize((size_t)len);
    return util::Trim(s);
}

std::wstring GetPasswordFromEdit() {
    const int len = ::GetWindowTextLengthW(g_hPwdEdit);
    if (len <= 0) return L"";
    std::wstring s((size_t)len + 1, L'\0');
    ::GetWindowTextW(g_hPwdEdit, &s[0], len + 1);
    s.resize((size_t)len);
    return s;
}

void UpdatePwdHint() {
    const std::wstring p = GetPasswordFromEdit();
    if (p.empty()) {
        SetPwdHint(0, L"请输入 4~24 位字母或数字，作为压缩包密码（上传/下载都需用同一密码）");
        return;
    }
    if (util::IsValidPassword(p))
        SetPwdHint(1, L"✓ 密码格式有效");
    else
        SetPwdHint(2, L"✗ 需 4~24 位字母或数字（仅限 A-Z a-z 0-9）");
}

// 前向声明：FilterPasswordEdit 内部会调用，定义在下方
void RefreshButtons();

// 实时过滤密码框：仅保留字母数字、最长 24 位
void FilterPasswordEdit() {
    HWND h = g_hPwdEdit;
    const int len = ::GetWindowTextLengthW(h);
    std::wstring s((size_t)len + 1, L'\0');
    ::GetWindowTextW(h, &s[0], len + 1);
    s.resize((size_t)len);

    std::wstring out;
    bool changed = false;
    for (wchar_t c : s) {
        if (out.size() >= 24) { changed = true; break; }
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        if (ok) out += c; else changed = true;
    }

    if (changed) {
        ::SetWindowTextW(h, out.c_str());
        ::SendMessageW(h, EM_SETSEL, (WPARAM)out.size(), (LPARAM)out.size());
    }
    UpdatePwdHint();
    RefreshButtons();
}

// 根据目录与密码状态，启用/禁用上传、下载按钮
void RefreshButtons() {
    const std::wstring dir = GetPathFromEdit();
    const bool dirValid  = !dir.empty() && util::DirectoryExists(dir) && lolfind::HasMarker(dir);
    const bool dirExists = !dir.empty() && util::DirectoryExists(dir);
    const bool pwdOk     = util::IsValidPassword(GetPasswordFromEdit());

    ::EnableWindow(g_hBtnUpload,   !g_busy.load() && dirValid  && pwdOk);
    ::EnableWindow(g_hBtnDownload, !g_busy.load() && dirExists && pwdOk);
}

void RefreshVerify() {
    const std::wstring p = GetPathFromEdit();
    if (p.empty()) {
        SetVerifyState(0, L"尚未选择目录");
    } else if (!util::DirectoryExists(p)) {
        SetVerifyState(2, L"× 目录不存在");
    } else if (!lolfind::HasMarker(p)) {
        SetVerifyState(2, L"× 目录中没有 " MARKER_FILE_W L"，不是有效的 saves 目录");
    } else {
        SetVerifyState(1, L"√ 校验通过，已找到 " MARKER_FILE_W);
    }
    RefreshButtons();
}

void SetBusy(bool busy) {
    g_busy.store(busy);
    ::EnableWindow(g_hBtnDetect, busy ? FALSE : TRUE);
    ::EnableWindow(g_hBtnBrowse, busy ? FALSE : TRUE);
    ::EnableWindow(g_hPathEdit,  busy ? FALSE : TRUE);
    ::EnableWindow(g_hPwdEdit,   busy ? FALSE : TRUE);
    if (!busy) RefreshVerify();
    else { ::EnableWindow(g_hBtnUpload, FALSE); ::EnableWindow(g_hBtnDownload, FALSE); }
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
        dlg->SetTitle(L"请选择 League of Legends 的 saves 目录");

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

    // 兜底：老式对话框
    BROWSEINFOW bi{};
    wchar_t buf[MAX_PATH]{};
    bi.hwndOwner = owner;
    bi.pszDisplayName = buf;
    bi.lpszTitle = L"请选择 saves 目录";
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
// 工作线程
// ---------------------------------------------------------------------------
void DetectThread() {
    g_cancel.store(false);
    ::PostMessageW(g_hMain, WM_APP_SET_BUSY, 1, 0);
    PostProgress(0, L"正在检测目录...");
    PostLog(L"开始检测 saves 目录...");

    auto results = lolfind::FindAll(
        [](const std::wstring& s) { PostLog(s); },
        &g_cancel,
        60);

    auto* payload = new std::vector<lolfind::Candidate>(std::move(results));
    ::PostMessageW(g_hMain, WM_APP_DETECT_DONE, 0, (LPARAM)payload);
}

void UploadThread(std::wstring dir, std::wstring password) {
    g_cancel.store(false);
    ::PostMessageW(g_hMain, WM_APP_SET_BUSY, 1, 0);

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

    uploader::Outcome result = uploader::Download(
        dir, password,
        [](const std::wstring& s) { PostLog(s); },
        [](int permille, const std::wstring& stage) { PostProgress(permille, stage); },
        &g_cancel);

    auto* payload = new uploader::Outcome(std::move(result));
    ::PostMessageW(g_hMain, WM_APP_UPLOAD_DONE, 0, (LPARAM)payload);
}

// ---------------------------------------------------------------------------
// 创建控件
// ---------------------------------------------------------------------------
HWND MakeStatic(HWND parent, int id, const wchar_t* text, DWORD extra = 0) {
    return ::CreateWindowExW(0, L"STATIC", text,
                             WS_CHILD | WS_VISIBLE | extra,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

HWND MakeOwnerButton(HWND parent, int id, const wchar_t* text) {
    return ::CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

void CreateControls(HWND hwnd) {
    g_hHeader = MakeStatic(hwnd, IDC_STATIC_HEADER, APP_TITLE_W);
    g_hSubHeader = MakeStatic(hwnd, IDC_STATIC_SUBHEADER,
        L"定位 League of Legends 的 saves 目录，加密打包上传；或凭密码下载并覆盖解压");

    g_hPathLabel = MakeStatic(hwnd, IDC_STATIC_PATH_LABEL, L"saves 目录");

    g_hPathEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_PATH, g_hInst, nullptr);

    g_hBtnDetect = MakeOwnerButton(hwnd, IDC_BTN_DETECT, L"自动检测");
    g_hBtnBrowse = MakeOwnerButton(hwnd, IDC_BTN_BROWSE, L"手动选择");

    g_hVerify = MakeStatic(hwnd, IDC_STATIC_VERIFY, L"尚未选择目录");

    g_hPwdLabel = MakeStatic(hwnd, IDC_STATIC_PWD_LABEL, L"压缩包密码（4-24 位字母/数字）");

    g_hPwdEdit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
        0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_PWD, g_hInst, nullptr);

    g_hPwdHint = MakeStatic(hwnd, IDC_STATIC_PWD_HINT,
        L"请输入 4~24 位字母或数字，作为压缩包密码（上传/下载都需用同一密码）");

    g_hBtnUpload = MakeOwnerButton(hwnd, IDC_BTN_UPLOAD, L"📤 打包并上传");
    ::EnableWindow(g_hBtnUpload, FALSE);

    g_hBtnDownload = MakeOwnerButton(hwnd, IDC_BTN_DOWNLOAD, L"📥 下载并解压");
    ::EnableWindow(g_hBtnDownload, FALSE);

    g_hProgress = ::CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        0, 0, 10, 10, hwnd, (HMENU)IDC_PROGRESS, g_hInst, nullptr);
    ::SendMessageW(g_hProgress, PBM_SETRANGE32, 0, 1000);
    ::SendMessageW(g_hProgress, PBM_SETSTEP, 1, 0);

    g_hStage = MakeStatic(hwnd, IDC_STATIC_STAGE, L"就绪");

    g_hLog = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_LOG, g_hInst, nullptr);

    g_hResultLabel = MakeStatic(hwnd, IDC_STATIC_RESULT_LBL, L"操作结果");

    g_hResult = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT_RESULT, g_hInst, nullptr);

    g_hBtnCopy = MakeOwnerButton(hwnd, IDC_BTN_COPY, L"复制结果");
    ::EnableWindow(g_hBtnCopy, FALSE);

    g_hFooter = MakeStatic(hwnd, IDC_STATIC_FOOTER,
        L"请牢记您设置的密码：下载恢复时需用同一密码，密码不存储于本地、无法找回");
}

void ApplyFonts() {
    HWND all[] = { g_hSubHeader, g_hPathLabel, g_hPathEdit, g_hBtnDetect, g_hBtnBrowse,
                   g_hVerify, g_hPwdLabel, g_hPwdEdit, g_hPwdHint, g_hStage,
                   g_hResultLabel, g_hBtnCopy, g_hFooter };
    for (HWND h : all)
        if (h) ::SendMessageW(h, WM_SETFONT, (WPARAM)g_fontBase, TRUE);

    if (g_hHeader) ::SendMessageW(g_hHeader, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);
    if (g_hLog)    ::SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
    if (g_hResult) ::SendMessageW(g_hResult, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
    if (g_hBtnDetect)  ::SendMessageW(g_hBtnDetect, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
    if (g_hBtnBrowse)  ::SendMessageW(g_hBtnBrowse, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
    if (g_hBtnUpload)  ::SendMessageW(g_hBtnUpload, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
    if (g_hBtnDownload)::SendMessageW(g_hBtnDownload, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
    if (g_hBtnCopy)    ::SendMessageW(g_hBtnCopy, WM_SETFONT, (WPARAM)g_fontButton, TRUE);
}

void LayoutControls(HWND hwnd) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    const int W = rc.right - rc.left;

    const int M = S(20);            // 外边距
    const int cardTop = g_titleH + S(8);
    const int contentW = W - M * 2;
    int y = cardTop + S(16);

    auto place = [](HWND h, int x, int yy, int w, int hh) {
        if (h) ::SetWindowPos(h, nullptr, x, yy, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    place(g_hHeader, M, y, contentW, S(30));
    y += S(34);
    place(g_hSubHeader, M, y, contentW, S(20));
    y += S(30);

    // 目录行
    place(g_hPathLabel, M, y, contentW, S(18));
    y += S(22);

    const int btnW = S(92);
    const int gap  = S(10);
    const int editW = contentW - btnW * 2 - gap * 2;
    place(g_hPathEdit, M, y, editW, S(28));
    place(g_hBtnDetect, M + editW + gap, y, btnW, S(28));
    place(g_hBtnBrowse, M + editW + gap + btnW + gap, y, btnW, S(28));
    y += S(34);

    place(g_hVerify, M, y, contentW, S(20));
    y += S(28);

    // 密码区
    place(g_hPwdLabel, M, y, contentW, S(18));
    y += S(22);
    place(g_hPwdEdit, M, y, contentW, S(28));
    y += S(34);
    place(g_hPwdHint, M, y, contentW, S(18));
    y += S(26);

    // 两个主按钮并排
    const int btnH = S(46);
    const int twoGap = S(14);
    const int halfW = (contentW - twoGap) / 2;
    place(g_hBtnUpload, M, y, halfW, btnH);
    place(g_hBtnDownload, M + halfW + twoGap, y, halfW, btnH);
    y += btnH + S(16);

    place(g_hProgress, M, y, contentW, S(16));
    y += S(22);
    place(g_hStage, M, y, contentW, S(18));
    y += S(26);

    // 日志区自适应高度
    const int bottomBlock = S(20) + S(84) + S(10) + S(30) + S(10) + S(18) + S(16);
    int logH = rc.bottom - y - bottomBlock;
    if (logH < S(80)) logH = S(80);
    place(g_hLog, M, y, contentW, logH);
    y += logH + S(14);

    place(g_hResultLabel, M, y, contentW, S(18));
    y += S(22);
    place(g_hResult, M, y, contentW, S(84));
    y += S(84) + S(10);

    place(g_hBtnCopy, M, y, S(100), S(30));
    y += S(38);

    place(g_hFooter, M, y, contentW, S(18));
}

void CreateFonts() {
    auto mk = [&](int pt, int weight, const wchar_t* face) -> HFONT {
        LOGFONTW lf{};
        lf.lfHeight = -::MulDiv(pt, g_dpi, 72);
        lf.lfWeight = weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        ::wcscpy_s(lf.lfFaceName, face);
        HFONT f = ::CreateFontIndirectW(&lf);
        if (!f) f = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
        return f;
    };

    g_fontBase    = mk(9,  FW_NORMAL,   L"Microsoft YaHei UI");
    g_fontTitle   = mk(16, FW_SEMIBOLD, L"Microsoft YaHei UI");
    g_fontTitleBar= mk(12, FW_SEMIBOLD, L"Microsoft YaHei UI");
    g_fontButton  = mk(11, FW_SEMIBOLD, L"Microsoft YaHei UI");
    g_fontMono    = mk(9,  FW_NORMAL,   L"Consolas");
}

void DestroyFonts() {
    HFONT fs[] = { g_fontBase, g_fontTitle, g_fontTitleBar, g_fontButton, g_fontMono };
    for (HFONT f : fs)
        if (f && f != (HFONT)::GetStockObject(DEFAULT_GUI_FONT)) ::DeleteObject(f);
    g_fontBase = g_fontTitle = g_fontTitleBar = g_fontButton = g_fontMono = nullptr;
}

// 圆角窗口区域
void UpdateWindowRgn(HWND hwnd) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    HRGN rgn = ::CreateRoundRectRgn(rc.left, rc.top, rc.right, rc.bottom, S(16), S(16));
    if (rgn) {
        ::SetWindowRgn(hwnd, rgn, TRUE);
        ::DeleteObject(rgn);
    }
}

// ---------------------------------------------------------------------------
// 背景绘制（渐变 + 卡片 + 标题栏）
// ---------------------------------------------------------------------------
void PaintBackground(HDC dc, HWND hwnd) {
    RECT rc{};
    ::GetClientRect(hwnd, &rc);

    // 渐变背景
    VGrad3(dc, rc, kColBgTop, kColBgMid, kColBgBottom);

    // 柔和光斑（二次元氛围）
    HBRUSH b1 = ::CreateSolidBrush(RGB(255, 236, 246));
    HBRUSH b2 = ::CreateSolidBrush(RGB(226, 214, 255));
    HRGN c1 = ::CreateEllipticRgn(rc.right - S(160), S(-60), rc.right + S(40), S(160));
    HRGN c2 = ::CreateEllipticRgn(S(-80), rc.bottom - S(180), S(160), rc.bottom + S(40));
    if (c1) { ::FillRgn(dc, c1, b1); ::DeleteObject(c1); }
    if (c2) { ::FillRgn(dc, c2, b2); ::DeleteObject(c2); }
    ::DeleteObject(b1);
    ::DeleteObject(b2);

    // 内容卡片（白底圆角）
    const int M = S(20);
    RECT card = { M, g_titleH + S(8), rc.right - M, rc.bottom - S(10) };
    FillRoundRect(dc, card, S(18), RGB(255, 252, 255), RGB(235, 225, 245));

    // 标题栏文字
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, kColText);
    HGDIOBJ of = ::SelectObject(dc, g_fontTitleBar);
    RECT tr = { S(18), S(6), rc.right, g_titleH };
    ::DrawTextW(dc, APP_TITLE_W, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, of);

    // 标题栏按钮
    DrawCaptionButton(dc, CloseRect(rc.right), g_closeHot, true);
    DrawCaptionButton(dc, MinRect(rc.right), g_minHot, false);
}

// ---------------------------------------------------------------------------
// 结果处理
// ---------------------------------------------------------------------------
void HandleDetectDone(std::vector<lolfind::Candidate>* results) {
    if (!results) return;

    if (results->empty()) {
        AppendLog(L"没有找到包含 " MARKER_FILE_W L" 的 saves 目录，请点「手动选择」指定");
        SetVerifyState(2, L"× 自动检测未找到目录，请手动选择");
    } else {
        if (results->size() > 1) {
            AppendLog(L"共找到 " + std::to_wstring(results->size()) + L" 个候选目录：");
            for (size_t i = 0; i < results->size(); ++i) {
                AppendLog(L"    " + std::to_wstring(i + 1) + L". " +
                          (*results)[i].savesPath + L"  （来源：" + (*results)[i].source + L"）");
            }
            AppendLog(L"已自动选用第 1 个，若不正确请点「手动选择」更换");
        } else {
            AppendLog(L"已找到：" + (*results)[0].savesPath +
                      L"  （来源：" + (*results)[0].source + L"）");
        }
        ::SetWindowTextW(g_hPathEdit, (*results)[0].savesPath.c_str());
    }

    delete results;
    RefreshVerify();
    PostProgress(0, L"就绪");
    ::PostMessageW(g_hMain, WM_APP_SET_BUSY, 0, 0);
}

void HandleUploadDone(uploader::Outcome* r) {
    if (!r) return;

    if (r->canceled) {
        AppendLog(L"操作已取消");
        ::SetWindowTextW(g_hStage, L"已取消");
        ::SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
    } else if (!r->ok) {
        if (r->passwordWrong) {
            AppendLog(L"密码错误，无法解密压缩包");
            ::SetWindowTextW(g_hStage, L"密码错误");
            ::MessageBoxW(g_hMain, L"密码不正确，无法解密该压缩包。\r\n请确认下载时使用的密码与上传时一致。",
                          L"密码错误", MB_OK | MB_ICONWARNING);
        } else {
            AppendLog(L"操作失败：" + r->error);
            ::SetWindowTextW(g_hStage, r->isDownload ? L"下载失败" : L"上传失败");
            ::MessageBoxW(g_hMain, r->error.c_str(), r->isDownload ? L"下载失败" : L"上传失败",
                          MB_OK | MB_ICONWARNING);
        }
    } else if (r->isDownload) {
        std::wstring text;
        text += L"已解压到： " + GetPathFromEdit() + L"\r\n";
        text += L"恢复文件： " + std::to_wstring((unsigned long long)r->extractedFiles) + L" 个\r\n";
        text += L"下载大小： " + util::FormatSize(r->downloadedBytes) + L"\r\n";
        text += L"解压大小： " + util::FormatSize(r->rawBytes);

        g_lastResultText = text;
        ::SetWindowTextW(g_hResult, text.c_str());
        ::EnableWindow(g_hBtnCopy, TRUE);
        ::SetWindowTextW(g_hStage, L"下载解压完成");
        ::SendMessageW(g_hProgress, PBM_SETPOS, 1000, 0);
        ::SetWindowTextW(g_hResultLabel, L"下载结果");
        AppendLog(L"===== 下载并解压完成，文件已覆盖至目标目录 =====");
    } else {
        std::wstring text;
        text += L"压缩密码： " + r->password + L"\r\n";
        text += L"对象 Key： " + r->objectKey + L"\r\n";
        if (!r->downloadUrl.empty())
            text += L"下载链接： " + r->downloadUrl + L"\r\n";
        if (!r->expireText.empty())
            text += L"有效期：   " + r->expireText + L"\r\n";
        text += L"压缩包大小：" + util::FormatSize(r->zipBytes) +
                L"（原始 " + util::FormatSize(r->rawBytes) + L"，共 " +
                std::to_wstring((unsigned long long)r->fileCount) + L" 个文件）";

        g_lastResultText = text;
        ::SetWindowTextW(g_hResult, text.c_str());
        ::EnableWindow(g_hBtnCopy, TRUE);
        ::SetWindowTextW(g_hStage, L"上传完成");
        ::SetWindowTextW(g_hResultLabel, L"上传结果（请务必保存密码）");
        ::SendMessageW(g_hProgress, PBM_SETPOS, 1000, 0);

        AppendLog(L"===== 上传成功，请立即保存下方密码 =====");
        g_uploadDone.store(true);

        util::CopyTextToClipboard(g_hMain, text);
        AppendLog(L"结果已自动复制到剪贴板");
    }

    delete r;
    ::PostMessageW(g_hMain, WM_APP_SET_BUSY, 0, 0);
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

        CreateFonts();
        CreateControls(hwnd);
        ApplyFonts();
        LayoutControls(hwnd);
        UpdateWindowRgn(hwnd);

        AppendLog(APP_TITLE_W L" v" APP_VERSION_W L" 已启动");
        AppendLog(L"后端地址：" + config::BackendBaseUrl);
        AppendLog(L"判定依据：目录中必须存在 " MARKER_FILE_W);

        // 启动即自动检测
        std::thread(DetectThread).detach();
        return 0;
    }

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        g_titleH = S(42);
        DestroyFonts();
        CreateFonts();
        ApplyFonts();

        RECT* nr = (RECT*)lp;
        ::SetWindowPos(hwnd, nullptr, nr->left, nr->top,
                       nr->right - nr->left, nr->bottom - nr->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutControls(hwnd);
        UpdateWindowRgn(hwnd);
        ::InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_SIZE:
        LayoutControls(hwnd);
        UpdateWindowRgn(hwnd);
        ::InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = S(680);
        mmi->ptMinTrackSize.y = S(640);
        return 0;
    }

    case WM_ERASEBKGND: {
        PaintBackground((HDC)wp, hwnd);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        ::SetBkMode(dc, OPAQUE);
        ::SetBkColor(dc, RGB(255, 252, 255));
        if (ctl == g_hHeader) {
            ::SetTextColor(dc, kColText);
        } else if (ctl == g_hSubHeader || ctl == g_hFooter || ctl == g_hStage) {
            ::SetTextColor(dc, kColSubText);
        } else if (ctl == g_hVerify) {
            const LONG_PTR st = ::GetWindowLongPtrW(g_hVerify, GWLP_USERDATA);
            ::SetTextColor(dc, st == 1 ? kColOk : (st == 2 ? kColWarn : kColSubText));
        } else if (ctl == g_hPwdHint) {
            const LONG_PTR st = ::GetWindowLongPtrW(g_hPwdHint, GWLP_USERDATA);
            ::SetTextColor(dc, st == 1 ? kColOk : (st == 2 ? kColWarn : kColSubText));
        } else {
            ::SetTextColor(dc, kColText);
        }
        return (LRESULT)g_brushCard;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        ::SetTextColor(dc, kColText);
        ::SetBkColor(dc, RGB(255, 255, 255));
        return (LRESULT)g_brushCard;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlType != ODT_BUTTON) break;
        const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        const bool pressed  = (dis->itemState & ODS_SELECTED) != 0;

        wchar_t text[128]{};
        ::GetWindowTextW(dis->hwndItem, text, 128);
        RECT r = dis->rcItem;

        if (dis->CtlID == IDC_BTN_UPLOAD) {
            DrawGradientButton(dis->hDC, r, pressed, disabled,
                               kUpTop, kUpBot, kUpTopH, kUpBotH, text, g_fontButton);
        } else if (dis->CtlID == IDC_BTN_DOWNLOAD) {
            DrawGradientButton(dis->hDC, r, pressed, disabled,
                               kDlTop, kDlBot, kDlTopH, kDlBotH, text, g_fontButton);
        } else if (dis->CtlID == IDC_BTN_DETECT || dis->CtlID == IDC_BTN_BROWSE ||
                   dis->CtlID == IDC_BTN_COPY) {
            DrawGhostButton(dis->hDC, r, pressed, disabled, text, g_fontButton);
        }
        return TRUE;
    }

    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ::ScreenToClient(hwnd, &pt);
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        RECT cr = CloseRect(rc.right);
        RECT mr = MinRect(rc.right);
        if (pt.y < g_titleH) {
            if (::PtInRect(&cr, pt)) return HTCLIENT;
            if (::PtInRect(&mr, pt))   return HTCLIENT;
            return HTCAPTION;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        RECT cr = CloseRect(rc.right);
        RECT mr = MinRect(rc.right);
        if (::PtInRect(&cr, pt)) {
            ::SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (::PtInRect(&mr, pt)) {
            ::ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        RECT cr = CloseRect(rc.right);
        RECT mr = MinRect(rc.right);
        const bool ch = ::PtInRect(&cr, pt) != FALSE;
        const bool mh = ::PtInRect(&mr, pt) != FALSE;
        if (ch != g_closeHot) { g_closeHot = ch; ::InvalidateRect(hwnd, &cr, FALSE); }
        if (mh != g_minHot)   { g_minHot   = mh; ::InvalidateRect(hwnd, &mr, FALSE); }
        if (!g_titleTracking) {
            TRACKMOUSEEVENT tme{ sizeof(tme) };
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            ::TrackMouseEvent(&tme);
            g_titleTracking = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_titleTracking = false;
        if (g_closeHot) { g_closeHot = false; RECT rc{}; ::GetClientRect(hwnd, &rc); RECT cr = CloseRect(rc.right); ::InvalidateRect(hwnd, &cr, FALSE); }
        if (g_minHot)   { g_minHot   = false; RECT rc{}; ::GetClientRect(hwnd, &rc); RECT mr = MinRect(rc.right); ::InvalidateRect(hwnd, &mr, FALSE); }
        return 0;

    case WM_COMMAND: {
        const int id   = LOWORD(wp);
        const int code = HIWORD(wp);

        if (id == IDC_EDIT_PATH && code == EN_CHANGE) {
            if (!g_busy.load()) RefreshVerify();
            return 0;
        }
        if (id == IDC_EDIT_PWD && code == EN_CHANGE) {
            FilterPasswordEdit();
            return 0;
        }

        if (code != BN_CLICKED) break;

        switch (id) {
        case IDC_BTN_DETECT:
            if (!g_busy.load()) {
                ::SetWindowTextW(g_hResult, L"");
                ::EnableWindow(g_hBtnCopy, FALSE);
                std::thread(DetectThread).detach();
            }
            return 0;

        case IDC_BTN_BROWSE: {
            if (g_busy.load()) return 0;
            const std::wstring p = PickFolder(hwnd);
            if (!p.empty()) {
                ::SetWindowTextW(g_hPathEdit, p.c_str());
                AppendLog(L"已手动选择：" + p);
                if (!lolfind::HasMarker(p))
                    AppendLog(L"注意：该目录里没有 " MARKER_FILE_W L"，无法上传");
            }
            return 0;
        }

        case IDC_BTN_UPLOAD: {
            if (g_busy.load()) return 0;
            const std::wstring dir = GetPathFromEdit();
            const std::wstring pwd = GetPasswordFromEdit();
            if (dir.empty() || !lolfind::HasMarker(dir)) {
                ::MessageBoxW(hwnd, L"当前目录无效，必须是包含 " MARKER_FILE_W L" 的 saves 目录",
                              L"无法上传", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (!util::IsValidPassword(pwd)) {
                ::MessageBoxW(hwnd, L"请先在密码框输入 4-24 位字母或数字",
                              L"缺少密码", MB_OK | MB_ICONWARNING);
                return 0;
            }
            ::SetWindowTextW(g_hResult, L"");
            ::EnableWindow(g_hBtnCopy, FALSE);
            g_uploadDone.store(false);
            std::thread(UploadThread, dir, pwd).detach();
            return 0;
        }

        case IDC_BTN_DOWNLOAD: {
            if (g_busy.load()) return 0;
            const std::wstring dir = GetPathFromEdit();
            const std::wstring pwd = GetPasswordFromEdit();
            if (dir.empty() || !util::DirectoryExists(dir)) {
                ::MessageBoxW(hwnd, L"请先选择要解压到的 saves 目录",
                              L"缺少目录", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (!util::IsValidPassword(pwd)) {
                ::MessageBoxW(hwnd, L"请先在密码框输入 4-24 位字母或数字",
                              L"缺少密码", MB_OK | MB_ICONWARNING);
                return 0;
            }
            ::SetWindowTextW(g_hResult, L"");
            ::EnableWindow(g_hBtnCopy, FALSE);
            std::thread(DownloadThread, dir, pwd).detach();
            return 0;
        }

        case IDC_BTN_COPY:
            if (!g_lastResultText.empty()) {
                util::CopyTextToClipboard(hwnd, g_lastResultText);
                AppendLog(L"结果已复制到剪贴板");
            }
            return 0;

        default: break;
        }
        break;
    }

    case WM_APP_LOG: {
        auto* s = (std::wstring*)lp;
        if (s) { AppendLog(*s); delete s; }
        return 0;
    }

    case WM_APP_PROGRESS: {
        int permille = (int)wp;
        if (permille < 0) permille = 0;
        if (permille > 1000) permille = 1000;
        ::SendMessageW(g_hProgress, PBM_SETPOS, (WPARAM)permille, 0);

        auto* s = (std::wstring*)lp;
        if (s) {
            if (!s->empty()) ::SetWindowTextW(g_hStage, s->c_str());
            delete s;
        }
        return 0;
    }

    case WM_APP_DETECT_DONE:
        HandleDetectDone((std::vector<lolfind::Candidate>*)lp);
        return 0;

    case WM_APP_UPLOAD_DONE:
        HandleUploadDone((uploader::Outcome*)lp);
        return 0;

    case WM_APP_SET_BUSY:
        SetBusy(wp != 0);
        return 0;

    case WM_CLOSE:
        if (g_busy.load()) {
            const int r = ::MessageBoxW(hwnd,
                L"任务正在进行中，确定要中止并退出吗？",
                L"确认退出", MB_YESNO | MB_ICONQUESTION);
            if (r != IDYES) return 0;
            g_cancel.store(true);
            ::Sleep(200);
        }
        if (g_uploadDone.load() && !g_lastResultText.empty()) {
            const int r = ::MessageBoxW(hwnd,
                L"密码关闭后将无法找回，确认已经保存好了吗？\r\n\r\n"
                L"（结果已自动复制到剪贴板）",
                L"确认退出", MB_YESNO | MB_ICONWARNING);
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
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetCtxFn = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setCtx = (SetCtxFn)::GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx && setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }

    HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using SetAwareFn = HRESULT (WINAPI*)(int);
        auto setAware = (SetAwareFn)::GetProcAddress(shcore, "SetProcessDpiAwareness");
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

    EnableDpiAwareness();
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_WIN95_CLASSES };
    ::InitCommonControlsEx(&icc);

    config::Load();
    config::WriteTemplateIfMissing();

    // 只允许运行一个实例
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, L"Global\\HanbotSavesUploader_SingleInstance");
    if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND exist = ::FindWindowW(L"HanbotUploaderWndClass", nullptr);
        if (exist) {
            ::ShowWindow(exist, SW_RESTORE);
            ::SetForegroundWindow(exist);
        }
        return 0;
    }

    g_brushBg   = ::CreateSolidBrush(kColBgTop);
    g_brushCard = ::CreateSolidBrush(RGB(255, 252, 255));

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_brushCard;
    wc.lpszClassName = L"HanbotUploaderWndClass";
    wc.hIcon         = ::LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = ::LoadIconW(nullptr, IDI_APPLICATION);

    if (!::RegisterClassExW(&wc)) {
        ::MessageBoxW(nullptr, L"窗口注册失败", APP_TITLE_W, MB_OK | MB_ICONERROR);
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

    RECT wr{ 0, 0, S(760), S(720) };
    ::AdjustWindowRectEx(&wr, WS_POPUP | WS_SYSMENU, FALSE, 0);

    const int winW = wr.right - wr.left;
    const int winH = wr.bottom - wr.top;
    const int x = (::GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    const int y = (::GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, APP_TITLE_W,
                                  WS_POPUP | WS_SYSMENU,
                                  x > 0 ? x : CW_USEDEFAULT,
                                  y > 0 ? y : CW_USEDEFAULT,
                                  winW, winH,
                                  nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        ::MessageBoxW(nullptr, L"窗口创建失败", APP_TITLE_W, MB_OK | MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(hwnd, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    DestroyFonts();
    if (g_brushBg)   ::DeleteObject(g_brushBg);
    if (g_brushCard) ::DeleteObject(g_brushCard);
    if (mutex)       ::CloseHandle(mutex);
    ::CoUninitialize();

    return (int)msg.wParam;
}
