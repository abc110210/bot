#include "util.h"

#include <bcrypt.h>
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cwctype>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace util {

// ===========================================================================
// 编码
// ===========================================================================
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out((size_t)need, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], need);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out((size_t)need, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], need, nullptr, nullptr);
    return out;
}

// ===========================================================================
// 字符串
// ===========================================================================
std::wstring Trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    auto isSpace = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
               c == L'\v' || c == L'\f' || c == 0xFEFF /* BOM */;
    };
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    }
    return out;
}

std::string ToLowerA(const std::string& s) {
    std::string out = s;
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return out;
}

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    if (suffix.size() > s.size()) return false;
    const std::wstring tail = ToLower(s.substr(s.size() - suffix.size()));
    return tail == ToLower(suffix);
}

// ===========================================================================
// 路径 / 文件
// ===========================================================================
bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirectoryExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;

    std::wstring left = a;
    while (!left.empty() && (left.back() == L'\\' || left.back() == L'/')) left.pop_back();

    size_t i = 0;
    while (i < b.size() && (b[i] == L'\\' || b[i] == L'/')) ++i;

    return left + L"\\" + b.substr(i);
}

std::wstring GetExeDir() {
    std::vector<wchar_t> buf(1024);
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::wstring();
        if (n < buf.size() - 1) break;
        buf.resize(buf.size() * 2);
        if (buf.size() > 65536) return std::wstring();
    }
    std::wstring p(buf.data());
    const size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : p.substr(0, pos);
}

std::wstring GetTempDir() {
    wchar_t buf[MAX_PATH + 2]{};
    const DWORD n = ::GetTempPathW(MAX_PATH + 1, buf);
    if (n == 0 || n > MAX_PATH + 1) return L"C:\\Windows\\Temp";
    std::wstring p(buf, n);
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p.empty() ? std::wstring(L"C:\\Windows\\Temp") : p;
}

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) return path;
    wchar_t buf[4096]{};
    const DWORD n = ::GetFullPathNameW(path.c_str(), 4096, buf, nullptr);
    std::wstring p = (n > 0 && n < 4096) ? std::wstring(buf, n) : path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        ::CloseHandle(h);
        return false;
    }
    // 单文件读取上限 64 MB，防止误读超大文件把内存吃光
    if (size.QuadPart > 64ll * 1024 * 1024) {
        ::CloseHandle(h);
        return false;
    }

    out.resize((size_t)size.QuadPart);
    size_t done = 0;
    bool ok = true;
    while (done < out.size()) {
        const DWORD want = (DWORD)((out.size() - done) > 0x100000 ? 0x100000 : (out.size() - done));
        DWORD got = 0;
        if (!::ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) {
            ok = (done == out.size());
            break;
        }
        done += got;
    }
    ::CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

bool WriteWholeFile(const std::wstring& path, const void* data, size_t bytes) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    const uint8_t* p = (const uint8_t*)data;
    size_t done = 0;
    bool ok = true;
    while (done < bytes) {
        const DWORD want = (DWORD)((bytes - done) > 0x100000 ? 0x100000 : (bytes - done));
        DWORD wrote = 0;
        if (!::WriteFile(h, p + done, want, &wrote, nullptr) || wrote == 0) { ok = false; break; }
        done += wrote;
    }
    ::CloseHandle(h);
    return ok;
}

// ===========================================================================
// 随机
// ===========================================================================
void RandomBytes(void* buf, size_t n) {
    if (!buf || n == 0) return;

    const NTSTATUS st = ::BCryptGenRandom(nullptr, (PUCHAR)buf, (ULONG)n,
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st == STATUS_SUCCESS) return;

    // 退化方案：多源混合，够用即可（仅在系统 RNG 不可用时触发）
    uint8_t* p = (uint8_t*)buf;
    uint64_t s = 0;
    LARGE_INTEGER qpc{};
    ::QueryPerformanceCounter(&qpc);
    s ^= (uint64_t)qpc.QuadPart;
    s ^= (uint64_t)::GetTickCount64() << 17;
    s ^= (uint64_t)::GetCurrentProcessId() << 33;
    s ^= (uint64_t)::GetCurrentThreadId() << 7;
    s ^= (uint64_t)(uintptr_t)buf;
    if (s == 0) s = 0x9E3779B97F4A7C15ull;

    for (size_t i = 0; i < n; ++i) {
        // xorshift64*
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        const uint64_t v = s * 0x2545F4914F6CDD1Dull;
        p[i] = (uint8_t)(v >> 33);
    }
}

std::string GenerateRandomPassword(size_t length) {
    // 去掉 0 O o 1 l I 等易混淆字符，避免用户手抄出错
    static const char kAlphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZ"
        "abcdefghijkmnpqrstuvwxyz"
        "23456789"
        "!@#%^&*-_=+";
    const size_t alphaLen = sizeof(kAlphabet) - 1;

    if (length < 8)  length = 8;
    if (length > 64) length = 64;

    std::vector<uint8_t> raw(length * 2);
    RandomBytes(raw.data(), raw.size());

    std::string out;
    out.reserve(length);

    // 拒绝采样，消除取模偏置
    const size_t limit = 256 - (256 % alphaLen);
    size_t idx = 0;
    while (out.size() < length) {
        if (idx >= raw.size()) {
            raw.resize(raw.size() + length);
            RandomBytes(raw.data() + idx, raw.size() - idx);
        }
        const uint8_t v = raw[idx++];
        if (v >= limit) continue;
        out.push_back(kAlphabet[v % alphaLen]);
    }
    return out;
}

