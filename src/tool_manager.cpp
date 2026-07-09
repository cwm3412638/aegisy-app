#include "tool_manager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QDebug>

// 官方指南规定：BASE_URL 一律裸域名，不带 /v1
static const QString kBaseUrl = "https://aegisy.cc";

#ifdef Q_OS_WIN
static const QString kNpmCmd = "npm.cmd";
static const QString kWhichCmd = "where";
#else
static const QString kNpmCmd = "npm";
static const QString kWhichCmd = "which";
#endif

ToolManager::ToolManager(QObject *parent)
    : QObject(parent)
{
}

QString ToolManager::toolName(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("Claude Code");
    case AiTool::CodexCli:   return QStringLiteral("Codex CLI");
    case AiTool::GeminiCli:  return QStringLiteral("Gemini CLI");
    }
    return QString();
}

QString ToolManager::toolPlatform(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("anthropic");
    case AiTool::CodexCli:   return QStringLiteral("openai");
    case AiTool::GeminiCli:  return QStringLiteral("gemini");
    }
    return QString();
}

QString ToolManager::npmPackage(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("@anthropic-ai/claude-code");
    case AiTool::CodexCli:   return QStringLiteral("@openai/codex");
    case AiTool::GeminiCli:  return QStringLiteral("@google/gemini-cli");
    }
    return QString();
}

QString ToolManager::cliCommand(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("claude");
    case AiTool::CodexCli:   return QStringLiteral("codex");
    case AiTool::GeminiCli:  return QStringLiteral("gemini");
    }
    return QString();
}

bool ToolManager::commandExists(const QString &command)
{
    QProcess process;
    process.start(kWhichCmd, QStringList() << command);
    if (!process.waitForFinished(5000)) {
        process.kill();
        return false;
    }
    return process.exitCode() == 0;
}

bool ToolManager::isNodeAvailable()
{
    return commandExists("node");
}

QString ToolManager::homeFilePath(const QString &relative)
{
    return QDir::homePath() + "/" + relative;
}

bool ToolManager::backupFile(const QString &path)
{
    QFile file(path);
    if (!file.exists()) {
        return true;  // 无需备份
    }
    const QString backupPath = path + ".bak." +
        QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    return QFile::copy(path, backupPath);
}

bool ToolManager::writeTextFile(const QString &path, const QByteArray &data)
{
    QFileInfo info(path);
    QDir dir = info.dir();
    if (!dir.exists() && !dir.mkpath(".")) {
        m_lastError = QStringLiteral("无法创建目录：%1").arg(dir.path());
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = QStringLiteral("无法写入文件：%1").arg(path);
        return false;
    }
    file.write(data);
    file.close();
    return true;
}

