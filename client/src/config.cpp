#include "config.h"
#include "util.h"

#include <windows.h>

namespace config {

// 后端地址（编译期硬编码；端口以部署为准，这里用 20333）
std::wstring BackendBaseUrl   = BACKEND_BASE_URL_W;

// 客户端密钥（来自 secrets，运行时 OBFW 解码；与 server.py 的 CLIENT_API_KEY 保持一致）
std::string  ClientKey        = util::WideToUtf8(secrets::client_key());

// 网络超时（毫秒，硬编码）
int          ConnectTimeoutMs = 15000;
int          SendTimeoutMs    = 120000;
int          RecvTimeoutMs    = 120000;

static std::wstring IniPath() {
    return util::JoinPath(util::GetExeDir(), L"uploader.ini");
}

void Load() {
    // 重要参数均已编译期硬编码，不再从 uploader.ini 读取，
    // 避免密钥 / 地址等敏感信息落到配置文件里。本函数保留仅为兼容调用点。
}

void WriteTemplateIfMissing() {
    const std::wstring ini = IniPath();
    if (util::FileExists(ini)) return;

    // 用 UTF-16 LE + BOM 写，保证中文在记事本里正常显示。
    // 地址 / 密钥 / 超时 / 体积上限等敏感项不在此模板里，它们全部编译进 exe。
    // 运行后程序会在末尾追加 [State] 段，记录本机记住的 saves 目录与密码（明文，仅本机便利）。
    std::wstring content;
    content += L"\uFEFF";
    content += L"; " APP_TITLE_W L" 配置文件\r\n";

    util::WriteWholeFile(ini, content.data(), content.size() * sizeof(wchar_t));
}

// ---------------------------------------------------------------------------
// 本地记住的状态：直接写入 uploader.ini 的 [State] 段
// ---------------------------------------------------------------------------
static std::wstring StateIniPath() {
    return IniPath();   // uploader.ini 同目录同文件，[State] 段
}

void LoadState(AppState& s) {
    const std::wstring ini = StateIniPath();
    if (!util::FileExists(ini)) return;
    wchar_t buf[2048] = {0};
    DWORD n = ::GetPrivateProfileStringW(L"State", L"SavesDir", L"", buf, 2048, ini.c_str());
    if (n > 0) s.savesDir = std::wstring(buf, n);
    n = ::GetPrivateProfileStringW(L"State", L"Password", L"", buf, 2048, ini.c_str());
    if (n > 0) s.password = std::wstring(buf, n);
}

void SaveState(const std::wstring& savesDir, const std::wstring& password) {
    const std::wstring ini = StateIniPath();
    ::WritePrivateProfileStringW(L"State", L"SavesDir", savesDir.c_str(), ini.c_str());
    ::WritePrivateProfileStringW(L"State", L"Password", password.c_str(), ini.c_str());
}

} // namespace config
