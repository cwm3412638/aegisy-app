#ifndef TOOL_MANAGER_H
#define TOOL_MANAGER_H

#include <QObject>
#include <QString>
#include <QProcess>

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

    // 同步检测（含 QProcess 探测，最长约 5s）
    ToolStatus detect(AiTool tool);

    // 快速检测：超时缩短为 2s，适合 UI 场景
    ToolStatus detectFast(AiTool tool);

    // 检测桌面应用安装状态（Claude 桌面版 / ChatGPT 桌面版）
    DesktopAppStatus detectDesktop(AiTool tool);

    // 异步安装：npm install -g <pkg>；输出与结果通过信号回传
    void install(AiTool tool);

    // 写入官方格式配置（先备份）。model 为空时使用各工具默认值。失败返回 false，详情见 lastError()
    bool configure(AiTool tool, const QString &apiKey, const QString &model = QString());

    QString lastError() const { return m_lastError; }

    // Node.js 是否可用（供未装 Node 时给引导）
    bool isNodeAvailable();

signals:
    void installOutput(AiTool tool, const QString &line);
    void installFinished(AiTool tool, bool success);

private:
    bool configureClaudeCode(const QString &apiKey, const QString &model);
    bool configureCodexCli(const QString &apiKey, const QString &model);
    bool configureGeminiCli(const QString &apiKey, const QString &model);

    // 探测辅助：timeout 单位 ms
    bool commandExists(const QString &command, int timeoutMs = 5000);
    ToolStatus detectWithTimeout(AiTool tool, int timeoutMs);

    // 文件辅助
    static QString homeFilePath(const QString &relative);
    bool backupFile(const QString &path);
    bool writeTextFile(const QString &path, const QByteArray &data);

    QString m_lastError;
};

#endif // TOOL_MANAGER_H