ToolStatus ToolManager::detect(AiTool tool)
{
    ToolStatus status;
    status.nodeOk = isNodeAvailable();
    status.installed = commandExists(cliCommand(tool));

    // 兜底：PATH 里找不到时查 npm 全局包（有些环境 npm bin 不在 PATH）
    if (!status.installed && status.nodeOk) {
        QProcess process;
        process.start(kNpmCmd, QStringList() << "list" << "-g" << npmPackage(tool) << "--depth=0");
        if (process.waitForFinished(5000)) {
            const QString output = QString::fromUtf8(process.readAllStandardOutput());
            status.installed = output.contains(npmPackage(tool)) && !output.contains("(empty)");
        } else {
            process.kill();
        }
    }

    switch (tool) {
    case AiTool::ClaudeCode: {
        // ~/.claude/settings.json 的 env.ANTHROPIC_BASE_URL 指向 aegisy.cc 即视为已接入
        QFile file(homeFilePath(".claude/settings.json"));
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            const QJsonObject env = root["env"].toObject();
            if (env["ANTHROPIC_BASE_URL"].toString().contains("aegisy.cc")) {
                status.configured = true;
                status.configuredKey = env["ANTHROPIC_AUTH_TOKEN"].toString();
            }
        }
        // 官方指南：ANTHROPIC_API_KEY 与 AUTH_TOKEN 并存会 401
        if (!qgetenv("ANTHROPIC_API_KEY").isEmpty()) {
            status.conflictWarning =
                QStringLiteral("检测到系统环境变量 ANTHROPIC_API_KEY，会与接入配置冲突导致 401，请删除该环境变量");
        }
        break;
    }
    case AiTool::CodexCli: {
        QFile toml(homeFilePath(".codex/config.toml"));
        bool baseOk = false;
        if (toml.exists() && toml.open(QIODevice::ReadOnly)) {
            const QString content = QString::fromUtf8(toml.readAll());
            toml.close();
            baseOk = content.contains("aegisy.cc");
        }
        QFile auth(homeFilePath(".codex/auth.json"));
        QString key;
        if (auth.exists() && auth.open(QIODevice::ReadOnly)) {
            key = QJsonDocument::fromJson(auth.readAll()).object()["OPENAI_API_KEY"].toString();
            auth.close();
        }
        status.configured = baseOk && !key.isEmpty();
        if (status.configured) {
            status.configuredKey = key;
        }
        break;
    }
    case AiTool::GeminiCli: {
        QFile envFile(homeFilePath(".gemini/.env"));
        if (envFile.exists() && envFile.open(QIODevice::ReadOnly)) {
            const QString content = QString::fromUtf8(envFile.readAll());
            envFile.close();
            QString key;
            bool baseOk = false;
            for (const QString &line : content.split('\n')) {
                const QString t = line.trimmed();
                if (t.startsWith("GOOGLE_GEMINI_BASE_URL") && t.contains("aegisy.cc")) {
                    baseOk = true;
                } else if (t.startsWith("GEMINI_API_KEY")) {
                    key = t.section('=', 1).trimmed();
                    key.remove('"');
                }
            }
            status.configured = baseOk && !key.isEmpty();
            if (status.configured) {
                status.configuredKey = key;
            }
        }
        break;
    }
    }

    return status;
}

void ToolManager::install(AiTool tool)
{
    QProcess *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, tool]() {
        const QString text = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        if (!text.isEmpty()) {
            emit installOutput(tool, text);
        }
    });

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process, tool](int exitCode, QProcess::ExitStatus exitStatus) {
        process->deleteLater();
        emit installFinished(tool, exitStatus == QProcess::NormalExit && exitCode == 0);
    });

    connect(process, &QProcess::errorOccurred, this, [this, process, tool](QProcess::ProcessError) {
        emit installOutput(tool, QStringLiteral("无法启动 npm：%1").arg(process->errorString()));
        process->deleteLater();
        emit installFinished(tool, false);
    });

    emit installOutput(tool, QStringLiteral("$ %1 install -g %2").arg(kNpmCmd, npmPackage(tool)));
    process->start(kNpmCmd, QStringList() << "install" << "-g" << npmPackage(tool));
}

bool ToolManager::configure(AiTool tool, const QString &apiKey, const QString &model)
{
    m_lastError.clear();
    switch (tool) {
    case AiTool::ClaudeCode: return configureClaudeCode(apiKey, model);
    case AiTool::CodexCli:   return configureCodexCli(apiKey, model);
    case AiTool::GeminiCli:  return configureGeminiCli(apiKey, model);
    }
    return false;
}

