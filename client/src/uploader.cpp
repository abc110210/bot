#include "uploader.h"
#include "config.h"
#include "util.h"
#include "zip_writer.h"
#include "zip_reader.h"
#include "http_client.h"
#include "json_mini.h"
#include "lol_finder.h"

#include <windows.h>

namespace uploader {

namespace {

http::Timeouts MakeTimeouts() {
    http::Timeouts t;
    t.connectMs = config::ConnectTimeoutMs;
    t.sendMs    = config::SendTimeoutMs;
    t.recvMs    = config::RecvTimeoutMs;
    return t;
}

// 发送前自检请求体是否为合法 JSON 对象。
// 手拼 JSON 曾因字段间漏逗号导致服务端 400（还误以为是版本/网络问题），
// 这里兜底：拼坏了客户端直接报内部错误，而不是发一个坏请求出去。
bool JsonOk(const std::string& s) {
    auto v = json::Parse(s);
    return v && v->IsObject();
}

std::vector<http::Header> AuthHeaders() {
    std::vector<http::Header> h;
    h.emplace_back(OBFW("WC1DbGllbnQtS2V5"), util::Utf8ToWide(config::ClientKey));
    h.emplace_back(OBFW("WC1DbGllbnQtVmVyc2lvbg=="), APP_VERSION_W);
    return h;
}

// 临时文件自动清理
class ScopedTempFile {
public:
    explicit ScopedTempFile(std::wstring path) : path_(std::move(path)) {}
    ~ScopedTempFile() {
        if (!keep_ && !path_.empty()) {
            ::SetFileAttributesW(path_.c_str(), FILE_ATTRIBUTE_NORMAL);
            ::DeleteFileW(path_.c_str());
        }
    }
    void Keep() { keep_ = true; }
    const std::wstring& Path() const { return path_; }

private:
    std::wstring path_;
    bool         keep_ = false;
};

std::wstring PrettyHttpError(const http::Response& r, const std::wstring& what) {
    if (!r.error.empty()) return what + OBFW("77ya") + r.error;

    std::wstring detail;
    if (!r.body.empty()) {
        auto v = json::Parse(r.body);
        if (v && v->IsObject()) {
            const std::string m = v->GetStr("message", v->GetStr("error", ""));
            if (!m.empty()) detail = OBFW("77yM5pyN5Yqh5Zmo5o+Q56S677ya") + util::Utf8ToWide(m);
        }
        if (detail.empty()) {
            std::string b = r.body.substr(0, 300);
            detail = OBFW("77yM6L+U5Zue5YaF5a6577ya") + util::Utf8ToWide(b);
        }
    }

    wchar_t buf[64]{};
    ::swprintf(buf, 64, OBFW("77yISFRUUCAlZO+8iQ=="), r.status);
    return what + buf + detail;
}

} // namespace

// ===========================================================================
Outcome Run(const std::wstring& savesDir,
            const std::wstring& password,
            const LogFn& log,
            const ProgressFn& progress,
            const std::atomic<bool>* cancel) {
    Outcome out;

    auto L = [&](const std::wstring& s) { if (log) log(s); };
    auto P = [&](int permille, const std::wstring& stage) { if (progress) progress(permille, stage); };
    auto Canceled = [&]() { return cancel && cancel->load(); };

    // ---------------- 0. 前置校验 ----------------
    P(0, OBFW("5qOA5p+l55uu5b2V"));
    if (savesDir.empty() || !util::DirectoryExists(savesDir)) {
        out.error = OBFW("55uu5b2V5LiN5a2Y5Zyo77yM6K+35YWI5qOA5rWL5oiW5omL5Yqo6YCJ5oupIHNhdmVzIOebruW9lQ==");
        return out;
    }
    if (!lolfind::HasMarker(savesDir)) {
        out.error = L"该目录里没有找到 " MARKER_FILE_W L"，不是有效的 saves 目录";
        return out;
    }
    if (!util::IsValidPassword(password)) {
        out.error = OBFW("5a+G56CB5b+F6aG75pivIDQtMjQg5L2N5a2X5q+N5oiW5pWw5a2X");
        return out;
    }
    L(OBFW("55uu5qCH55uu5b2V77ya") + savesDir);

    // ---------------- 1. 扫描 ----------------
    P(10, OBFW("5q2j5Zyo5omr5o+P5paH5Lu2"));
    std::vector<zipw::Entry> entries;
    zipw::ScanResult scan = zipw::ScanDirectory(savesDir, OBFW("c2F2ZXM="), entries, cancel);
    if (Canceled()) { out.canceled = true; out.error = OBFW("5bey5Y+W5raI"); return out; }
    if (!scan.ok) { out.error = scan.error.empty() ? OBFW("5omr5o+P55uu5b2V5aSx6LSl") : scan.error; return out; }

    {
        wchar_t buf[256]{};
        ::swprintf(buf, 256, OBFW("5YWxICVsbHUg5Liq5paH5Lu244CBJWxsdSDkuKrlrZDnm67lvZXvvIzljp/lp4vlpKflsI8gJXM="),
                   (unsigned long long)scan.fileCount,
                   (unsigned long long)scan.dirCount,
                   util::FormatSize(scan.totalBytes).c_str());
        L(buf);
    }

    if (scan.fileCount == 0) {
        out.error = OBFW("55uu5b2V6YeM5rKh5pyJ5Lu75L2V5paH5Lu277yM5peg6ZyA5LiK5Lyg");
        return out;
    }
    // 体积上限由服务端 MAX_UPLOAD_BYTES 校验，客户端不再做本地拦截。

    // ---------------- 2. 使用用户输入的密码 ----------------
    const std::string pw = util::WideToUtf8(password);
    out.password = password;
    L(OBFW("5bCG5L2/55So5oKo6L6T5YWl55qE5a+G56CB6L+b6KGM5Yqg5a+G5omT5YyF77yI6K+35Yqh5b+F54mi6K6w5q2k5a+G56CB77yM5LiL6L295pe26ZyA55So5ZCM5LiA5a+G56CB77yJ"));

    // ---------------- 3. 打包 ----------------
    const std::wstring stamp = util::TimestampCompact();
    const std::wstring machine = util::MachineId();
    const std::wstring zipName = OBFW("c2F2ZXNf") + machine + OBFW("Xw==") + stamp + OBFW("LnppcA==");
    const std::wstring zipPath = util::JoinPath(util::GetTempDir(), OBFW("aGFuYm90Xw==") + zipName);

    ScopedTempFile temp(zipPath);

    P(60, OBFW("5q2j5Zyo5omT5YyF"));
    L(OBFW("5q2j5Zyo5Yqg5a+G5omT5YyF77yM6K+356iN5YCZLi4u"));

    auto packProgress = [&](unsigned long long done, unsigned long long total,
                            const std::wstring& cur) -> bool {
        if (Canceled()) return false;
        int permille = 60;
        if (total > 0) {
            // 打包阶段占总进度 60‰ ~ 550‰
            permille = 60 + (int)((done * 490ull) / total);
            if (permille > 550) permille = 550;
        }
        std::wstring shortName = cur;
        if (shortName.size() > 48) shortName = OBFW("Li4u") + shortName.substr(shortName.size() - 45);
        P(permille, OBFW("5omT5YyF5LitIA==") + shortName);
        return true;
    };

    zipw::PackResult pack = zipw::CreateEncryptedZip(zipPath, entries, pw, packProgress, cancel);
    if (Canceled() || pack.error == OBFW("5bey5Y+W5raI")) { out.canceled = true; out.error = OBFW("5bey5Y+W5raI"); return out; }
    if (!pack.ok) { out.error = pack.error.empty() ? OBFW("5omT5YyF5aSx6LSl") : pack.error; return out; }

    out.rawBytes  = pack.rawBytes;
    out.zipBytes  = pack.zipBytes;
    out.fileCount = pack.fileCount;
    out.skipped   = pack.skipped;

    {
        wchar_t buf[256]{};
        ::swprintf(buf, 256, OBFW("5omT5YyF5a6M5oiQ77yaJXMgLT4gJXPvvIglbGx1IOS4quaWh+S7tiVz77yJ"),
                   util::FormatSize(pack.rawBytes).c_str(),
                   util::FormatSize(pack.zipBytes).c_str(),
                   (unsigned long long)pack.fileCount,
                   pack.skipped ? OBFW("77yM5Y+m5pyJ6YOo5YiG5paH5Lu26KKr5Y2g55So5bey6Lez6L+H") : L"");
        L(buf);
    }

    // ---------------- 4. 申请上传凭证 ----------------
    P(560, OBFW("5q2j5Zyo55Sz6K+35LiK5Lyg5Yet6K+B"));
    L(OBFW("5q2j5Zyo5ZCR5pyN5Yqh5Zmo55Sz6K+35LiK5Lyg5Yet6K+BLi4u"));

    std::string reqJson;
    {
        reqJson  = OBFA("ew==");
        reqJson += OBFA("Im1hY2hpbmUiOiI=")   + json::EscapeString(util::WideToUtf8(machine))  + OBFA("Iiw=");
        reqJson += OBFA("ImZpbGVuYW1lIjoi")  + json::EscapeString(util::WideToUtf8(zipName))  + OBFA("Iiw=");
        reqJson += OBFA("InNpemUiOg==")        + std::to_string(pack.zipBytes) + OBFA("LA==");
        reqJson += OBFA("InJhd19zaXplIjo=")    + std::to_string(pack.rawBytes) + OBFA("LA==");
        reqJson += OBFA("ImZpbGVfY291bnQiOg==")  + std::to_string(pack.fileCount) + OBFA("LA==");
        reqJson += OBFA("InNvdXJjZV9kaXIiOiI=") + json::EscapeString(util::WideToUtf8(savesDir)) + OBFA("Iiw=");
        // 带密码申请凭证：后端据此判断「同密码覆盖上传」——已有记录则复用旧 key
        // 签发覆盖式凭证，新存档直接覆盖原存档；没有记录才发全新 key。
        reqJson += OBFA("InBhc3N3b3JkIjoi") + json::EscapeString(pw) + OBFA("Ig==");
        reqJson += OBFA("fQ==");
    }

    // 发送前自检（防手拼 JSON 漏逗号类 bug）
    if (!JsonOk(reqJson)) {
        out.error = OBFW("5a6i5oi356uv5YaF6YOo6ZSZ6K+v77ya55Sf5oiQ55qE6K+35rGC5L2T5LiN5ZCI5rOV77yM6K+36YeN6K+V");
        return out;
    }

    const std::wstring tokenUrl = config::BackendBaseUrl + OBFW("L2FwaS91cGxvYWQtdG9rZW4=");
    http::Response tr = http::PostJson(tokenUrl, reqJson, AuthHeaders(), MakeTimeouts());

    if (!tr.ok || !tr.Is2xx()) {
        out.error = PrettyHttpError(tr, OBFW("55Sz6K+35LiK5Lyg5Yet6K+B5aSx6LSl"));
        return out;
    }

    auto tj = json::Parse(tr.body);
    if (!tj || !tj->IsObject()) {
        out.error = OBFW("5pyN5Yqh5Zmo6L+U5Zue55qE5pWw5o2u5peg5rOV6Kej5p6Q77yM6K+356iN5ZCO5YaN6K+V");
        return out;
    }

    const std::string uploadToken = tj->GetStr("upload_token");
    const std::string objectKey   = tj->GetStr("key");
    const std::string uploadHost  = tj->GetStr("upload_host", OBFA("aHR0cHM6Ly91cC16Mi5xaW5pdXAuY29t"));

    if (uploadToken.empty() || objectKey.empty()) {
        const std::string msg = tj->GetStr("message", "");
        out.error = OBFW("5pyN5Yqh5Zmo5pyq6L+U5Zue5pyJ5pWI55qE5LiK5Lyg5Yet6K+B") +
                    (msg.empty() ? std::wstring() : (OBFW("77ya") + util::Utf8ToWide(msg)));
        return out;
    }

    out.objectKey = util::Utf8ToWide(objectKey);
    L(OBFW("5Yet6K+B6I635Y+W5oiQ5Yqf77yM55uu5qCH5a+56LGh77ya") + out.objectKey);

    // ---------------- 5. 直传七牛 ----------------
    P(580, OBFW("5q2j5Zyo5LiK5Lyg"));
    L(OBFW("5q2j5Zyo5LiK5Lyg5Yiw5a+56LGh5a2Y5YKo77ya") + util::Utf8ToWide(uploadHost));

    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back(OBFA("dG9rZW4="), uploadToken);
    fields.emplace_back("key", objectKey);
    fields.emplace_back(OBFA("eDptYWNoaW5l"), util::WideToUtf8(machine));

    auto upProgress = [&](unsigned long long sent, unsigned long long total) -> bool {
        if (Canceled()) return false;
        int permille = 580;
        if (total > 0) {
            // 上传阶段占 580‰ ~ 960‰
            permille = 580 + (int)((sent * 380ull) / total);
            if (permille > 960) permille = 960;
        }
        wchar_t buf[128]{};
        ::swprintf(buf, 128, OBFW("5LiK5Lyg5LitICVzIC8gJXM="),
                   util::FormatSize(sent).c_str(), util::FormatSize(total).c_str());
        P(permille, buf);
        return true;
    };

    http::Response ur = http::UploadMultipartFile(
        util::Utf8ToWide(uploadHost),
        fields,
        OBFA("ZmlsZQ=="),
        util::WideToUtf8(zipName),
        zipPath,
        {},                 // 七牛直传不需要额外头
        MakeTimeouts(),
        upProgress,
        cancel);

    if (Canceled() || ur.error == OBFW("5bey5Y+W5raI")) { out.canceled = true; out.error = OBFW("5bey5Y+W5raI"); return out; }

    if (!ur.ok || !ur.Is2xx()) {
        out.error = PrettyHttpError(ur, OBFW("5LiK5Lyg5Yiw5a+56LGh5a2Y5YKo5aSx6LSl"));
        return out;
    }

    {
        auto uj = json::Parse(ur.body);
        if (uj && uj->IsObject()) {
            const std::string k = uj->GetStr("key");
            if (!k.empty()) out.objectKey = util::Utf8ToWide(k);
        }
    }
    L(OBFW("5LiK5Lyg5a6M5oiQ"));

    // ---------------- 6. 回报后端登记 ----------------
    P(970, OBFW("5q2j5Zyo55m76K6w"));

    std::string repJson;
    {
        repJson  = OBFA("ew==");
        repJson += OBFA("ImtleSI6Ig==")       + json::EscapeString(util::WideToUtf8(out.objectKey)) + OBFA("Iiw=");
        repJson += OBFA("InBhc3N3b3JkIjoi")  + json::EscapeString(pw) + OBFA("Iiw=");
        repJson += OBFA("Im1hY2hpbmUiOiI=")   + json::EscapeString(util::WideToUtf8(machine)) + OBFA("Iiw=");
        repJson += OBFA("InNpemUiOg==")        + std::to_string(pack.zipBytes) + OBFA("LA==");
        repJson += OBFA("InJhd19zaXplIjo=")    + std::to_string(pack.rawBytes) + OBFA("LA==");
        repJson += OBFA("ImZpbGVfY291bnQiOg==")  + std::to_string(pack.fileCount) + OBFA("LA==");
        repJson += OBFA("InNvdXJjZV9kaXIiOiI=") + json::EscapeString(util::WideToUtf8(savesDir)) + OBFA("Iiw=");
        repJson += OBFA("ImlwIjoi")        + json::EscapeString(util::WideToUtf8(util::GetMachineIp())) + OBFA("Ig==");
        repJson += OBFA("fQ==");
    }

    // 发送前自检（防手拼 JSON 漏逗号类 bug）
    if (!JsonOk(repJson)) {
        out.error = OBFW("5a6i5oi356uv5YaF6YOo6ZSZ6K+v77ya55Sf5oiQ55qE6K+35rGC5L2T5LiN5ZCI5rOV77yM6K+36YeN6K+V");
        return out;
    }

    const std::wstring reportUrl = config::BackendBaseUrl + OBFW("L2FwaS9yZXBvcnQ=");
    http::Response rr = http::PostJson(reportUrl, repJson, AuthHeaders(), MakeTimeouts());

    if (rr.ok && rr.Is2xx()) {
        auto rj = json::Parse(rr.body);
        if (rj && rj->IsObject()) {
            out.downloadUrl = util::Utf8ToWide(rj->GetStr("download_url"));
            const long long expires = rj->GetInt("expires_in", 0);
            if (expires > 0) {
                wchar_t buf[96]{};
                ::swprintf(buf, 96, OBFW("5LiL6L296ZO+5o6l5pyJ5pWI5pyf57qmICVsbGQg5bCP5pe2"), expires / 3600);
                out.expireText = buf;
            }
        }
        L(OBFW("5bey5Zyo5pyN5Yqh5Zmo55m76K6w77yM5a+G56CB5bey5ZCM5q2l5L+d5a2Y"));
    } else {
        // 登记失败不算致命错误——文件已经上传成功，密码在界面上也拿得到
        L(OBFW("5o+Q56S677ya5pyN5Yqh5Zmo55m76K6w5aSx6LSl77yI") + PrettyHttpError(rr, L"") + OBFW("77yJ77yM5L2G5paH5Lu25bey5LiK5Lyg5oiQ5Yqf77yM6K+35Yqh5b+F6Ieq6KGM5L+d5a2Y5LiL5pa55a+G56CB"));
    }

    P(1000, OBFW("5a6M5oiQ"));
    out.ok = true;
    return out;
}

// ===========================================================================
Outcome Download(const std::wstring& savesDir,
                 const std::wstring& password,
                 const LogFn& log,
                 const ProgressFn& progress,
                 const std::atomic<bool>* cancel) {
    Outcome out;
    out.isDownload = true;

    auto L = [&](const std::wstring& s) { if (log) log(s); };
    auto P = [&](int permille, const std::wstring& stage) { if (progress) progress(permille, stage); };
    auto Canceled = [&]() { return cancel && cancel->load(); };

    // ---------------- 0. 前置校验 ----------------
    P(0, OBFW("5qOA5p+l55uu5b2V"));
    if (savesDir.empty() || !util::DirectoryExists(savesDir)) {
        out.error = OBFW("55uu5b2V5LiN5a2Y5Zyo77yM6K+35YWI6YCJ5oup6KaB6Kej5Y6L5Yiw55qEIHNhdmVzIOebruW9lQ==");
        return out;
    }
    if (!lolfind::HasMarker(savesDir)) {
        out.error = L"该目录里没有找到 " MARKER_FILE_W L"，不是有效的 saves 目录，无法解压";
        return out;
    }
    if (!util::IsValidPassword(password)) {
        out.error = OBFW("5a+G56CB5b+F6aG75pivIDQtMjQg5L2N5a2X5q+N5oiW5pWw5a2X");
        return out;
    }
    const std::string pw = util::WideToUtf8(password);
    out.password = password;
    L(OBFW("55uu5qCH55uu5b2V77ya") + savesDir);

    // ---------------- 1. 换取下载链接 ----------------
    P(5, OBFW("5q2j5Zyo5o2i5Y+W5LiL6L296ZO+5o6l"));
    L(OBFW("5q2j5Zyo5ZCR5pyN5Yqh5Zmo5p+l6K+i6K+l5a+G56CB5a+55bqU55qE5a2Y5qGjLi4u"));
    const std::string req = OBFA("eyJwYXNzd29yZCI6Ig==") + json::EscapeString(pw) + OBFA("In0=");
    const std::wstring dlUrl = config::BackendBaseUrl + OBFW("L2FwaS9kb3dubG9hZA==");
    http::Response dr = http::PostJson(dlUrl, req, AuthHeaders(), MakeTimeouts());
    if (!dr.ok || !dr.Is2xx()) {
        out.error = PrettyHttpError(dr, OBFW("5p+l6K+i5LiL6L296ZO+5o6l5aSx6LSl"));
        return out;
    }
    auto dj = json::Parse(dr.body);
    if (!dj || !dj->IsObject()) {
        out.error = OBFW("5pyN5Yqh5Zmo6L+U5Zue55qE5pWw5o2u5peg5rOV6Kej5p6Q");
        return out;
    }
    const std::string downloadUrl = dj->GetStr("download_url");
    const std::string key = dj->GetStr("key");
    if (downloadUrl.empty() || key.empty()) {
        out.error = OBFW("5pyN5Yqh5Zmo5pyq6L+U5Zue5pyJ5pWI55qE5LiL6L295L+h5oGv");
        return out;
    }
    out.objectKey = util::Utf8ToWide(key);
    L(OBFW("5bey5om+5Yiw5a+55bqU5a2Y5qGj77yM5a+56LGh77ya") + out.objectKey);

    // ---------------- 2. 下载到临时文件 ----------------
    P(20, OBFW("5q2j5Zyo5LiL6L29"));
    const std::wstring stamp  = util::TimestampCompact();
    const std::wstring machine = util::MachineId();
    const std::wstring tmpName = OBFW("aGFuYm90X2RsXw==") + machine + OBFW("Xw==") + stamp + OBFW("LnppcA==");
    const std::wstring tmpPath = util::JoinPath(util::GetTempDir(), tmpName);
    ScopedTempFile tmp(tmpPath);

    auto dlProgress = [&](unsigned long long received, unsigned long long total) -> bool {
        if (Canceled()) return false;
        int permille = 20;
        if (total > 0) {
            permille = 20 + (int)((received * 880ull) / total);
            if (permille > 900) permille = 900;
        }
        wchar_t buf[128]{};
        ::swprintf(buf, 128, OBFW("5LiL6L295LitICVzIC8gJXM="),
                   util::FormatSize(received).c_str(), util::FormatSize(total).c_str());
        P(permille, buf);
        return true;
    };

    http::Response fr = http::DownloadToFile(util::Utf8ToWide(downloadUrl), tmpPath,
                                             {}, MakeTimeouts(), dlProgress, cancel);
    if (Canceled() || fr.error == OBFW("5bey5Y+W5raI")) { out.canceled = true; out.error = OBFW("5bey5Y+W5raI"); return out; }
    if (!fr.ok) {
        out.error = (fr.status == 403)
            ? OBFW("5LiL6L296KKr5ouS57ud77yIQ0ROIOmYsuebl+mTvuagoemqjOWksei0pe+8jOivt+ehruiupOWuouaIt+err+eJiOacrOaIluiBlOezu+euoeeQhuWRmO+8iQ==")
            : PrettyHttpError(fr, OBFW("5LiL6L295aSx6LSl"));
        return out;
    }

    // 记录下载字节数（读取临时文件大小）
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (::GetFileAttributesExW(tmpPath.c_str(), GetFileExInfoStandard, &fad)) {
            ULARGE_INTEGER s{}; s.LowPart = fad.nFileSizeLow; s.HighPart = fad.nFileSizeHigh;
            out.downloadedBytes = s.QuadPart;
        }
    }
    L(OBFW("5LiL6L295a6M5oiQ77yI") + util::FormatSize(out.downloadedBytes) + OBFW("77yJ77yM5byA5aeL6Kej5a+G6Kej5Y6LLi4u"));

