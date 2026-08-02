#include "zip_reader.h"
#include "deflate.h"
#include "util.h"
#include "inflate.h"   // inflate::Decompress —— DEFLATE 解压

#include <algorithm>
#include <vector>
#include <cstdint>

namespace zipr {

namespace {

// ===========================================================================
// ZipCrypto 解密（与 zip_writer 写端对称）
// ===========================================================================
class ZipCrypto {
public:
    void Init(const std::string& password) {
        k0_ = 0x12345678u;
        k1_ = 0x23456789u;
        k2_ = 0x34567890u;
        for (unsigned char c : password) UpdateKeys(c);
    }

    // 原地解密：cipher -> plain，并推进密钥状态（与写端顺序一致）
    void Decrypt(uint8_t* buf, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const uint8_t plain = (uint8_t)(buf[i] ^ StreamByte());
            UpdateKeys(plain);
            buf[i] = plain;
        }
    }

    uint8_t StreamByte() const {
        const uint16_t t = (uint16_t)((k2_ & 0xFFFFu) | 2u);
        return (uint8_t)(((t * (t ^ 1u)) >> 8) & 0xFFu);
    }

private:
    void UpdateKeys(uint8_t c) {
        k0_ = deflate::Crc32Step(k0_, c);
        k1_ = k1_ + (k0_ & 0xFFu);
        k1_ = k1_ * 134775813u + 1u;
        k2_ = deflate::Crc32Step(k2_, (uint8_t)(k1_ >> 24));
    }

    uint32_t k0_ = 0, k1_ = 0, k2_ = 0;
};

// ===========================================================================
// 小工具
// ===========================================================================
inline uint16_t LE16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t LE32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool SeekTo(HANDLE h, unsigned long long off) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    return ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0;
}

bool ReadExact(HANDLE h, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n > 0) {
        DWORD want = (DWORD)(n > 0x100000 ? 0x100000 : n);
        DWORD got = 0;
        if (!::ReadFile(h, p, want, &got, nullptr) || got == 0) return false;
        p += got; n -= got;
    }
    return true;
}

// 逐层创建目录（已存在则忽略）
bool EnsureDirExists(const std::wstring& dir) {
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        const wchar_t c = dir[i];
        if (c == L'\\' || c == L'/') {
            if (!cur.empty() && cur.back() != L'\\' && cur.back() != L'/') {
                const std::wstring part = cur + L"\\";
                if (!::CreateDirectoryW(part.c_str(), nullptr)) {
                    const DWORD e = ::GetLastError();
                    if (e != ERROR_ALREADY_EXISTS) return false;
                }
            }
        }
        cur.push_back(c);
    }
    if (!cur.empty() && cur.back() != L'\\' && cur.back() != L'/') {
        if (!::CreateDirectoryW(cur.c_str(), nullptr)) {
            const DWORD e = ::GetLastError();
            if (e != ERROR_ALREADY_EXISTS) return false;
        }
    }
    return true;
}

// 创建文件父目录
bool CreateParentDirs(const std::wstring& filePath) {
    std::wstring d = filePath;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    const size_t slash = d.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return true;
    return EnsureDirExists(d.substr(0, slash));
}

// 把内部条目名映射到目标目录；剥离前缀 "saves/" 或 "saves\\"
// 拒绝「..」以防穿越。isRoot = 内部名就是被剥离后的根（如 "saves/"）。
bool MapTarget(const std::wstring& targetDir, const std::string& nameUtf8,
               std::wstring& outPath, bool& isRoot) {
    std::wstring w = util::Utf8ToWide(nameUtf8);
    for (auto& c : w) if (c == L'/') c = L'\\';

    const std::wstring prefix = L"saves\\";
    if (w.size() >= prefix.size() && _wcsnicmp(w.c_str(), prefix.c_str(), prefix.size()) == 0)
        w = w.substr(prefix.size());

    isRoot = false;
    if (w.empty()) { isRoot = true; outPath = targetDir; return true; }

    std::vector<std::wstring> parts;
    size_t pos = 0;
    while (pos < w.size()) {
        const size_t slash = w.find_first_of(L"\\/", pos);
        const size_t end = (slash == std::wstring::npos) ? w.size() : slash;
        const std::wstring comp = w.substr(pos, end - pos);
        if (comp == L"..") return false;            // 路径穿越，拒绝
        if (!comp.empty() && comp != L".") parts.push_back(comp);
        if (slash == std::wstring::npos) break;
        pos = slash + 1;
    }

    std::wstring base = targetDir;
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();

    std::wstring full = base;
    for (const auto& p : parts) { full += L"\\"; full += p; }
    outPath = full;
    return true;
}

