#include "lol_finder.h"
#include "config.h"
#include "util.h"

#include <windows.h>
#include <tlhelp32.h>
#include <deque>
#include <set>
#include <algorithm>

namespace lolfind {

// ===========================================================================
// 内部辅助
// ===========================================================================
namespace {

struct Ctx {
    LogFn                    log;
    const std::atomic<bool>* cancel = nullptr;
    ULONGLONG                deadline = 0;   // GetTickCount64 截止时刻，0 表示不限时

    bool Expired() const {
        if (cancel && cancel->load()) return true;
        if (deadline == 0) return false;
        return ::GetTickCount64() > deadline;
    }
    void Log(const std::wstring& s) const { if (log) log(s); }
};

// 结果收集器（按小写路径去重）
class Collector {
public:
    void Add(const std::wstring& savesPath, const std::wstring& source) {
        std::wstring key = util::ToLower(savesPath);
        while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
        if (seen_.count(key)) return;
        seen_.insert(key);

        std::wstring clean = savesPath;
        while (!clean.empty() && (clean.back() == L'\\' || clean.back() == L'/')) clean.pop_back();
        items_.push_back({ clean, source });
    }
    bool Empty() const { return items_.empty(); }
    std::vector<Candidate> Take() { return std::move(items_); }

private:
    std::set<std::wstring> seen_;
    std::vector<Candidate> items_;
};

// 从一个「疑似 LoL 根目录」出发，尝试所有可能的 saves 子路径
void ProbeRoot(const std::wstring& root, const std::wstring& source,
               Collector& out, const Ctx& ctx) {
    if (root.empty()) return;

    static const wchar_t* kSubPaths[] = {
        L"saves",
        L"Game\\saves",
        L"LeagueClient\\saves",
        L"Config\\saves",
        L"..\\saves",
        L"League of Legends\\saves",
        L"League of Legends\\Game\\saves",
    };

    for (const wchar_t* sub : kSubPaths) {
        if (ctx.Expired()) return;
        const std::wstring p = util::JoinPath(root, sub);
        if (HasMarker(p)) {
            wchar_t full[MAX_PATH * 4]{};
            DWORD n = ::GetFullPathNameW(p.c_str(), MAX_PATH * 4, full, nullptr);
            out.Add((n > 0 && n < MAX_PATH * 4) ? std::wstring(full, n) : p, source);
        }
    }
}

// -------- 1. 运行中的进程 ----------------------------------------------
void FromRunningProcesses(Collector& out, const Ctx& ctx) {
    static const wchar_t* kNames[] = {
        L"league of legends.exe",
        L"leagueclient.exe",
        L"leagueclientux.exe",
        L"riotclientservices.exe",
    };

    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe)) {
        do {
            if (ctx.Expired()) break;

            const std::wstring name = util::ToLower(pe.szExeFile);
            bool match = false;
            for (const wchar_t* n : kNames) if (name == n) { match = true; break; }
            if (!match) continue;

            HANDLE hp = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!hp) continue;

            wchar_t buf[MAX_PATH * 4]{};
            DWORD sz = MAX_PATH * 4;
            if (::QueryFullProcessImageNameW(hp, 0, buf, &sz) && sz > 0) {
                std::wstring exe(buf, sz);
                size_t slash = exe.find_last_of(L"\\/");
                if (slash != std::wstring::npos) {
                    const std::wstring dir = exe.substr(0, slash);
                    ProbeRoot(dir, L"运行中的进程", out, ctx);
                    // 再往上一级找（Game\xxx.exe 的情况）
                    size_t up = dir.find_last_of(L"\\/");
                    if (up != std::wstring::npos)
                        ProbeRoot(dir.substr(0, up), L"运行中的进程", out, ctx);
                }
            }
            ::CloseHandle(hp);
        } while (::Process32NextW(snap, &pe));
    }
    ::CloseHandle(snap);
}