    // ---------------- 3. 解密解压到 savesDir（覆盖）----------------
    P(910, OBFW("5q2j5Zyo6Kej5Y6L"));
    zipr::ExtractResult ex = zipr::ExtractEncryptedZip(tmpPath, savesDir, pw);
    if (ex.passwordWrong) {
        out.passwordWrong = true;
        out.error = OBFW("5a+G56CB5LiN5q2j56Gu77yM5peg5rOV6Kej5a+G6K+l5Y6L57yp5YyF77yI6K+356Gu6K6k5a+G56CB5piv5ZCm5q2j56Gu77yJ");
        return out;
    }
    if (!ex.ok) {
        out.error = ex.error.empty() ? OBFW("6Kej5Y6L5aSx6LSl") : ex.error;
        return out;
    }
    out.extractedFiles = ex.fileCount;
    out.rawBytes = ex.writtenBytes;
    L(OBFW("6Kej5Y6L5a6M5oiQ77yM5YWx5oGi5aSNIA==") + std::to_wstring(ex.fileCount) + OBFW("IOS4quaWh+S7tu+8jOW3suimhuebluiHs+ebruagh+ebruW9lQ=="));

    P(1000, OBFW("5a6M5oiQ"));
    out.ok = true;
    return out;
}

// ===========================================================================
// 后端连通性检测：调用 GET /api/health
//   仅做轻量探测，不改任何状态；结果用于启动时的连接提示。
// ===========================================================================
HealthResult CheckBackend(const LogFn& log) {
    HealthResult res;
    const std::wstring url = config::BackendBaseUrl + OBFW("L2FwaS9oZWFsdGg=");

    // 注意：日志面板对用户可见，这里刻意不打印后端地址，避免暴露服务端 IP / 端口。
    if (log) log(OBFW("5q2j5Zyo5qOA5rWL5pyN5Yqh5Zmo6L+e5o6lLi4u"));

    http::Response r = http::Get(url, AuthHeaders(), MakeTimeouts());

    // 网络层失败，或收到非 2xx 响应（例如 CF 隧道活着但后端进程挂了 →
    // cloudflared 返回 502 错误页），一律按「服务器连接失败」处理。
    // 不能把 502 之类算成「版本不匹配」，那会误导用户去升级客户端。
    if (!r.ok || !r.Is2xx()) {
        res.reachable = false;
        // 只给一句结论，不带地址、不带状态码——那些对用户没意义，还会泄露后端地址。
        res.message = OBFW("5pyN5Yqh5Zmo6L+e5o6l5aSx6LSl");
        if (log) log(res.message);
        return res;
    }

    res.reachable = true;
    auto j = json::Parse(r.body);
    if (j && j->IsObject()) {
        const bool ok         = j->GetBool("ok", false);
        const bool configured = j->GetBool("configured", false);
        // service 字段只用于内部判断，不再回显到界面（属于服务端身份信息）。
        if (ok && configured) {
            res.ok = true;
            res.message = OBFW("5pyN5Yqh5Zmo6L+e5o6l5oiQ5Yqf");
        } else if (ok && !configured) {
            res.ok = false;
            res.message = OBFW("5pyN5Yqh5Zmo5bey6L+e5o6l77yM5L2G5a2Y5YKo5pyq6YWN572u77yM5LiK5Lyg5Lya5aSx6LSl");
        } else {
            res.ok = false;
            res.message = OBFW("5pyN5Yqh5Zmo5bey5ZON5bqU77yM5L2G54q25oCB5byC5bi4");
        }
    } else {
        // 2xx 但响应体不是合法 JSON（几乎不会发生）：说明后端返回了意料之外的内容，
        // 提醒用户稍后重试即可，别再抛「版本不匹配」误导人。
        res.ok = false;
        res.message = OBFW("5pyN5Yqh5Zmo6L+U5Zue5byC5bi45pWw5o2u77yM6K+356iN5ZCO6YeN6K+V");
    }

    if (log) log(res.message);
    return res;
}

// ===========================================================================
// 后端密码占用查询：POST /api/check-password
//   仅用于「随机生成密码」时核对，保证生成出来的密码未被使用过；
//   手动输入密码不再查重（同密码上传 = 覆盖更新原存档）。
// ===========================================================================
bool PasswordExists(const std::wstring& password) {
    if (!util::IsValidPassword(password)) return false;

    const std::string req = OBFA("eyJwYXNzd29yZCI6Ig==") + json::EscapeString(util::WideToUtf8(password)) + OBFA("In0=");
    const std::wstring url = config::BackendBaseUrl + OBFW("L2FwaS9jaGVjay1wYXNzd29yZA==");
    http::Response r = http::PostJson(url, req, AuthHeaders(), MakeTimeouts());
    if (!r.ok || !r.Is2xx()) return false;     // 异常时不拦截，直接用生成的密码

    auto j = json::Parse(r.body);
    if (!j || !j->IsObject()) return false;
    return j->GetBool("exists", false);
}

} // namespace uploader