// ===========================================================================
// 定位 EOCD
// ===========================================================================
bool FindEocd(HANDLE h, unsigned long long fileSize,
              uint32_t& cdOffset, uint32_t& cdSize, uint16_t& entryCount) {
    if (fileSize < 22) return false;
    const size_t tailCap = (size_t)std::min<unsigned long long>(fileSize, (unsigned long long)(22 + 65535));
    std::vector<uint8_t> tail(tailCap);
    if (!SeekTo(h, fileSize - tailCap)) return false;
    if (!ReadExact(h, tail.data(), tailCap)) return false;

    // 从尾部往前搜签名 0x06054B50（字节序 50 4B 05 06）
    for (size_t i = tailCap; i >= 4; --i) {
        const size_t p = i - 4;
        if (tail[p] == 0x50 && tail[p + 1] == 0x4B && tail[p + 2] == 0x05 && tail[p + 3] == 0x06) {
            const uint32_t csize  = LE32(&tail[p + 12]);
            const uint32_t coff   = LE32(&tail[p + 16]);
            const uint16_t ecnt   = LE16(&tail[p + 10]);
            if ((unsigned long long)coff + csize <= fileSize) {
                cdOffset = coff; cdSize = csize; entryCount = ecnt;
                return true;
            }
        }
    }
    return false;
}

struct CentEntry {
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t crc = 0;
    uint32_t csize = 0;
    uint32_t usize = 0;
    uint32_t localOffset = 0;
    std::string nameUtf8;
};

} // namespace