// -------- 2. Riot 官方元数据 -------------------------------------------
// 从文本里抓取形如  key: "X:\path"  或  "key":"X:\\path"  的路径
void ExtractPathsFromText(const std::string& text, std::vector<std::wstring>& out) {
    const std::wstring w = util::Utf8ToWide(text);
    // 简单扫描：找出所有 "盘符:\..." 的片段
    for (size_t i = 0; i + 2 < w.size(); ++i) {
        if (((w[i] >= L'A' && w[i] <= L'Z') || (w[i] >= L'a' && w[i] <= L'z')) &&
            w[i + 1] == L':' && (w[i + 2] == L'\\' || w[i + 2] == L'/')) {
            size_t j = i;
            std::wstring path;
            while (j < w.size()) {
                wchar_t c = w[j];
                if (c == L'"' || c == L'\'' || c == L'\r' || c == L'\n' || c == L',') break;
                path.push_back(c);
                ++j;
            }
            // 处理 JSON 里的双反斜杠
            std::wstring norm;
            for (size_t k = 0; k < path.size(); ++k) {
                if (path[k] == L'\\' && k + 1 < path.size() && path[k + 1] == L'\\') { norm.push_back(L'\\'); ++k; }
                else norm.push_back(path[k]);
            }
            norm = util::Trim(norm);
            while (!norm.empty() && (norm.back() == L'\\' || norm.back() == L'/')) norm.pop_back();
            if (norm.size() > 3) out.push_back(norm);
            i = j;
        }
    }
}

void FromRiotMetadata(Collector& out, const Ctx& ctx) {
    std::vector<std::wstring> files;

    wchar_t pd[MAX_PATH]{};
    if (::GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH) > 0) {
        const std::wstring base = pd;
        files.push_back(util::JoinPath(base, L"Riot Games\\RiotClientInstalls.json"));
        files.push_back(util::JoinPath(base, L"Riot Games\\Metadata\\league_of_legends.live\\league_of_legends.live.product_settings.yaml"));
        files.push_back(util::JoinPath(base, L"Riot Games\\Metadata\\league_of_legends.live\\league_of_legends.live.product_settings.json"));
    }

    for (const auto& f : files) {
        if (ctx.Expired()) return;
        if (!util::FileExists(f)) continue;

        std::vector<uint8_t> data;
        if (!util::ReadWholeFile(f, data) || data.empty()) continue;
        if (data.size() > 2u * 1024 * 1024) continue;

        std::string text((const char*)data.data(), data.size());
        std::vector<std::wstring> paths;
        ExtractPathsFromText(text, paths);

        for (const auto& p : paths) {
            if (ctx.Expired()) return;
            // 路径可能指向 exe，取其目录
            std::wstring dir = p;
            if (util::FileExists(dir)) {
                size_t slash = dir.find_last_of(L"\\/");
                if (slash != std::wstring::npos) dir = dir.substr(0, slash);
            }
            if (!util::DirectoryExists(dir)) continue;
            ProbeRoot(dir, L"Riot 官方配置", out, ctx);
        }
    }
}

// -------- 3. 注册表 ------------------------------------------------------
std::wstring ReadRegString(HKEY root, const wchar_t* subKey, const wchar_t* value, REGSAM extra) {
    HKEY hk = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_READ | extra, &hk) != ERROR_SUCCESS) return L"";

    wchar_t buf[MAX_PATH * 2]{};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    std::wstring result;
    if (::RegQueryValueExW(hk, value, nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        result = buf;
        result = util::Trim(result);
        while (!result.empty() && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    }
    ::RegCloseKey(hk);
    return result;
}

void FromRegistry(Collector& out, const Ctx& ctx) {
    struct Item { HKEY root; const wchar_t* key; const wchar_t* val; };
    static const Item kItems[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Riot Game league_of_legends.live", L"InstallLocation" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Riot Game league_of_legends.live", L"InstallLocation" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Riot Game league_of_legends.live", L"InstallLocation" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\League of Legends", L"InstallLocation" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\League of Legends", L"InstallLocation" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Tencent\\LOL",  L"InstallPath" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Tencent\\LOL",               L"InstallPath" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Tencent\\LOL",               L"InstallPath" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Riot Games, Inc\\League of Legends", L"Location" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Riot Games, Inc\\League of Legends",              L"Location" },
    };

    for (const auto& it : kItems) {
        if (ctx.Expired()) return;
        for (REGSAM extra : { (REGSAM)0, (REGSAM)KEY_WOW64_64KEY, (REGSAM)KEY_WOW64_32KEY }) {
            const std::wstring p = ReadRegString(it.root, it.key, it.val, extra);
            if (p.empty()) continue;
            if (!util::DirectoryExists(p)) continue;
            ProbeRoot(p, L"注册表", out, ctx);
        }
    }
}

