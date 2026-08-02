#include "config.h"
#include "util.h"

#include <windows.h>

namespace config {

std::wstring BackendBaseUrl   = L"http://106.52.205.16:8000";
std::string  ClientKey        = "";   // 默认不校验（与后端 CLIENT_API_KEY 留空一致）
int          ConnectTimeoutMs = 15000;
int          SendTimeoutMs    = 120000;
int          RecvTimeoutMs    = 120000;
unsigned long long MaxPackBytes = 2ull * 1024 * 1024 * 1024;   // 2 GB
int          PasswordLength   = 20;

static std::wstring IniPath() {
    return util::JoinPath(util::GetExeDir(), L"uploader.ini");
}

static std::wstring ReadIniStr(const std::wstring& file,
                               const wchar_t* section,
                               const wchar_t* key,
                               const std::wstring& def) {
    wchar_t buf[2048]{};
    DWORD n = ::GetPrivateProfileStringW(section, key, def.c_str(), buf, 2048, file.c_str());
    return util::Trim(std::wstring(buf, n));
}

static int ReadIniInt(const std::wstring& file,
                      const wchar_t* section,
                      const wchar_t* key,
                      int def) {
    return (int)::GetPrivateProfileIntW(section, key, def, file.c_str());
}

void Load() {
    const std::wstring ini = IniPath();
    if (!util::FileExists(ini)) return;

    std::wstring url = ReadIniStr(ini, L"backend", L"url", BackendBaseUrl);
    if (!url.empty()) {
        while (!url.empty() && url.back() == L'/') url.pop_back();
        BackendBaseUrl = url;
    }

    std::wstring ck = ReadIniStr(ini, L"backend", L"client_key", util::Utf8ToWide(ClientKey));
    if (!ck.empty()) ClientKey = util::WideToUtf8(ck);

    ConnectTimeoutMs = ReadIniInt(ini, L"network", L"connect_timeout_ms", ConnectTimeoutMs);
    SendTimeoutMs    = ReadIniInt(ini, L"network", L"send_timeout_ms", SendTimeoutMs);
    RecvTimeoutMs    = ReadIniInt(ini, L"network", L"recv_timeout_ms", RecvTimeoutMs);

    int maxMb = ReadIniInt(ini, L"pack", L"max_size_mb", (int)(MaxPackBytes / (1024 * 1024)));
    if (maxMb > 0) MaxPackBytes = (unsigned long long)maxMb * 1024 * 1024;

    int pl = ReadIniInt(ini, L"pack", L"password_length", PasswordLength);
    if (pl >= 8 && pl <= 64) PasswordLength = pl;
}

void WriteTemplateIfMissing() {
    const std::wstring ini = IniPath();
    if (util::FileExists(ini)) return;

    // 用 UTF-16 LE + BOM 写，保证中文注释在记事本里正常显示，
    // 同时 GetPrivateProfileString 也能正确解析。
    std::wstring content;
    content += L"\uFEFF";
    content += L"; " APP_TITLE_W L" 配置文件\r\n";
    content += L"; 删除本文件后程序会重新生成一份默认配置\r\n";
    content += L"\r\n[backend]\r\n";
    content += L"; 后端地址（只支持 http，末尾不要加斜杠）\r\n";
    content += L"url=" + BackendBaseUrl + L"\r\n";
    content += L"; 与后端 CLIENT_API_KEY 保持一致\r\n";
    content += L"client_key=" + util::Utf8ToWide(ClientKey) + L"\r\n";
    content += L"\r\n[network]\r\n";
    content += L"connect_timeout_ms=15000\r\n";
    content += L"send_timeout_ms=120000\r\n";
    content += L"recv_timeout_ms=120000\r\n";
    content += L"\r\n[pack]\r\n";
    content += L"; 打包体积上限（MB），超过就拒绝上传\r\n";
    content += L"max_size_mb=2048\r\n";
    content += L"; 随机密码长度（8-64）\r\n";
    content += L"password_length=20\r\n";

    util::WriteWholeFile(ini, content.data(), content.size() * sizeof(wchar_t));
}

} // namespace config