// ===========================================================================
// 解压主流程
// ===========================================================================
ExtractResult ExtractEncryptedZip(const std::wstring& zipPath,
                                  const std::wstring& targetDir,
                                  const std::string& password) {
    ExtractResult res;

    HANDLE h = ::CreateFileW(zipPath.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        res.error = L"无法打开压缩包：" + zipPath;
        return res;
    }

    auto closeFile = [&]() { ::CloseHandle(h); };

    // ---- 文件大小 ----
    LARGE_INTEGER fsz{};
    if (!::GetFileSizeEx(h, &fsz)) { res.error = L"读取压缩包大小失败"; closeFile(); return res; }
    const unsigned long long fileSize = (unsigned long long)fsz.QuadPart;

    // ---- 定位 EOCD ----
    uint32_t cdOffset = 0, cdSize = 0;
    uint16_t entryCount = 0;
    if (!FindEocd(h, fileSize, cdOffset, cdSize, entryCount)) {
        res.error = L"不是有效的 ZIP 文件（找不到结尾记录）";
        closeFile(); return res;
    }

    // ---- 读中央目录 ----
    if ((unsigned long long)cdOffset + cdSize > fileSize) {
        res.error = L"压缩包索引越界，可能已损坏";
        closeFile(); return res;
    }
    std::vector<uint8_t> cd(cdSize);
    if (!SeekTo(h, cdOffset) || !ReadExact(h, cd.data(), cdSize)) {
        res.error = L"读取压缩包索引失败";
        closeFile(); return res;
    }

    // ---- 解析中央目录条目 ----
    std::vector<CentEntry> entries;
    entries.reserve(entryCount ? entryCount : 64);
    size_t pos = 0;
    while (pos + 46 <= cd.size()) {
        if (LE32(&cd[pos]) != 0x02014B50u) break;   // 中央目录头签名
        CentEntry e;
        e.flags       = LE16(&cd[pos + 8]);
        e.method      = LE16(&cd[pos + 10]);
        e.crc         = LE32(&cd[pos + 16]);
        e.csize       = LE32(&cd[pos + 20]);
        e.usize       = LE32(&cd[pos + 24]);
        const uint16_t nameLen  = LE16(&cd[pos + 28]);
        const uint16_t extraLen = LE16(&cd[pos + 30]);
        const uint16_t commLen  = LE16(&cd[pos + 32]);
        e.localOffset = LE32(&cd[pos + 42]);
        e.nameUtf8.assign((const char*)&cd[pos + 46], nameLen);

        entries.push_back(std::move(e));
        const size_t adv = 46u + (size_t)nameLen + extraLen + commLen;
        if (adv < 46) break;            // 防止回绕
        pos += adv;
    }

    // ---- 逐个条目解压 ----
    for (const auto& e : entries) {
        const bool isDir = (!e.nameUtf8.empty() && e.nameUtf8.back() == '/') ||
                           (e.csize == 0 && e.method == 0 && (e.flags & 0x0001) == 0);

        std::wstring outPath;
        bool isRoot = false;
        if (!MapTarget(targetDir, e.nameUtf8, outPath, isRoot)) {
            res.error = L"压缩包内含有非法路径（疑似路径穿越），已中止解压";
            closeFile(); return res;
        }

        if (isDir || isRoot) {
            if (outPath.empty()) outPath = targetDir;
            EnsureDirExists(outPath);
            res.dirCount++;
            continue;
        }

        // ---- 文件条目：读本地文件头，定位数据起点 ----
        if ((unsigned long long)e.localOffset + 30 > fileSize) {
            res.error = L"本地文件头越界：" + util::Utf8ToWide(e.nameUtf8);
            closeFile(); return res;
        }
        uint8_t lh[30];
        if (!SeekTo(h, e.localOffset) || !ReadExact(h, lh, 30)) {
            res.error = L"读取本地文件头失败：" + util::Utf8ToWide(e.nameUtf8);
            closeFile(); return res;
        }
        if (LE32(lh) != 0x04034B50u) {
            res.error = L"本地文件头签名错误：" + util::Utf8ToWide(e.nameUtf8);
            closeFile(); return res;
        }
        const uint16_t lNameLen  = LE16(&lh[26]);
        const uint16_t lExtraLen = LE16(&lh[28]);
        const unsigned long long dataStart = (unsigned long long)e.localOffset + 30 +
                                             (unsigned long long)lNameLen + (unsigned long long)lExtraLen;
        if (dataStart + e.csize > fileSize) {
            res.error = L"文件数据越界：" + util::Utf8ToWide(e.nameUtf8);
            closeFile(); return res;
        }

        const bool encrypted = (e.flags & 0x0001) != 0;
        ZipCrypto crypto;
        if (encrypted) crypto.Init(password);

        if (e.method == 8) {
            // DEFLATE：整体读入再解密再解压
            std::vector<uint8_t> comp(e.csize);
            if (!SeekTo(h, dataStart) || !ReadExact(h, comp.data(), e.csize)) {
                res.error = L"读取压缩数据失败：" + util::Utf8ToWide(e.nameUtf8);
                closeFile(); return res;
            }
            if (encrypted) {
                crypto.Decrypt(comp.data(), e.csize);
                if (e.csize < 12 || comp[11] != (uint8_t)((e.crc >> 24) & 0xFF)) {
                    res.passwordWrong = true;
                    res.error = L"密码不正确，无法解密该压缩包";
                    closeFile(); return res;
                }
            }
            const size_t off = encrypted ? 12 : 0;
            std::vector<uint8_t> plain;
            if (!inflate::Decompress(comp.data() + off, e.csize - off, plain)) {
                res.error = L"解压失败（数据可能已损坏）：" + util::Utf8ToWide(e.nameUtf8);
                closeFile(); return res;
            }
            if (!CreateParentDirs(outPath) ||
                !util::WriteWholeFile(outPath, plain.data(), plain.size())) {
                res.error = L"写入文件失败：" + outPath;
                closeFile(); return res;
            }
            res.writtenBytes += plain.size();
            res.fileCount++;

        } else if (e.method == 0) {
            // 存储型：流式读 + 解密 + 写
            if (!CreateParentDirs(outPath)) {
                res.error = L"创建目录失败：" + outPath;
                closeFile(); return res;
            }
            HANDLE out = ::CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (out == INVALID_HANDLE_VALUE) {
                res.error = L"写入文件失败：" + outPath;
                closeFile(); return res;
            }

            bool ioOk = true;
            auto failWrite = [&]() {
                ioOk = false;
                res.error = L"写入文件失败：" + outPath;
            };

            if (!SeekTo(h, dataStart)) { ::CloseHandle(out); closeFile(); return res; }

            if (encrypted) {
                uint8_t hdr[12];
                if (!ReadExact(h, hdr, 12)) { failWrite(); }
                else {
                    crypto.Decrypt(hdr, 12);
                    if (hdr[11] != (uint8_t)((e.crc >> 24) & 0xFF)) {
                        res.passwordWrong = true;
                        res.error = L"密码不正确，无法解密该压缩包";
                        ioOk = false;
                    }
                }
            }

            unsigned long long remaining = e.csize - (encrypted ? 12 : 0);
            std::vector<uint8_t> buf(256 * 1024);
            while (ioOk && remaining > 0) {
                DWORD want = (DWORD)std::min<unsigned long long>(remaining, buf.size());
                if (!ReadExact(h, buf.data(), want)) { failWrite(); break; }
                if (encrypted) crypto.Decrypt(buf.data(), want);
                DWORD wrote = 0;
                if (!::WriteFile(out, buf.data(), want, &wrote, nullptr) || wrote != want) { failWrite(); break; }
                remaining -= wrote;
                res.writtenBytes += wrote;
            }
            ::CloseHandle(out);
            if (!ioOk) { closeFile(); return res; }
            res.fileCount++;

        } else {
            res.error = L"不支持的压缩方式（method=" + std::to_wstring(e.method) + L"）：" +
                        util::Utf8ToWide(e.nameUtf8);
            closeFile(); return res;
        }
    }

    closeFile();
    res.ok = true;
    return res;
}

} // namespace zipr