// -------- 4. 常见固定路径 ------------------------------------------------
std::vector<std::wstring> FixedDrives() {
    std::vector<std::wstring> drives;
    DWORD mask = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT t = ::GetDriveTypeW(root);
        if (t == DRIVE_FIXED || t == DRIVE_REMOVABLE) drives.push_back(root);
    }
    return drives;
}

void FromCommonPaths(Collector& out, const Ctx& ctx) {
    static const wchar_t* kRels[] = {
        L"Riot Games\\League of Legends",
        L"Program Files\\Riot Games\\League of Legends",
        L"Program Files (x86)\\Riot Games\\League of Legends",
        L"Games\\Riot Games\\League of Legends",
        L"League of Legends",
        L"Program Files\\League of Legends",
        L"Program Files (x86)\\League of Legends",
        L"WeGameApps\\rail_apps\\英雄联盟",
        L"WeGameApps\\英雄联盟",
        L"Program Files (x86)\\WeGameApps\\英雄联盟",
        L"英雄联盟",
        L"Tencent\\英雄联盟",
        L"Program Files (x86)\\Tencent\\英雄联盟",
        L"LOL\\英雄联盟",
        L"Riot Games",
    };

    for (const auto& d : FixedDrives()) {
        if (ctx.Expired()) return;
        for (const wchar_t* rel : kRels) {
            if (ctx.Expired()) return;
            const std::wstring p = util::JoinPath(d, rel);
            if (!util::DirectoryExists(p)) continue;
            ProbeRoot(p, L"常见安装路径", out, ctx);
        }
    }

    // 用户目录也扫一下（有人把脚本目录放在文档里）
    static const wchar_t* kEnvs[] = { L"LOCALAPPDATA", L"APPDATA", L"USERPROFILE" };
    for (const wchar_t* ev : kEnvs) {
        if (ctx.Expired()) return;
        wchar_t buf[MAX_PATH]{};
        if (::GetEnvironmentVariableW(ev, buf, MAX_PATH) == 0) continue;
        ProbeRoot(util::JoinPath(buf, L"League of Legends"), L"用户目录", out, ctx);
        ProbeRoot(util::JoinPath(buf, L"Riot Games\\League of Legends"), L"用户目录", out, ctx);
        ProbeRoot(util::JoinPath(buf, L"Documents\\League of Legends"), L"用户目录", out, ctx);
        ProbeRoot(util::JoinPath(buf, L"hanbot"), L"用户目录", out, ctx);
    }
}

// -------- 5. 有界遍历 ----------------------------------------------------
bool ShouldSkipDir(const std::wstring& lowerName) {
    static const wchar_t* kSkip[] = {
        L"windows", L"$recycle.bin", L"system volume information", L"recovery",
        L"perflogs", L"msocache", L"$windows.~bt", L"$windows.~ws",
        L"node_modules", L".git", L".svn", L"temp", L"tmp", L"cache",
        L"windowsapps", L"packages", L"package cache", L"driverstore",
        L"winsxs", L"assembly", L"installer", L"softwaredistribution",
    };
    for (const wchar_t* s : kSkip) if (lowerName == s) return true;
    return false;
}

