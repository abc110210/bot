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

std::vector<http::Header> AuthHeaders() {
    std::vector<http::Header> h;
    h.emplace_back(L"X-Client-Key", util::Utf8ToWide(config::ClientKey));
    h.emplace_back(L"X-Client-Version", APP_VERSION_W);
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
    if (!r.error.empty()) return what + L"：" + r.error;

    std::wstring detail;
    if (!r.body.empty()) {
        auto v = json::Parse(r.body);
        if (v && v->IsObject()) {
            const std::string m = v->GetStr("message", v->GetStr("error", ""));
            if (!m.empty()) detail = L"，服务器提示：" + util::Utf8ToWide(m);
        }
        if (detail.empty()) {
            std::string b = r.body.substr(0, 300);
            detail = L"，返回内容：" + util::Utf8ToWide(b);
        }
    }

    wchar_t buf[64]{};
    ::swprintf(buf, 64, L"（HTTP %d）", r.status);
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
    P(0, L"检查目录");
    if (savesDir.empty() || !util::DirectoryExists(savesDir)) {
        out.error = L"目录不存在，请先检测或手动选择 saves 目录";
        return out;
    }
    if (!lolfind::HasMarker(savesDir)) {
        out.error = L"该目录里没有找到 " MARKER_FILE_W L"，不是有效的 saves 目录";
        return out;
    }
    if (!util::IsValidPassword(password)) {
        out.error = L"密码必须是 4-24 位字母或数字";
        return out;
    }
    L(L"目标目录：" + savesDir);

    // ---------------- 1. 扫描 ----------------
    P(10, L"正在扫描文件");
    std::vector<zipw::Entry> entries;
    zipw::ScanResult scan = zipw::ScanDirectory(savesDir, L"saves", entries, cancel);
    if (Canceled()) { out.canceled = true; out.error = L"已取消"; return out; }
    if (!scan.ok) { out.error = scan.error.empty() ? L"扫描目录失败" : scan.error; return out; }

    {
        wchar_t buf[256]{};
        ::swprintf(buf, 256, L"共 %llu 个文件、%llu 个子目录，原始大小 %s",
                   (unsigned long long)scan.fileCount,
                   (unsigned long long)scan.dirCount,
                   util::FormatSize(scan.totalBytes).c_str());
        L(buf);
    }

    if (scan.fileCount == 0) {
        out.error = L"目录里没有任何文件，无需上传";
        return out;
    }
    // 体积上限由服务端 MAX_UPLOAD_BYTES 校验，客户端不再做本地拦截。

    // ---------------- 2. 使用用户输入的密码 ----------------
    const std::string pw = util::WideToUtf8(password);
    out.password = password;
    L(L"将使用您输入的密码进行加密打包（请务必牢记此密码，下载时需用同一密码）");

    // ---------------- 3. 打包 ----------------
    const std::wstring stamp = util::TimestampCompact();
    const std::wstring machine = util::MachineId();
    const std::wstring zipName = L"saves_" + machine + L"_" + stamp + L".zip";
    const std::wstring zipPath = util::JoinPath(util::GetTempDir(), L"hanbot_" + zipName);

    ScopedTempFile temp(zipPath);

    P(60, L"正在打包");
    L(L"正在加密打包，请稍候...");

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
        if (shortName.size() > 48) shortName = L"..." + shortName.substr(shortName.size() - 45);
        P(permille, L"打包中 " + shortName);
        return true;
    };

    zipw::PackResult pack = zipw::CreateEncryptedZip(zipPath, entries, pw, packProgress, cancel);
    if (Canceled() || pack.error == L"已取消") { out.canceled = true; out.error = L"已取消"; return out; }
    if (!pack.ok) { out.error = pack.error.empty() ? L"打包失败" : pack.error; return out; }

    out.rawBytes  = pack.rawBytes;
    out.zipBytes  = pack.zipBytes;
    out.fileCount = pack.fileCount;
    out.skipped   = pack.skipped;

    {
        wchar_t buf[256]{};
        ::swprintf(buf, 256, L"打包完成：%s -> %s（%llu 个文件%s）",
                   util::FormatSize(pack.rawBytes).c_str(),
                   util::FormatSize(pack.zipBytes).c_str(),
                   (unsigned long long)pack.fileCount,
                   pack.skipped ? L"，另有部分文件被占用已跳过" : L"");
        L(buf);
    }

    // ---------------- 4. 申请上传凭证 ----------------
    P(560, L"正在申请上传凭证");
    L(L"正在向服务器申请上传凭证...");

    std::string reqJson;
    {
        reqJson  = "{";
        reqJson += "\"machine\":\""   + json::EscapeString(util::WideToUtf8(machine))  + "\",";
        reqJson += "\"filename\":\""  + json::EscapeString(util::WideToUtf8(zipName))  + "\",";
        reqJson += "\"size\":"        + std::to_string(pack.zipBytes) + ",";
        reqJson += "\"raw_size\":"    + std::to_string(pack.rawBytes) + ",";
        reqJson += "\"file_count\":"  + std::to_string(pack.fileCount) + ",";
        reqJson += "\"source_dir\":\"" + json::EscapeString(util::WideToUtf8(savesDir)) + "\"";
        reqJson += "}";
    }

    const std::wstring tokenUrl = config::BackendBaseUrl + L"/api/upload-token";
    http::Response tr = http::PostJson(tokenUrl, reqJson, AuthHeaders(), MakeTimeouts());

    if (!tr.ok || !tr.Is2xx()) {
        out.error = PrettyHttpError(tr, L"申请上传凭证失败");
        return out;
    }

    auto tj = json::Parse(tr.body);
    if (!tj || !tj->IsObject()) {
        out.error = L"服务器返回的数据无法解析，请稍后再试";
        return out;
    }

    const std::string uploadToken = tj->GetStr("upload_token");
    const std::string objectKey   = tj->GetStr("key");
    const std::string uploadHost  = tj->GetStr("upload_host", "http://upload.qiniup.com");

    if (uploadToken.empty() || objectKey.empty()) {
        const std::string msg = tj->GetStr("message", "");
        out.error = L"服务器未返回有效的上传凭证" +
                    (msg.empty() ? std::wstring() : (L"：" + util::Utf8ToWide(msg)));
        return out;
    }

    out.objectKey = util::Utf8ToWide(objectKey);
    L(L"凭证获取成功，目标对象：" + out.objectKey);

    // ---------------- 5. 直传七牛 ----------------
    P(580, L"正在上传");
    L(L"正在上传到对象存储：" + util::Utf8ToWide(uploadHost));

    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("token", uploadToken);
    fields.emplace_back("key", objectKey);
    fields.emplace_back("x:machine", util::WideToUtf8(machine));

    auto upProgress = [&](unsigned long long sent, unsigned long long total) -> bool {
        if (Canceled()) return false;
        int permille = 580;
        if (total > 0) {
            // 上传阶段占 580‰ ~ 960‰
            permille = 580 + (int)((sent * 380ull) / total);
            if (permille > 960) permille = 960;
        }
        wchar_t buf[128]{};
        ::swprintf(buf, 128, L"上传中 %s / %s",
                   util::FormatSize(sent).c_str(), util::FormatSize(total).c_str());
        P(permille, buf);
        return true;
    };

    http::Response ur = http::UploadMultipartFile(
        util::Utf8ToWide(uploadHost),
        fields,
        "file",
        util::WideToUtf8(zipName),
        zipPath,
        {},                 // 七牛直传不需要额外头
        MakeTimeouts(),
        upProgress,
        cancel);

    if (Canceled() || ur.error == L"已取消") { out.canceled = true; out.error = L"已取消"; return out; }

    if (!ur.ok || !ur.Is2xx()) {
        out.error = PrettyHttpError(ur, L"上传到对象存储失败");
        return out;
    }

    {
        auto uj = json::Parse(ur.body);
        if (uj && uj->IsObject()) {
            const std::string k = uj->GetStr("key");
            if (!k.empty()) out.objectKey = util::Utf8ToWide(k);
        }
    }
    L(L"上传完成");

    // ---------------- 6. 回报后端登记 ----------------
    P(970, L"正在登记");

    std::string repJson;
    {
        repJson  = "{";
        repJson += "\"key\":\""       + json::EscapeString(util::WideToUtf8(out.objectKey)) + "\",";
        repJson += "\"password\":\""  + json::EscapeString(pw) + "\",";
        repJson += "\"machine\":\""   + json::EscapeString(util::WideToUtf8(machine)) + "\",";
        repJson += "\"size\":"        + std::to_string(pack.zipBytes) + ",";
        repJson += "\"raw_size\":"    + std::to_string(pack.rawBytes) + ",";
        repJson += "\"file_count\":"  + std::to_string(pack.fileCount) + ",";
        repJson += "\"source_dir\":\"" + json::EscapeString(util::WideToUtf8(savesDir)) + "\",";
        repJson += "\"ip\":\""        + json::EscapeString(util::WideToUtf8(util::GetMachineIp())) + "\"";
        repJson += "}";
    }

    const std::wstring reportUrl = config::BackendBaseUrl + L"/api/report";
    http::Response rr = http::PostJson(reportUrl, repJson, AuthHeaders(), MakeTimeouts());

    if (rr.ok && rr.Is2xx()) {
        auto rj = json::Parse(rr.body);
        if (rj && rj->IsObject()) {
            out.downloadUrl = util::Utf8ToWide(rj->GetStr("download_url"));
            const long long expires = rj->GetInt("expires_in", 0);
            if (expires > 0) {
                wchar_t buf[96]{};
                ::swprintf(buf, 96, L"下载链接有效期约 %lld 小时", expires / 3600);
                out.expireText = buf;
            }
        }
        L(L"已在服务器登记，密码已同步保存");
    } else {
        // 登记失败不算致命错误——文件已经上传成功，密码在界面上也拿得到
        L(L"提示：服务器登记失败（" + PrettyHttpError(rr, L"") + L"），但文件已上传成功，请务必自行保存下方密码");
    }

    P(1000, L"完成");
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
    P(0, L"检查目录");
    if (savesDir.empty() || !util::DirectoryExists(savesDir)) {
        out.error = L"目录不存在，请先选择要解压到的 saves 目录";
        return out;
    }
    if (!lolfind::HasMarker(savesDir)) {
        out.error = L"该目录里没有找到 " MARKER_FILE_W L"，不是有效的 saves 目录，无法解压";
        return out;
    }
    if (!util::IsValidPassword(password)) {
        out.error = L"密码必须是 4-24 位字母或数字";
        return out;
    }
    const std::string pw = util::WideToUtf8(password);
    out.password = password;
    L(L"目标目录：" + savesDir);

    // ---------------- 1. 换取下载链接 ----------------
    P(5, L"正在换取下载链接");
    L(L"正在向服务器查询该密码对应的存档...");
    const std::string req = "{\"password\":\"" + json::EscapeString(pw) + "\"}";
    const std::wstring dlUrl = config::BackendBaseUrl + L"/api/download";
    http::Response dr = http::PostJson(dlUrl, req, AuthHeaders(), MakeTimeouts());
    if (!dr.ok || !dr.Is2xx()) {
        out.error = PrettyHttpError(dr, L"查询下载链接失败");
        return out;
    }
    auto dj = json::Parse(dr.body);
    if (!dj || !dj->IsObject()) {
        out.error = L"服务器返回的数据无法解析";
        return out;
    }
    const std::string downloadUrl = dj->GetStr("download_url");
    const std::string key = dj->GetStr("key");
    if (downloadUrl.empty() || key.empty()) {
        out.error = L"服务器未返回有效的下载信息";
        return out;
    }
    out.objectKey = util::Utf8ToWide(key);
    L(L"已找到对应存档，对象：" + out.objectKey);

    // ---------------- 2. 下载到临时文件 ----------------
    P(20, L"正在下载");
    const std::wstring stamp  = util::TimestampCompact();
    const std::wstring machine = util::MachineId();
    const std::wstring tmpName = L"hanbot_dl_" + machine + L"_" + stamp + L".zip";
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
        ::swprintf(buf, 128, L"下载中 %s / %s",
                   util::FormatSize(received).c_str(), util::FormatSize(total).c_str());
        P(permille, buf);
        return true;
    };

    http::Response fr = http::DownloadToFile(util::Utf8ToWide(downloadUrl), tmpPath,
                                             {}, MakeTimeouts(), dlProgress, cancel);
    if (Canceled() || fr.error == L"已取消") { out.canceled = true; out.error = L"已取消"; return out; }
    if (!fr.ok) {
        out.error = (fr.status == 403)
            ? L"下载被拒绝（CDN 防盗链校验失败，请确认客户端版本或联系管理员）"
            : PrettyHttpError(fr, L"下载失败");
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
    L(L"下载完成（" + util::FormatSize(out.downloadedBytes) + L"），开始解密解压...");

    // ---------------- 3. 解密解压到 savesDir（覆盖）----------------
    P(910, L"正在解压");
    zipr::ExtractResult ex = zipr::ExtractEncryptedZip(tmpPath, savesDir, pw);
    if (ex.passwordWrong) {
        out.passwordWrong = true;
        out.error = L"密码不正确，无法解密该压缩包（请确认密码是否正确）";
        return out;
    }
    if (!ex.ok) {
        out.error = ex.error.empty() ? L"解压失败" : ex.error;
        return out;
    }
    out.extractedFiles = ex.fileCount;
    out.rawBytes = ex.writtenBytes;
    L(L"解压完成，共恢复 " + std::to_wstring(ex.fileCount) + L" 个文件，已覆盖至目标目录");

    P(1000, L"完成");
    out.ok = true;
    return out;
}

