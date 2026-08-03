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

// 后端基础地址（编译期硬编码，末尾不带斜杠，仅支持 http）
// 注意：必须与 server.py 实际监听的地址/端口一致（默认 8000，部署时为 20333）
#define BACKEND_BASE_URL_W  L"http://106.52.205.16:20333"

// 客户端密钥（编译期硬编码，与 server.py 的 CLIENT_API_KEY 保持一致）。
// 必须与后端 server.conf 里的 CLIENT_API_KEY 完全一致，否则后端会拒绝请求。
#define CLIENT_API_KEY_W     L"1058823513"

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

} // namespace config
