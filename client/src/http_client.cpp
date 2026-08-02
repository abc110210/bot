#include "http_client.h"
#include "config.h"
#include "util.h"

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace http {

// ===========================================================================
// RAII 句柄
// ===========================================================================
namespace {

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : h_(h) {}
    ~WinHttpHandle() { if (h_) ::WinHttpCloseHandle(h_); }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle& operator=(HINTERNET h) {
        if (h_) ::WinHttpCloseHandle(h_);
        h_ = h;
        return *this;
    }
    operator HINTERNET() const { return h_; }
    bool Valid() const { return h_ != nullptr; }

private:
    HINTERNET h_ = nullptr;
};

struct UrlParts {
    std::wstring host;
    INTERNET_PORT port = 80;
    std::wstring pathWithQuery;
    bool secure = false;
    bool ok = false;
};

UrlParts CrackUrl(const std::wstring& url) {
    UrlParts p;

    URL_COMPONENTS uc{};
    uc.dwStructSize      = sizeof(uc);
    uc.dwSchemeLength    = (DWORD)-1;
    uc.dwHostNameLength  = (DWORD)-1;
    uc.dwUrlPathLength   = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;

    if (!::WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return p;

    p.host   = std::wstring(uc.lpszHostName, uc.dwHostNameLength);
    p.port   = uc.nPort;
    p.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    std::wstring path  = uc.dwUrlPathLength   ? std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength)     : L"/";
    std::wstring extra = uc.dwExtraInfoLength ? std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength) : L"";
    if (path.empty()) path = L"/";
    p.pathWithQuery = path + extra;
    p.ok = true;
    return p;
}

std::wstring BuildHeaderBlock(const std::vector<Header>& headers) {
    std::wstring out;
    for (const auto& h : headers) {
        if (h.first.empty()) continue;
        out += h.first;
        out += L": ";
        out += h.second;
        out += L"\r\n";
    }
    return out;
}

bool ReadResponseBody(HINTERNET hRequest, std::string& body, std::wstring& err) {
    body.clear();
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(hRequest, &avail)) {
            err = L"读取响应失败：" + util::LastErrorText(::GetLastError());
            return false;
        }
        if (avail == 0) break;
        if (body.size() + avail > (32u * 1024 * 1024)) {
            err = L"响应内容过大";
            return false;
        }

        const size_t old = body.size();
        body.resize(old + avail);
        DWORD read = 0;
        if (!::WinHttpReadData(hRequest, &body[old], avail, &read)) {
            err = L"读取响应失败：" + util::LastErrorText(::GetLastError());
            return false;
        }
        body.resize(old + read);
        if (read == 0) break;
    }
    return true;
}

int QueryStatus(HINTERNET hRequest) {
    DWORD status = 0;
    DWORD sz = sizeof(status);
    if (::WinHttpQueryHeaders(hRequest,
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                              WINHTTP_NO_HEADER_INDEX)) {
        return (int)status;
    }
    return 0;
}

// 打开 session / connect / request 三件套
bool OpenChain(const UrlParts& parts,
               const std::wstring& method,
               const Timeouts& to,
               WinHttpHandle& session,
               WinHttpHandle& connect,
               WinHttpHandle& request,
               std::wstring& err) {
    session = ::WinHttpOpen(APP_UA_W,
                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.Valid()) {
        // 老系统不支持 AUTOMATIC_PROXY，退回默认代理配置
        session = ::WinHttpOpen(APP_UA_W,
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.Valid()) {
        err = L"初始化网络会话失败：" + util::LastErrorText(::GetLastError());
        return false;
    }

    ::WinHttpSetTimeouts(session, to.connectMs, to.connectMs, to.sendMs, to.recvMs);

    connect = ::WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (!connect.Valid()) {
        err = L"连接服务器失败：" + util::LastErrorText(::GetLastError());
        return false;
    }

    const DWORD flags = parts.secure ? WINHTTP_FLAG_SECURE : 0;
    request = ::WinHttpOpenRequest(connect, method.c_str(), parts.pathWithQuery.c_str(),
                                   nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request.Valid()) {
        err = L"创建请求失败：" + util::LastErrorText(::GetLastError());
        return false;
    }

    // 关掉自动重定向之外的一些坑：禁用 keep-alive 复用带来的偶发问题
    DWORD disableFeature = WINHTTP_DISABLE_COOKIES;
    ::WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disableFeature, sizeof(disableFeature));

    return true;
}

} // namespace