bool ToolManager::configureClaudeCode(const QString &apiKey, const QString &/*model*/)
{
    // ~/.claude/settings.json：合并 env 三变量，保留其它字段
    const QString path = homeFilePath(".claude/settings.json");

    QJsonObject root;
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        root = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
    }

    if (!backupFile(path)) {
        m_lastError = QStringLiteral("备份失败：%1").arg(path);
        return false;
    }

    QJsonObject env = root["env"].toObject();
    env["ANTHROPIC_BASE_URL"] = kBaseUrl;
    env["ANTHROPIC_AUTH_TOKEN"] = apiKey;
    env["CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC"] = "1";
    root["env"] = env;

    return writeTextFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool ToolManager::configureCodexCli(const QString &apiKey, const QString &model)
{
    const QString effectiveModel = model.isEmpty() ? QStringLiteral("gpt-4o") : model;
    // 1) ~/.codex/auth.json
    const QString authPath = homeFilePath(".codex/auth.json");
    if (!backupFile(authPath)) {
        m_lastError = QStringLiteral("备份失败：%1").arg(authPath);
        return false;
    }
    QJsonObject auth;
    {
        // 保留 auth.json 其它字段（如 OAuth token）
        QFile f(authPath);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            auth = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
        }
    }
    auth["OPENAI_API_KEY"] = apiKey;
    if (!writeTextFile(authPath, QJsonDocument(auth).toJson(QJsonDocument::Indented))) {
        return false;
    }

    // 2) ~/.codex/config.toml：移除我们管理的旧行/旧段，追加官方指南全量配置
    const QString tomlPath = homeFilePath(".codex/config.toml");
    QString existing;
    {
        QFile f(tomlPath);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            existing = QString::fromUtf8(f.readAll());
            f.close();
        }
    }
    if (!backupFile(tomlPath)) {
        m_lastError = QStringLiteral("备份失败：%1").arg(tomlPath);
        return false;
    }

    // 我们管理的顶层键（旧值删除后重写）
    static const QStringList managedTopKeys = {
        "model_provider", "model", "review_model", "model_reasoning_effort",
        "disable_response_storage", "network_access", "windows_wsl_setup_acknowledged",
    };
    // 我们管理的表段
    static const QStringList managedSections = {
        "[model_providers.OpenAI]", "[model_providers.aegisy]", "[features]",
    };

    QStringList outLines;
    bool skippingSection = false;
    for (const QString &raw : existing.split('\n')) {
        const QString t = raw.trimmed();
        if (t.startsWith('[')) {
            skippingSection = managedSections.contains(t);
            if (skippingSection) continue;
        }
        if (skippingSection) continue;
        bool managed = false;
        for (const QString &k : managedTopKeys) {
            if (t.startsWith(k) && t.mid(k.length()).trimmed().startsWith('=')) {
                managed = true;
                break;
            }
        }
        if (managed) continue;
        outLines.append(raw);
    }
    while (!outLines.isEmpty() && outLines.last().trimmed().isEmpty()) {
        outLines.removeLast();
    }

    QString result = outLines.join('\n');
    if (!result.isEmpty()) {
        result += "\n\n";
    }
    // 与官网「使用总指南」逐字段一致
    result += QStringLiteral(
        "model_provider = \"OpenAI\"\n"
        "model = \"%1\"\n"
        "review_model = \"%1\"\n").arg(effectiveModel);
    result += QStringLiteral(
        "model_reasoning_effort = \"xhigh\"\n"
        "disable_response_storage = true\n"
        "network_access = \"enabled\"\n"
        "windows_wsl_setup_acknowledged = true\n"
        "\n"
        "[model_providers.OpenAI]\n"
        "name = \"OpenAI\"\n"
        "base_url = \"%1\"\n"
        "wire_api = \"responses\"\n"
        "requires_openai_auth = true\n"
        "\n"
        "[features]\n"
        "goals = true\n").arg(kBaseUrl);

    return writeTextFile(tomlPath, result.toUtf8());
}

bool ToolManager::configureGeminiCli(const QString &apiKey, const QString &model)
{
    const QString effectiveModel = model.isEmpty() ? QStringLiteral("gemini-2.5-pro") : model;
    // ~/.gemini/.env：按行合并三个变量，保留其它行
    const QString path = homeFilePath(".gemini/.env");

    QString existing;
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        existing = QString::fromUtf8(file.readAll());
        file.close();
    }

    if (!backupFile(path)) {
        m_lastError = QStringLiteral("备份失败：%1").arg(path);
        return false;
    }

    static const QStringList managedKeys = {
        "GOOGLE_GEMINI_BASE_URL", "GEMINI_API_KEY", "GEMINI_MODEL",
    };

    QStringList outLines;
    for (const QString &raw : existing.split('\n')) {
        const QString t = raw.trimmed();
        bool managed = false;
        for (const QString &k : managedKeys) {
            if (t.startsWith(k)) {
                managed = true;
                break;
            }
        }
        if (!managed) {
            outLines.append(raw);
        }
    }
    while (!outLines.isEmpty() && outLines.last().trimmed().isEmpty()) {
        outLines.removeLast();
    }

    QString result = outLines.join('\n');
    if (!result.isEmpty()) {
        result += "\n";
    }
    result += QStringLiteral(
        "GOOGLE_GEMINI_BASE_URL=\"%1\"\n"
        "GEMINI_API_KEY=\"%2\"\n"
        "GEMINI_MODEL=\"%3\"\n").arg(kBaseUrl, apiKey, effectiveModel);

    return writeTextFile(path, result.toUtf8());
}
