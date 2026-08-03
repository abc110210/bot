#include "config.h"
#include "util.h"

#include <windows.h>

namespace config {

// 后端地址（编译期硬编码；端口以部署为准，这里用 20333）
std::wstring BackendBaseUrl   = BACKEND_BASE_URL_W;

// 客户端密钥（编译期硬编码，与 server.py 的 CLIENT_API_KEY 保持一致）
std::string  ClientKey        = util::WideToUtf8(CLIENT_API_KEY_W);

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
    // 故意不写入任何地址 / 密钥 / 超时 / 体积上限等敏感项，
    // 这些全部编译进 exe，改它们需要重新编译。
    std::wstring content;
    content += L"\uFEFF";
    content += L"; " APP_TITLE_W L" 配置文件\r\n";
    content += L"; 服务器地址、客户端密钥、网络超时、体积上限等都已内置在程序中，\r\n";
    content += L"; 不在此文件填写。此文件仅作为占位保留。\r\n";

    util::WriteWholeFile(ini, content.data(), content.size() * sizeof(wchar_t));
}

} // namespace config