// ===========================================================================
// 普通请求
// ===========================================================================
Response Request(const std::wstring& method,
                 const std::wstring& url,
                 const std::string&  body,
                 const std::vector<Header>& headers,
                 const Timeouts& to) {
    Response res;

    const UrlParts parts = CrackUrl(url);
    if (!parts.ok) {
        res.error = L"地址格式不正确：" + url;
        return res;
    }

    WinHttpHandle session, connect, request;
    if (!OpenChain(parts, method, to, session, connect, request, res.error)) return res;

    const std::wstring hdr = BuildHeaderBlock(headers);

    BOOL sent = ::WinHttpSendRequest(
        request,
        hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
        hdr.empty() ? 0 : (DWORD)-1,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(),
        (DWORD)body.size(),
        0);

    if (!sent) {
        res.error = L"发送请求失败：" + util::LastErrorText(::GetLastError());
        return res;
    }

    if (!::WinHttpReceiveResponse(request, nullptr)) {
        res.error = L"接收响应失败：" + util::LastErrorText(::GetLastError());
        return res;
    }

    res.status = QueryStatus(request);
    if (!ReadResponseBody(request, res.body, res.error)) return res;

    res.ok = true;
    return res;
}

// ===========================================================================
// multipart 上传
// ===========================================================================
Response UploadMultipartFile(const std::wstring& url,
                             const std::vector<std::pair<std::string, std::string>>& fields,
                             const std::string& fileFieldName,
                             const std::string& fileNameInForm,
                             const std::wstring& localFilePath,
                             const std::vector<Header>& headers,
                             const Timeouts& to,
                             const UploadProgressFn& progress,
                             const std::atomic<bool>* cancel) {
    Response res;

    // ---- 文件大小 ----
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(localFilePath.c_str(), GetFileExInfoStandard, &fad)) {
        res.error = L"待上传文件不存在";
        return res;
    }
    ULARGE_INTEGER fileSize{};
    fileSize.LowPart  = fad.nFileSizeLow;
    fileSize.HighPart = fad.nFileSizeHigh;

    // ---- 生成 boundary ----
    uint8_t rnd[16]{};
    util::RandomBytes(rnd, sizeof(rnd));
    std::string boundary = "----HanbotUploader";
    {
        static const char* hex = "0123456789abcdef";
        for (unsigned char c : rnd) {
            boundary.push_back(hex[(c >> 4) & 0xF]);
            boundary.push_back(hex[c & 0xF]);
        }
    }

    // ---- 拼装前后段 ----
    std::string prologue;
    for (const auto& kv : fields) {
        prologue += "--" + boundary + "\r\n";
        prologue += "Content-Disposition: form-data; name=\"" + kv.first + "\"\r\n\r\n";
        prologue += kv.second + "\r\n";
    }
    prologue += "--" + boundary + "\r\n";
    prologue += "Content-Disposition: form-data; name=\"" + fileFieldName +
                "\"; filename=\"" + fileNameInForm + "\"\r\n";
    prologue += "Content-Type: application/octet-stream\r\n\r\n";

    const std::string epilogue = "\r\n--" + boundary + "--\r\n";

    const unsigned long long totalLen =
        (unsigned long long)prologue.size() + fileSize.QuadPart + epilogue.size();

    if (totalLen > 0xFFFFFFFFull) {
        res.error = L"文件过大，超出单次上传上限（4GB）";
        return res;
    }

    // ---- 建立连接 ----
    const UrlParts parts = CrackUrl(url);
    if (!parts.ok) {
        res.error = L"上传地址格式不正确：" + url;
        return res;
    }

    WinHttpHandle session, connect, request;
    if (!OpenChain(parts, L"POST", to, session, connect, request, res.error)) return res;

    std::vector<Header> hs = headers;
    hs.emplace_back(L"Content-Type",
                    L"multipart/form-data; boundary=" + util::Utf8ToWide(boundary));
    const std::wstring hdr = BuildHeaderBlock(hs);

    if (!::WinHttpSendRequest(request,
                              hdr.c_str(), (DWORD)-1,
                              WINHTTP_NO_REQUEST_DATA, 0,
                              (DWORD)totalLen, 0)) {
        res.error = L"发起上传失败：" + util::LastErrorText(::GetLastError());
        return res;
    }

    unsigned long long sentBytes = 0;
    auto writeChunk = [&](const void* data, DWORD len) -> bool {
        const uint8_t* p = (const uint8_t*)data;
        DWORD left = len;
        while (left > 0) {
            DWORD wrote = 0;
            if (!::WinHttpWriteData(request, p, left, &wrote) || wrote == 0) return false;
            p += wrote;
            left -= wrote;
            sentBytes += wrote;
        }
        return true;
    };

    // 前段
    if (!writeChunk(prologue.data(), (DWORD)prologue.size())) {
        res.error = L"上传中断：" + util::LastErrorText(::GetLastError());
        return res;
    }

    // 文件体
    {
        HANDLE hf = ::CreateFileW(localFilePath.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hf == INVALID_HANDLE_VALUE) {
            res.error = L"无法读取待上传文件";
            return res;
        }

        std::vector<uint8_t> buf(256 * 1024);
        unsigned long long fileSent = 0;
        bool ioOk = true;

        while (fileSent < fileSize.QuadPart) {
            if (cancel && cancel->load()) { ioOk = false; res.error = L"已取消"; break; }

            DWORD got = 0;
            if (!::ReadFile(hf, buf.data(), (DWORD)buf.size(), &got, nullptr) || got == 0) {
                ioOk = false;
                if (res.error.empty()) res.error = L"读取待上传文件失败";
                break;
            }
            if (fileSent + got > fileSize.QuadPart)
                got = (DWORD)(fileSize.QuadPart - fileSent);

            if (!writeChunk(buf.data(), got)) {
                ioOk = false;
                res.error = L"上传中断：" + util::LastErrorText(::GetLastError());
                break;
            }
            fileSent += got;

            if (progress && !progress(sentBytes, totalLen)) {
                ioOk = false;
                res.error = L"已取消";
                break;
            }
        }
        ::CloseHandle(hf);
        if (!ioOk) return res;
    }

    // 后段
    if (!writeChunk(epilogue.data(), (DWORD)epilogue.size())) {
        res.error = L"上传收尾失败：" + util::LastErrorText(::GetLastError());
        return res;
    }
    if (progress) progress(totalLen, totalLen);

    if (!::WinHttpReceiveResponse(request, nullptr)) {
        res.error = L"等待服务器响应失败：" + util::LastErrorText(::GetLastError());
        return res;
    }

    res.status = QueryStatus(request);
    if (!ReadResponseBody(request, res.body, res.error)) return res;

    res.ok = true;
    return res;
}

