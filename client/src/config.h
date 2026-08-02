#pragma once

// ---------------------------------------------------------------------------
// 全局常量与运行时配置
//   编译期常量用宏（便于字符串字面量拼接），运行期可调项放 namespace config
//   运行期项支持被 exe 同目录下的 uploader.ini 覆盖
// ---------------------------------------------------------------------------

#include <string>

// ---- 编译期常量（可直接与宽字符串字面量相邻拼接）----
#define APP_TITLE_W    L"Hanbot 存档打包上传器"
#define APP_VERSION_W  L"1.0.0"
// 客户端 UA（CDN 防盗链白名单要求为 xlingran/hanbot/1.1）
#define APP_UA_W       L"xlingran/hanbot/1.1"

// 目录判定标志文件：只有包含它的目录才认为是目标 saves 目录
#define MARKER_FILE_W  L"hanbot_core.ini"

namespace config {

// 后端地址（只支持 http，末尾不带斜杠）
extern std::wstring        BackendBaseUrl;

// 与后端 CLIENT_API_KEY 对应，用于最基本的调用方校验
extern std::string         ClientKey;

// 网络超时（毫秒）
extern int                 ConnectTimeoutMs;
extern int                 SendTimeoutMs;
extern int                 RecvTimeoutMs;

// 打包体积上限，超过直接拒绝，避免把 C 盘临时目录撑爆
extern unsigned long long  MaxPackBytes;

// 随机压缩密码长度
extern int                 PasswordLength;

// 读取 exe 同目录的 uploader.ini 覆盖上面的默认值；文件不存在则保持默认
void Load();

// 首次运行时写一份带中文注释的配置模板，方便用户改后端地址
void WriteTemplateIfMissing();

} // namespace config
