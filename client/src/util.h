#pragma once

// ---------------------------------------------------------------------------
// 通用工具：编码转换、文件、随机数、格式化
//   全部基于 Win32 API 与标准库，无第三方依赖
// ---------------------------------------------------------------------------

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace util {

// ---- 编码 ----
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& s);

// ---- 字符串 ----
std::wstring Trim(const std::wstring& s);
std::wstring ToLower(const std::wstring& s);
std::string  ToLowerA(const std::string& s);
bool         EndsWithNoCase(const std::wstring& s, const std::wstring& suffix);

// ---- 路径 / 文件 ----
bool         FileExists(const std::wstring& path);
bool         DirectoryExists(const std::wstring& path);
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring GetExeDir();
std::wstring GetTempDir();
std::wstring NormalizePath(const std::wstring& path);

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out);
bool WriteWholeFile(const std::wstring& path, const void* data, size_t bytes);

// ---- 随机 ----
// 优先使用 BCryptGenRandom，失败则退回到时间 + 进程信息混合的伪随机
void        RandomBytes(void* buf, size_t n);
// 生成不含易混淆字符（0/O/1/l/I）的随机密码
std::string GenerateRandomPassword(size_t length);

// ---- 校验 / 网络 ----
// 密码规则：4-24 位，仅允许字母（A-Z a-z）和数字（0-9）
bool        IsValidPassword(const std::wstring& pwd);
// 获取本机首个非回环 IPv4 地址（用于向后端登记，便于排查）
std::wstring GetMachineIp();

// ---- 格式化 ----
std::wstring FormatSize(unsigned long long bytes);   // 1.23 MB
std::wstring TimestampCompact();                     // 20260802_134501
std::wstring MachineId();                            // 8 位十六进制，机器指纹

// ---- 系统 ----
std::wstring LastErrorText(DWORD code);
bool         CopyTextToClipboard(HWND owner, const std::wstring& text);

} // namespace util
