#ifndef TOOL_MANAGER_H
#define TOOL_MANAGER_H

#include <QObject>
#include <QString>
#include <QProcess>
#include <QDateTime>
#include <QProcessEnvironment>

// 支持的三个官方接入工具
enum class AiTool {
    ClaudeCode,   // Claude Code   -> anthropic 分组
    CodexCli,     // Codex CLI     -> openai 分组
    GeminiCli     // Gemini CLI    -> gemini 分组
};

// 工具检测结果
struct ToolStatus {
    bool nodeOk = false;         // Node.js / npm 可用
    bool installed = false;      // CLI 已安装
    QString version;             // 本地安装版本，如 0.144.0
    QString latestVersion;       // npm registry 最新版本
    bool updateAvailable = false;
    bool configured = false;     // 已接入 aegisy（配置文件指向 aegisy.cc）
    QString configuredKey;       // 已配置的 key（掩码显示用）
    QString conflictWarning;     // 冲突警告（如 ANTHROPIC_API_KEY 环境变量）
};

// 桌面应用检测结果
struct DesktopAppStatus {
    bool    installed   = false;   // 是否已安装
    QString downloadUrl;           // 下载链接
    QString appName;               // 应用名称
};

struct ConfigBackup {
    QString id;
    AiTool tool = AiTool::CodexCli;
    QDateTime createdAt;
    int fileCount = 0;
};

struct RuntimeStatus {
    QString id;
    QString category;
    QString name;
    QString command;
    bool installed = false;
    bool required = false;
    QString version;
    QString executablePath;
};

struct ConfigurationPreview {
    QStringList files;
    QStringList changes;
    QStringList warnings;
};

// 三件套的检测 / 一键安装 / 配置写入。
// 所有写入均为「读-合并-写」并先备份，不整文件覆盖。
class ToolManager : public QObject
{
    Q_OBJECT

public:
    explicit ToolManager(QObject *parent = nullptr);

    // 工具元信息
    static QString toolName(AiTool tool);        // "Claude Code" 等
    static QString toolPlatform(AiTool tool);    // "anthropic" | "openai" | "gemini"
    static QString npmPackage(AiTool tool);      // npm 全局包名
    static QString cliCommand(AiTool tool);      // 可执行名 claude / codex / gemini
    static QString configFilePath(AiTool tool);  // UI 展示用的主认证文件路径

    // 同步检测（含 QProcess 探测，最长约 5s）
    ToolStatus detect(AiTool tool);

    // 快速检测：超时缩短为 2s，适合 UI 场景
    ToolStatus detectFast(AiTool tool);

    // 异步读取本地 CLI 版本，适合主界面展示
    void detectVersion(AiTool tool);
    void checkLatestVersion(AiTool tool);
    QString latestVersion(AiTool tool, int timeoutMs = 10000) const;

    // 检测桌面应用安装状态（Claude 桌面版 / ChatGPT 桌面版）
    DesktopAppStatus detectDesktop(AiTool tool);

    // 异步安装完整 CLI 环境。缺少 Node.js 时先调用系统包管理器安装 Node.js，
    // 然后执行 npm install -g <pkg>；输出与结果通过信号回传。
    void install(AiTool tool, int requestId = 0);

    // 写入官方格式配置（先备份）。model 为空时使用各工具默认值。失败返回 false，详情见 lastError()
    bool configure(AiTool tool, const QString &apiKey, const QString &model = QString());
    bool configureGateway(AiTool tool, const QString &localToken,
                          const QString &model = QString(), int port = 43112);
    ConfigurationPreview previewConfiguration(AiTool tool,
                                              const QString &model = QString(),
                                              bool gatewayMode = false);

    QList<ConfigBackup> backupHistory(AiTool tool) const;
    bool restoreBackup(const QString &backupId, AiTool tool);

    QString lastError() const { return m_lastError; }

    // Node.js 是否可用（供未装 Node 时给引导）
    bool isNodeAvailable();

    // 系统体检使用的基础运行环境检测。
    QList<RuntimeStatus> detectRuntimes(int timeoutMs = 1500) const;
    QList<RuntimeStatus> detectCompanionTools(int timeoutMs = 1500) const;

    // 在系统终端中启动对应 CLI，workingDirectory 为空时使用用户主目录。
    bool launch(AiTool tool, const QString &workingDirectory = QString());
    QString resolvedExecutable(AiTool tool, int timeoutMs = 1500) const;
    QString resolvedRuntimeCommand(const QString &command, int timeoutMs = 1500) const;
    QProcessEnvironment launchEnvironment(AiTool tool) const;

signals:
    void installOutput(AiTool tool, const QString &line);
    void installFinished(AiTool tool, int requestId, bool success);
    void toolVersionDetected(AiTool tool, bool installed, const QString &version);
    void toolLatestVersionDetected(AiTool tool, bool success,
                                   const QString &latestVersion,
                                   const QString &error);

private:
    bool configureClaudeCode(const QString &apiKey, const QString &model);
    bool configureCodexCli(const QString &apiKey, const QString &model);
    bool configureGeminiCli(const QString &apiKey, const QString &model);
    bool configureClaudeCodeEndpoint(const QString &apiKey, const QString &baseUrl);
    bool configureCodexCliEndpoint(const QString &apiKey, const QString &model,
                                   const QString &baseUrl, const QString &providerId);
    bool configureGeminiCliEndpoint(const QString &apiKey, const QString &model,
                                    const QString &baseUrl);
    QString readConfiguredKey(AiTool tool) const;

    // 探测辅助：timeout 单位 ms
    bool commandExists(const QString &command, int timeoutMs = 5000);
    QString resolveCommand(const QString &command, int timeoutMs = 5000) const;
    QString commandVersion(const QString &executable, int timeoutMs) const;
    QString npmPackageVersion(AiTool tool, int timeoutMs) const;
    void detectNpmVersion(AiTool tool);
    void installCli(AiTool tool, int requestId);
    ToolStatus detectWithTimeout(AiTool tool, int timeoutMs);

    // 文件辅助
    static QString homeFilePath(const QString &relative);
    bool writeTextFile(const QString &path, const QByteArray &data);
    QStringList managedConfigPaths(AiTool tool) const;
    QString createBackup(AiTool tool);
    bool restoreBackupInternal(const QString &backupId, AiTool tool);
    void pruneBackups(AiTool tool);

    QString m_lastError;
};

#endif // TOOL_MANAGER_H