// ===========================================================================
// 下载到文件
// ===========================================================================
Response DownloadToFile(const std::wstring& url,
                        const std::wstring& localPath,
                        const std::vector<Header>& headers,
                        const Timeouts& to,
                        const DownloadProgressFn& progress,
                        const std::atomic<bool>* cancel) {
    Response res;

    const UrlParts parts = CrackUrl(url);
    if (!parts.ok) {
        res.error = L"下载地址格式不正确：" + url;
        return res;
    }

    WinHttpHandle session, connect, request;
    if (!OpenChain(parts, L"GET", to, session, connect, request, res.error)) return res;

    const std::wstring hdr = BuildHeaderBlock(headers);
    if (!::WinHttpSendRequest(request,
                               hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
                               hdr.empty() ? 0 : (DWORD)-1,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        res.error = L"发起下载失败：" + util::LastErrorText(::GetLastError());
        return res;
    }

    if (!::WinHttpReceiveResponse(request, nullptr)) {
        res.error = L"接收下载响应失败：" + util::LastErrorText(::GetLastError());
        return res;
    }

    res.status = QueryStatus(request);
    if (res.status != 200) {
        // 防盗链 403 / 404 等：尽量读一点错误体，方便 UI 提示
        ReadResponseBody(request, res.body, res.error);
        return res;
    }

    HANDLE hf = ::CreateFileW(localPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hf == INVALID_HANDLE_VALUE) {
        res.error = L"无法创建下载文件：" + localPath;
        return res;
    }

    unsigned long long total = 0;
    {
        DWORD cl = 0, sz = sizeof(cl);
        if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &cl, &sz, WINHTTP_NO_HEADER_INDEX))
            total = cl;
    }

    unsigned long long received = 0;
    bool ioOk = true;
    std::vector<uint8_t> buf(256 * 1024);

    for (;;) {
        if (cancel && cancel->load()) { ioOk = false; res.error = L"已取消"; break; }

        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(request, &avail)) {
            ioOk = false; res.error = L"下载中断：" + util::LastErrorText(::GetLastError()); break;
        }
        if (avail == 0) break;
        if (avail > (DWORD)buf.size()) avail = (DWORD)buf.size();

        DWORD got = 0;
        if (!::WinHttpReadData(request, buf.data(), avail, &got) || got == 0) {
            ioOk = false; res.error = L"读取下载数据失败：" + util::LastErrorText(::GetLastError()); break;
        }

        DWORD wrote = 0;
        if (!::WriteFile(hf, buf.data(), got, &wrote, nullptr) || wrote != got) {
            ioOk = false; res.error = L"写入下载文件失败"; break;
        }
        received += got;

        if (progress && !progress(received, total)) {
            ioOk = false; res.error = L"已取消"; break;
        }
    }
    ::CloseHandle(hf);
    if (!ioOk) return res;

    res.ok = true;
    return res;
}

// ===========================================================================
std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

} // namespace http