// ===========================================================================
// 校验 / 网络
// ===========================================================================
bool IsValidPassword(const std::wstring& pwd) {
    const size_t n = pwd.size();
    if (n < 4 || n > 24) return false;
    for (wchar_t c : pwd) {
        const bool ok = (c >= L'A' && c <= L'Z') ||
                        (c >= L'a' && c <= L'z') ||
                        (c >= L'0' && c <= L'9');
        if (!ok) return false;
    }
    return true;
}

std::wstring GetMachineIp() {
    // 优先取非回环、已连接的 IPv4；失败退回回环/未知
    std::wstring best;

    ULONG bufLen = 16384;
    std::vector<uint8_t> buf(bufLen);
    ULONG ret = ::GetAdaptersAddresses(
        AF_INET,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &bufLen);

    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        ret = ::GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, (IP_ADAPTER_ADDRESSES*)buf.data(), &bufLen);
    }

    if (ret == NO_ERROR) {
        for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* sa = (sockaddr_in*)ua->Address.lpSockaddr;
                const uint32_t ip = ntohl(sa->sin_addr.S_un.S_addr);
                // 跳过回环 127.* 和链路本地 169.254.*
                const uint8_t b0 = (uint8_t)(ip >> 24);
                if (b0 == 127 || b0 == 169) continue;
                wchar_t s[64]{};
                ::swprintf(s, 64, L"%u.%u.%u.%u",
                           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                           (ip >> 8) & 0xFF, ip & 0xFF);
                return std::wstring(s);
            }
        }
    }

    // 兜底：解析本机名
    wchar_t name[256]{};
    DWORD n = 256;
    if (::GetComputerNameW(name, &n)) {
        addrinfoW hints{};
        hints.ai_family = AF_INET;
        addrinfoW* res = nullptr;
        if (::GetAddrInfoW(name, nullptr, &hints, &res) == 0) {
            if (res) {
                const auto* sa = (sockaddr_in*)res->ai_addr;
                const uint32_t ip = ntohl(sa->sin_addr.S_un.S_addr);
                wchar_t s[64]{};
                ::swprintf(s, 64, L"%u.%u.%u.%u",
                           (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                           (ip >> 8) & 0xFF, ip & 0xFF);
                best = s;
                ::FreeAddrInfoW(res);
            }
        }
    }
    return best.empty() ? L"unknown" : best;
}

// ===========================================================================
// 格式化
// ===========================================================================
std::wstring FormatSize(unsigned long long bytes) {
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }

    wchar_t buf[64]{};
    if (u == 0) ::swprintf(buf, 64, L"%llu B", bytes);
    else        ::swprintf(buf, 64, L"%.2f %s", v, units[u]);
    return buf;
}

std::wstring TimestampCompact() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t buf[32]{};
    ::swprintf(buf, 32, L"%04u%02u%02u_%02u%02u%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring MachineId() {
    // 机器名 + 用户名做 FNV-1a，只用于区分不同机器的上传，不含隐私信息
    wchar_t name[256]{};
    DWORD n = 256;
    if (!::GetComputerNameW(name, &n)) { wcscpy_s(name, L"unknown"); }

    wchar_t user[256]{};
    DWORD un = 256;
    if (!::GetUserNameW(user, &un)) { wcscpy_s(user, L"user"); }

    std::wstring src = std::wstring(name) + L"|" + user;

    uint32_t h = 2166136261u;
    for (wchar_t c : src) {
        h ^= (uint32_t)(c & 0xFF);
        h *= 16777619u;
        h ^= (uint32_t)((c >> 8) & 0xFF);
        h *= 16777619u;
    }

    wchar_t buf[16]{};
    ::swprintf(buf, 16, L"%08x", h);
    return buf;
}

// ===========================================================================
// 系统
// ===========================================================================
std::wstring LastErrorText(DWORD code) {
    if (code == 0) return L"未知错误";

    LPWSTR msg = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&msg, 0, nullptr);

    std::wstring text;
    if (n && msg) {
        text = Trim(std::wstring(msg, n));
    }
    if (msg) ::LocalFree(msg);

    wchar_t code_s[32]{};
    ::swprintf(code_s, 32, L"(0x%08X)", (unsigned)code);

    if (text.empty()) return std::wstring(L"系统错误 ") + code_s;
    return text + L" " + code_s;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!::OpenClipboard(owner)) return false;

    bool ok = false;
    if (::EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem) {
            void* p = ::GlobalLock(mem);
            if (p) {
                ::memcpy(p, text.c_str(), bytes);
                ::GlobalUnlock(mem);
                if (::SetClipboardData(CF_UNICODETEXT, mem)) {
                    ok = true;          // 所有权已交给系统，不能再 Free
                } else {
                    ::GlobalFree(mem);
                }
            } else {
                ::GlobalFree(mem);
            }
        }
    }
    ::CloseClipboard();
    return ok;
}

} // namespace util