// 广度优先扫描：matchSaves = true 时找名为 saves 的目录，
// 否则找名字含 "league of legends" / "英雄联盟" / "riot games" 的目录
void BoundedScan(Collector& out, const Ctx& ctx, int maxDepth, bool matchSaves,
                 const std::wstring& sourceTag) {
    struct Node { std::wstring path; int depth; };

    for (const auto& drive : FixedDrives()) {
        if (ctx.Expired()) return;

        std::deque<Node> queue;
        queue.push_back({ drive, 0 });
        size_t visited = 0;

        while (!queue.empty()) {
            if (ctx.Expired()) return;
            if (++visited > 200000) break;         // 硬上限，防止极端目录树拖死

            const Node node = queue.front();
            queue.pop_front();
            if (node.depth > maxDepth) continue;

            std::wstring pattern = node.path;
            if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
            pattern += L'*';

            WIN32_FIND_DATAW fd{};
            HANDLE h = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                          FindExSearchLimitToDirectories, nullptr,
                                          FIND_FIRST_EX_LARGE_FETCH);
            if (h == INVALID_HANDLE_VALUE) continue;

            do {
                if (ctx.Expired()) { ::FindClose(h); return; }
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;

                const std::wstring lower = util::ToLower(name);
                if (ShouldSkipDir(lower)) continue;

                const std::wstring child = util::JoinPath(node.path, name);

                if (matchSaves) {
                    if (lower == L"saves" && HasMarker(child)) {
                        out.Add(child, sourceTag);
                    }
                } else {
                    if (lower.find(L"league of legends") != std::wstring::npos ||
                        lower.find(L"英雄联盟") != std::wstring::npos ||
                        lower == L"riot games" || lower == L"hanbot") {
                        ProbeRoot(child, sourceTag, out, ctx);
                    }
                }

                if (node.depth < maxDepth)
                    queue.push_back({ child, node.depth + 1 });

            } while (::FindNextFileW(h, &fd));

            ::FindClose(h);
        }
    }
}

} // namespace

// ===========================================================================
// 对外接口
// ===========================================================================
bool HasMarker(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (!util::DirectoryExists(dir)) return false;
    return util::FileExists(util::JoinPath(dir, MARKER_FILE_W));
}

std::vector<Candidate> FindAll(const LogFn& log,
                               const std::atomic<bool>* cancel,
                               int timeoutSeconds) {
    Ctx ctx;
    ctx.log      = log;
    ctx.cancel   = cancel;
    ctx.deadline = (timeoutSeconds > 0)
                 ? (::GetTickCount64() + (ULONGLONG)timeoutSeconds * 1000ull)
                 : 0ull;

    Collector out;

    ctx.Log(L"[1/5] 检查正在运行的游戏进程...");
    FromRunningProcesses(out, ctx);
    if (!out.Empty()) { ctx.Log(L"      已命中，跳过后续深度扫描"); return out.Take(); }

    ctx.Log(L"[2/5] 读取 Riot 官方安装信息...");
    FromRiotMetadata(out, ctx);
    if (!out.Empty()) { ctx.Log(L"      已命中，跳过后续深度扫描"); return out.Take(); }

    ctx.Log(L"[3/5] 查询注册表安装记录...");
    FromRegistry(out, ctx);
    if (!out.Empty()) { ctx.Log(L"      已命中，跳过后续深度扫描"); return out.Take(); }

    ctx.Log(L"[4/5] 检查各磁盘常见安装路径...");
    FromCommonPaths(out, ctx);
    if (!out.Empty()) { ctx.Log(L"      已命中，跳过后续深度扫描"); return out.Take(); }

    ctx.Log(L"[5/5] 全盘有界扫描（可能需要十几秒，请稍候）...");
    BoundedScan(out, ctx, 4, false, L"目录遍历");
    if (!out.Empty()) return out.Take();

    BoundedScan(out, ctx, 6, true, L"深度遍历");
    return out.Take();
}

} // namespace lolfind