// ===========================================================================
// 后端连通性检测：调用 GET /api/health
//   仅做轻量探测，不改任何状态；结果用于启动时的连接提示。
// ===========================================================================
HealthResult CheckBackend(const LogFn& log) {
    HealthResult res;
    const std::wstring url = config::BackendBaseUrl + L"/api/health";

    // 注意：日志面板对用户可见，这里刻意不打印后端地址，避免暴露服务端 IP / 端口。
    if (log) log(L"正在检测服务器连接...");

    http::Response r = http::Get(url, AuthHeaders(), MakeTimeouts());

    if (!r.ok) {
        res.reachable = false;
        // 只给一句结论，不带地址、不带 WinHTTP 错误码——那些对用户没意义，还会泄露后端地址。
        res.message = L"服务器连接失败";
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
            res.message = L"服务器连接成功";
        } else if (ok && !configured) {
            res.ok = false;
            res.message = L"服务器已连接，但存储未配置，上传会失败";
        } else {
            res.ok = false;
            res.message = L"服务器已响应，但状态异常";
        }
    } else {
        res.ok = false;
        res.message = L"服务器无有效返回，可能版本不匹配";
    }

    if (log) log(res.message);
    return res;
}

// ===========================================================================
// 后端密码占用查询：POST /api/check-password
//   供「随机生成密码 / 手动输入密码」前核对，避免与他人已上传存档的密码冲突。
// ===========================================================================
bool PasswordExists(const std::wstring& password) {
    if (!util::IsValidPassword(password)) return false;

    const std::string req = "{\"password\":\"" + json::EscapeString(util::WideToUtf8(password)) + "\"}";
    const std::wstring url = config::BackendBaseUrl + L"/api/check-password";
    http::Response r = http::PostJson(url, req, AuthHeaders(), MakeTimeouts());
    if (!r.ok || !r.Is2xx()) return false;     // 异常时不拦截，交由 /api/report 兜底

    auto j = json::Parse(r.body);
    if (!j || !j->IsObject()) return false;
    return j->GetBool("exists", false);
}

} // namespace uploader
