#pragma once

// ---------------------------------------------------------------------------
// 全局常量与运行时配置
//   重要参数（后端地址 / 客户端密钥 / 网络超时）一律编译期硬编码，
//   不写入 uploader.ini，避免密钥、地址等敏感信息落到配置文件里被泄露。
// ---------------------------------------------------------------------------

#include <string>

// ---- 编译期常量（可直接与宽字符串字面量相邻拼接）----
#define APP_TITLE_W    L"Hanbot缓存存储器"
#define APP_VERSION_W  L"1.0.0"
// 客户端 UA（CDN 防盗链白名单要求为 xlingran/hanbot/1.1）
#define APP_UA_W       L"xlingran/hanbot/1.1"

// 目录判定标志文件：只有包含它的目录才认为是目标 saves 目录
#define MARKER_FILE_W  L"hanbot_core.ini"

// 后端基础地址（编译期硬编码，走 Cloudflare Tunnel 的 HTTPS 域名）。
// 隧道模式下源站仅监听 127.0.0.1，客户端必须经此域名访问，不能直连裸 IP。
#define BACKEND_BASE_URL_W  L"https://bot.xlingran.top"

// 客户端密钥（编译期硬编码，与 server.py 的 CLIENT_API_KEY 完全一致）。
// 不一致则后端 check_client_key 会拒绝请求（403）。
#define CLIENT_API_KEY_W     L"1058823513WAoOZ6-n%DLMPRaI"

namespace config {

// 后端地址（硬编码，不从 ini 读取）
extern std::wstring        BackendBaseUrl;

// 客户端密钥（硬编码，不从 ini 读取）
extern std::string         ClientKey;

// 网络超时（毫秒，硬编码）
extern int                 ConnectTimeoutMs;
extern int                 SendTimeoutMs;
extern int                 RecvTimeoutMs;

// 体积上限改由服务端校验（MAX_UPLOAD_BYTES），客户端不再做本地拦截

// 读取 exe 同目录的 uploader.ini（仅占位，不再包含任何敏感项）
void Load();

// 首次运行时写一份占位配置（不含地址/密钥/超时等敏感信息）
void WriteTemplateIfMissing();

// ---- 本地记住的状态（写入 uploader.ini 的 [State] 段，方便重开软件后直接下载）----
// 注意：这里把 saves 目录与密码以明文写入 uploader.ini（用户要求的本机便利），
// 仅用于本机自动填入，非跨设备传输。与后端地址/客户端密钥无关（那些仍编译进 exe）。
struct AppState {
    std::wstring savesDir;   // 上次成功操作的 saves 目录
    std::wstring password;   // 上次成功上传/下载用的密码
};

// 读取 uploader.ini 的 [State] 段。文件或该段不存在时返回空。
void LoadState(AppState& s);

// 写入 uploader.ini（覆盖 [State] 段的 SavesDir / Password）。
void SaveState(const std::wstring& savesDir, const std::wstring& password);

} // namespace config
