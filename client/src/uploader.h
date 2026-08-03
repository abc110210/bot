#pragma once

// ---------------------------------------------------------------------------
// 上传流水线
//   扫描目录 -> 生成随机密码 -> 加密打包 -> 向后端申请上传凭证
//   -> 直传七牛云 -> 回报后端登记 -> 清理临时文件
//
// 关键安全约定：AK / SK 永远只存在于后端，客户端只拿一次性上传凭证。
// ---------------------------------------------------------------------------

#include <string>
#include <functional>
#include <atomic>

namespace uploader {

struct Outcome {
    bool               ok = false;
    bool               canceled = false;
    bool               isDownload = false;     // true=下载解压流程；false=上传打包流程
    std::wstring       error;

    std::wstring       password;       // 压缩包密码（用户输入）
    std::wstring       objectKey;      // 对象存储中的 key
    std::wstring       downloadUrl;    // 可直接使用的下载链接
    std::wstring       expireText;     // 下载链接有效期说明

    unsigned long long zipBytes = 0;
    unsigned long long rawBytes = 0;
    size_t             fileCount = 0;
    size_t             skipped = 0;

    // 下载流程专用
    unsigned long long downloadedBytes = 0;
    size_t             extractedFiles = 0;
    bool               passwordWrong = false;  // 下载时密码错误（无法解密）
};

using LogFn      = std::function<void(const std::wstring&)>;
// stage: 阶段文字；permille: 0-1000
using ProgressFn = std::function<void(int permille, const std::wstring& stage)>;

// 上传：用用户输入的密码把 savesDir 打包加密后上传
Outcome Run(const std::wstring& savesDir,
            const std::wstring& password,
            const LogFn& log,
            const ProgressFn& progress,
            const std::atomic<bool>* cancel);

// 下载：用密码向后端换取下载链接，下载后解密解压到 savesDir 并覆盖
Outcome Download(const std::wstring& savesDir,
                 const std::wstring& password,
                 const LogFn& log,
                 const ProgressFn& progress,
                 const std::atomic<bool>* cancel);

// 后端连通性检测：调用 GET /api/health
struct HealthResult {
    bool         reachable = false;   // 网络是否连通（收到 HTTP 响应）
    bool         ok = false;          // 业务状态是否正常（ok && configured）
    std::wstring message;             // 给用户的提示文案
};

HealthResult CheckBackend(const LogFn& log);

// 检查某密码是否已被后端占用（用于「随机生成密码」或「手动输入密码」前核对，
// 确保每份存档对应一个独立密码，避免按密码取回时拿到错误存档）。
// 网络/鉴权异常时返回 false（不拦截），最终由 /api/report 的密码唯一校验兜底。
bool PasswordExists(const std::wstring& password);

} // namespace uploader
