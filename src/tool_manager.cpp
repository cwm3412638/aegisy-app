#include "tool_manager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QDateTime>
#include <QDebug>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

// 官方指南规定：BASE_URL 一律裸域名，不带 /v1
static const QString kBaseUrl = "https://aegisy.cc";
constexpr int kMaxBackupsPerTool = 10;

static QString toolSlug(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("claude");
    case AiTool::CodexCli:   return QStringLiteral("codex");
    case AiTool::GeminiCli:  return QStringLiteral("gemini");
    }
    return QStringLiteral("unknown");
}

static QString backupRootPath(AiTool tool)
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/backups/") + toolSlug(tool);
}

#ifdef Q_OS_WIN
static const QString kNpmCmd  = "npm.cmd";
static const QString kWhichCmd = "where";
#else
static const QString kNpmCmd  = "npm";
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

QString ToolManager::configFilePath(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("~/.claude/settings.json");
    case AiTool::CodexCli:   return QStringLiteral("~/.codex/auth.json");
    case AiTool::GeminiCli:  return QStringLiteral("~/.gemini/.env");
    }
    return QString();
}

bool ToolManager::commandExists(const QString &command, int timeoutMs)
{
    QProcess process;
    process.start(kWhichCmd, QStringList() << command);
    if (!process.waitForFinished(timeoutMs)) {
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

QStringList ToolManager::managedConfigPaths(AiTool tool) const
{
    switch (tool) {
    case AiTool::ClaudeCode:
        return { homeFilePath(QStringLiteral(".claude/settings.json")) };
    case AiTool::CodexCli:
        return {
            homeFilePath(QStringLiteral(".codex/auth.json")),
            homeFilePath(QStringLiteral(".codex/config.toml")),
        };
    case AiTool::GeminiCli:
        return { homeFilePath(QStringLiteral(".gemini/.env")) };
    }
    return {};
}

QString ToolManager::createBackup(AiTool tool)
{
    const QString rootPath = backupRootPath(tool);
    QDir root;
    if (!root.mkpath(rootPath)) {
        m_lastError = QStringLiteral("无法创建备份目录：%1").arg(rootPath);
        return QString();
    }

    const QString id = QStringLiteral("%1_%2")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")),
             QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
    const QString backupPath = rootPath + QLatin1Char('/') + id;
    if (!root.mkpath(backupPath)) {
        m_lastError = QStringLiteral("无法创建备份批次：%1").arg(backupPath);
        return QString();
    }

    QJsonArray files;
    const QStringList paths = managedConfigPaths(tool);
    for (int i = 0; i < paths.size(); ++i) {
        const QString path = paths[i];
        const bool existed = QFileInfo::exists(path);
        const QString payloadName = QStringLiteral("file_%1.bin").arg(i);
        if (existed) {
            QFile source(path);
            if (!source.open(QIODevice::ReadOnly)) {
                QDir(backupPath).removeRecursively();
                m_lastError = QStringLiteral("无法读取待备份配置：%1").arg(path);
                return QString();
            }
            const QByteArray payloadData = source.readAll();
            QSaveFile payload(backupPath + QLatin1Char('/') + payloadName);
            if (!payload.open(QIODevice::WriteOnly)
                    || payload.write(payloadData) != payloadData.size()
                    || !payload.commit()) {
                QDir(backupPath).removeRecursively();
                m_lastError = QStringLiteral("无法保存配置快照：%1").arg(path);
                return QString();
            }
            QFile::setPermissions(
                backupPath + QLatin1Char('/') + payloadName,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }

        QJsonObject entry;
        entry.insert(QStringLiteral("path"), path);
        entry.insert(QStringLiteral("existed"), existed);
        entry.insert(QStringLiteral("payload"), payloadName);
        files.append(entry);
    }

    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"), 1);
    manifest.insert(QStringLiteral("tool"), static_cast<int>(tool));
    manifest.insert(
        QStringLiteral("created_at"),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    manifest.insert(QStringLiteral("files"), files);

    const QString manifestPath = backupPath + QStringLiteral("/manifest.json");
    QSaveFile manifestFile(manifestPath);
    const QByteArray manifestData = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (!manifestFile.open(QIODevice::WriteOnly)
            || manifestFile.write(manifestData) != manifestData.size()
            || !manifestFile.commit()) {
        QDir(backupPath).removeRecursively();
        m_lastError = QStringLiteral("无法写入备份清单：%1").arg(manifestPath);
        return QString();
    }
    QFile::setPermissions(
        manifestPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return id;
}

QList<ConfigBackup> ToolManager::backupHistory(AiTool tool) const
{
    QList<ConfigBackup> result;
    QDir root(backupRootPath(tool));
    const QStringList ids = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &id : ids) {
        QFile manifest(root.filePath(id + QStringLiteral("/manifest.json")));
        if (!manifest.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject object = QJsonDocument::fromJson(manifest.readAll()).object();
        if (object.value(QStringLiteral("tool")).toInt(-1) != static_cast<int>(tool)) {
            continue;
        }
        ConfigBackup backup;
        backup.id = id;
        backup.tool = tool;
        backup.createdAt = QDateTime::fromString(
            object.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs);
        backup.fileCount = object.value(QStringLiteral("files")).toArray().size();
        result.append(backup);
    }
    std::sort(result.begin(), result.end(), [](const ConfigBackup &left, const ConfigBackup &right) {
        return left.createdAt > right.createdAt;
    });
    return result;
}

void ToolManager::pruneBackups(AiTool tool)
{
    const QList<ConfigBackup> history = backupHistory(tool);
    const QString rootPath = backupRootPath(tool);
    for (int i = kMaxBackupsPerTool; i < history.size(); ++i) {
        QDir(rootPath + QLatin1Char('/') + history[i].id).removeRecursively();
    }
}

bool ToolManager::restoreBackupInternal(const QString &backupId, AiTool tool)
{
    const QString backupPath = backupRootPath(tool) + QLatin1Char('/') + backupId;
    QFile manifestFile(backupPath + QStringLiteral("/manifest.json"));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        m_lastError = QStringLiteral("无法读取备份清单。");
        return false;
    }
    const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    if (manifest.value(QStringLiteral("tool")).toInt(-1) != static_cast<int>(tool)) {
        m_lastError = QStringLiteral("备份类型与目标终端不匹配。");
        return false;
    }

    const QJsonArray files = manifest.value(QStringLiteral("files")).toArray();
    for (const QJsonValue &value : files) {
        const QJsonObject entry = value.toObject();
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (path.isEmpty() || !managedConfigPaths(tool).contains(path)) {
            m_lastError = QStringLiteral("备份清单包含无效路径。");
            return false;
        }
        if (!entry.value(QStringLiteral("existed")).toBool()) {
            if (QFileInfo::exists(path) && !QFile::remove(path)) {
                m_lastError = QStringLiteral("无法删除备份前不存在的配置：%1").arg(path);
                return false;
            }
            continue;
        }

        QFile payload(backupPath + QLatin1Char('/')
                      + entry.value(QStringLiteral("payload")).toString());
        if (!payload.open(QIODevice::ReadOnly)) {
            m_lastError = QStringLiteral("备份内容缺失：%1").arg(path);
            return false;
        }
        if (!writeTextFile(path, payload.readAll())) {
            return false;
        }
    }
    return true;
}

bool ToolManager::restoreBackup(const QString &backupId, AiTool tool)
{
    m_lastError.clear();
    const QString safetyBackupId = createBackup(tool);
    if (safetyBackupId.isEmpty()) {
        return false;
    }
    if (restoreBackupInternal(backupId, tool)) {
        pruneBackups(tool);
        return true;
    }

    const QString restoreError = m_lastError;
    const bool recovered = restoreBackupInternal(safetyBackupId, tool);
    pruneBackups(tool);
    m_lastError = recovered
        ? QStringLiteral("%1（当前配置已恢复）").arg(restoreError)
        : QStringLiteral("%1；恢复当前配置也失败：%2").arg(restoreError, m_lastError);
    return false;
}

bool ToolManager::writeTextFile(const QString &path, const QByteArray &data)
{
    QFileInfo info(path);
    QDir dir = info.dir();
    if (!dir.exists() && !dir.mkpath(".")) {
        m_lastError = QStringLiteral("无法创建目录：%1").arg(dir.path());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        m_lastError = QStringLiteral("无法写入文件：%1").arg(path);
        return false;
    }
    if (file.write(data) != data.size()) {
        m_lastError = QStringLiteral("配置文件写入不完整：%1").arg(path);
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        m_lastError = QStringLiteral("无法提交配置文件：%1").arg(path);
        return false;
    }
    return true;
}

QString ToolManager::readConfiguredKey(AiTool tool) const
{
    switch (tool) {
    case AiTool::ClaudeCode: {
        QFile file(homeFilePath(QStringLiteral(".claude/settings.json")));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        return root.value(QStringLiteral("env")).toObject()
            .value(QStringLiteral("ANTHROPIC_AUTH_TOKEN")).toString();
    }
    case AiTool::CodexCli: {
        QFile file(homeFilePath(QStringLiteral(".codex/auth.json")));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        return QJsonDocument::fromJson(file.readAll()).object()
            .value(QStringLiteral("OPENAI_API_KEY")).toString();
    }
    case AiTool::GeminiCli: {
        QFile file(homeFilePath(QStringLiteral(".gemini/.env")));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        const QString content = QString::fromUtf8(file.readAll());
        for (const QString &line : content.split(QLatin1Char('\n'))) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) {
                continue;
            }
            const int separator = trimmed.indexOf(QLatin1Char('='));
            if (separator < 0
                    || trimmed.left(separator).trimmed() != QStringLiteral("GEMINI_API_KEY")) {
                continue;
            }
            QString value = trimmed.mid(separator + 1).trimmed();
            if (value.size() >= 2
                    && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                        || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))) {
                value = value.mid(1, value.size() - 2);
            }
            return value;
        }
        return QString();
    }
    }
    return QString();
}

// ── 核心检测（可指定超时）────────────────────────────────────────
ToolStatus ToolManager::detectWithTimeout(AiTool tool, int timeoutMs)
{
    ToolStatus status;
    status.nodeOk    = commandExists("node", timeoutMs);
    status.installed = commandExists(cliCommand(tool), timeoutMs);

    // 兜底：PATH 里找不到时查 npm 全局包（有些环境 npm bin 不在 PATH）
    if (!status.installed && status.nodeOk) {
        QProcess process;
        process.start(kNpmCmd, QStringList() << "list" << "-g" << npmPackage(tool) << "--depth=0");
        if (process.waitForFinished(timeoutMs)) {
            const QString output = QString::fromUtf8(process.readAllStandardOutput());
            status.installed = output.contains(npmPackage(tool)) && !output.contains("(empty)");
        } else {
            process.kill();
        }
    }

    switch (tool) {
    case AiTool::ClaudeCode: {
        const QString key = readConfiguredKey(tool);
        QFile file(homeFilePath(".claude/settings.json"));
        if (file.exists() && file.open(QIODevice::ReadOnly)) {
            const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
            const QJsonObject env = root["env"].toObject();
            if (env["ANTHROPIC_BASE_URL"].toString().contains("aegisy.cc")
                    && !key.isEmpty()) {
                status.configured = true;
                status.configuredKey = key;
            }
        }
        // 官方指南：ANTHROPIC_API_KEY 与 AUTH_TOKEN 并存会 401
        if (!qgetenv("ANTHROPIC_API_KEY").isEmpty()) {
            status.conflictWarning =
                QStringLiteral("检测到系统环境变量 ANTHROPIC_API_KEY，会与接入配置冲突导致 401，建议删除该变量");
        }
        break;
    }
    case AiTool::CodexCli: {
        const QString key = readConfiguredKey(tool);
        QFile toml(homeFilePath(".codex/config.toml"));
        bool baseOk = false;
        if (toml.exists() && toml.open(QIODevice::ReadOnly)) {
            const QString content = QString::fromUtf8(toml.readAll());
            toml.close();
            baseOk = content.contains("aegisy.cc");
        }
        status.configured = baseOk && !key.isEmpty();
        if (status.configured) status.configuredKey = key;
        // 检查 OPENAI_API_KEY 是否指向别的账号
        const QString envKey = qgetenv("OPENAI_API_KEY");
        if (!envKey.isEmpty() && (!status.configured || envKey != key)) {
            status.conflictWarning =
                QStringLiteral("检测到系统环境变量 OPENAI_API_KEY，可能覆盖档案配置，建议删除该变量");
        }
        break;
    }
    case AiTool::GeminiCli: {
        const QString key = readConfiguredKey(tool);
        QFile envFile(homeFilePath(".gemini/.env"));
        if (envFile.exists() && envFile.open(QIODevice::ReadOnly)) {
            const QString content = QString::fromUtf8(envFile.readAll());
            envFile.close();
            bool baseOk = false;
            for (const QString &line : content.split('\n')) {
                const QString t = line.trimmed();
                if (t.startsWith("GOOGLE_GEMINI_BASE_URL") && t.contains("aegisy.cc")) {
                    baseOk = true;
                }
            }
            status.configured = baseOk && !key.isEmpty();
            if (status.configured) status.configuredKey = key;
        }
        break;
    }
    }

    return status;
}

ToolStatus ToolManager::detect(AiTool tool)
{
    return detectWithTimeout(tool, 5000);
}

ToolStatus ToolManager::detectFast(AiTool tool)
{
    return detectWithTimeout(tool, 2000);
}

// ── 桌面应用检测 ─────────────────────────────────────────────────
DesktopAppStatus ToolManager::detectDesktop(AiTool tool)
{
    DesktopAppStatus result;

    switch (tool) {
    case AiTool::ClaudeCode: {
        result.appName     = QStringLiteral("Claude 桌面版");
        result.downloadUrl = QStringLiteral("https://claude.ai/download");
#if defined(Q_OS_MAC)
        result.installed = QFile::exists("/Applications/Claude.app");
#elif defined(Q_OS_WIN)
        // 常见安装路径
        const QString localApp = qgetenv("LOCALAPPDATA");
        result.installed = QFile::exists(localApp + "\\Programs\\Claude\\claude.exe")
                        || QFile::exists(localApp + "\\Claude\\claude.exe");
#else
        // Linux：检查常见位置
        result.installed = commandExists("claude-desktop", 2000)
                        || QFile::exists(QDir::homePath() + "/.local/share/applications/claude.desktop");
#endif
        break;
    }
    case AiTool::CodexCli: {
        // Codex 本身是 CLI 工具；OpenAI 桌面客户端是 ChatGPT
        result.appName     = QStringLiteral("ChatGPT 桌面版");
        result.downloadUrl = QStringLiteral("https://chatgpt.com/download");
#if defined(Q_OS_MAC)
        result.installed = QFile::exists("/Applications/ChatGPT.app");
#elif defined(Q_OS_WIN)
        const QString localApp = qgetenv("LOCALAPPDATA");
        result.installed = QFile::exists(localApp + "\\Programs\\OpenAI\\ChatGPT\\chatgpt.exe");
#else
        result.installed = false;  // Linux 暂无官方桌面版
#endif
        break;
    }
    case AiTool::GeminiCli: {
        // Gemini 暂无独立桌面版，指向 Web
        result.appName     = QStringLiteral("Gemini Web");
        result.downloadUrl = QStringLiteral("https://gemini.google.com/");
        result.installed   = true;  // 网页版始终"可用"
        break;
    }
    }

    return result;
}

// ── 异步安装 ─────────────────────────────────────────────────────
void ToolManager::install(AiTool tool, int requestId)
{
    QProcess *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);

    const auto complete = [this, process, tool, requestId](bool success) {
        if (process->property("aegisyCompletionEmitted").toBool()) {
            return;
        }
        process->setProperty("aegisyCompletionEmitted", true);
        process->deleteLater();
        emit installFinished(tool, requestId, success);
    };

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, tool]() {
        const QString text = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
        if (!text.isEmpty()) {
            emit installOutput(tool, text);
        }
    });

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [complete](int exitCode, QProcess::ExitStatus exitStatus) {
        complete(exitStatus == QProcess::NormalExit && exitCode == 0);
    });

    connect(process, &QProcess::errorOccurred, this, [this, process, tool, complete](QProcess::ProcessError) {
        emit installOutput(tool, QStringLiteral("无法启动 npm：%1").arg(process->errorString()));
        complete(false);
    });

    emit installOutput(tool, QStringLiteral("$ %1 install -g %2").arg(kNpmCmd, npmPackage(tool)));
    process->start(kNpmCmd, QStringList() << "install" << "-g" << npmPackage(tool));
}

// ── 配置写入 ─────────────────────────────────────────────────────
bool ToolManager::configure(AiTool tool, const QString &apiKey, const QString &model)
{
    m_lastError.clear();
    if (apiKey.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("API Key 不能为空");
        return false;
    }

    const QString backupId = createBackup(tool);
    if (backupId.isEmpty()) {
        return false;
    }

    bool success = false;
    switch (tool) {
    case AiTool::ClaudeCode: success = configureClaudeCode(apiKey, model); break;
    case AiTool::CodexCli:   success = configureCodexCli(apiKey, model); break;
    case AiTool::GeminiCli:  success = configureGeminiCli(apiKey, model); break;
    }
    if (!success) {
        const QString writeError = m_lastError;
        const bool rolledBack = restoreBackupInternal(backupId, tool);
        pruneBackups(tool);
        m_lastError = rolledBack
            ? QStringLiteral("%1（已自动回滚）").arg(writeError)
            : QStringLiteral("%1；自动回滚失败：%2").arg(writeError, m_lastError);
        return false;
    }

    if (readConfiguredKey(tool) != apiKey) {
        const QString validationError = QStringLiteral(
            "写入后校验失败：%1").arg(configFilePath(tool));
        const bool rolledBack = restoreBackupInternal(backupId, tool);
        pruneBackups(tool);
        m_lastError = rolledBack
            ? QStringLiteral("%1（已自动回滚）").arg(validationError)
            : QStringLiteral("%1；自动回滚失败：%2").arg(validationError, m_lastError);
        return false;
    }
    pruneBackups(tool);
    return true;
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

    QJsonObject env = root["env"].toObject();
    env["ANTHROPIC_BASE_URL"]                      = kBaseUrl;
    env["ANTHROPIC_AUTH_TOKEN"]                    = apiKey;
    env["CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC"] = QStringLiteral("1");
    root["env"] = env;

    return writeTextFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool ToolManager::configureCodexCli(const QString &apiKey, const QString &model)
{
    const QString effectiveModel = model.isEmpty() ? QStringLiteral("gpt-4o") : model;
    // 1) ~/.codex/auth.json
    const QString authPath = homeFilePath(".codex/auth.json");
    QJsonObject auth;
    {
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
    static const QStringList managedTopKeys = {
        "model_provider", "model", "review_model", "model_reasoning_effort",
        "disable_response_storage", "network_access", "windows_wsl_setup_acknowledged",
    };
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
    if (!result.isEmpty()) result += "\n\n";

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
    const QString path = homeFilePath(".gemini/.env");

    QString existing;
    QFile file(path);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        existing = QString::fromUtf8(file.readAll());
        file.close();
    }

    static const QStringList managedKeys = {
        "GOOGLE_GEMINI_BASE_URL", "GEMINI_API_KEY", "GEMINI_MODEL",
    };

    QStringList outLines;
    for (const QString &raw : existing.split('\n')) {
        const QString t = raw.trimmed();
        const int separator = t.indexOf(QLatin1Char('='));
        const QString variable = separator >= 0 ? t.left(separator).trimmed() : QString();
        const bool managed = managedKeys.contains(variable);
        if (!managed) outLines.append(raw);
    }
    while (!outLines.isEmpty() && outLines.last().trimmed().isEmpty()) {
        outLines.removeLast();
    }

    QString result = outLines.join('\n');
    if (!result.isEmpty()) result += "\n";
    result += QStringLiteral(
        "GOOGLE_GEMINI_BASE_URL=\"%1\"\n"
        "GEMINI_API_KEY=\"%2\"\n"
        "GEMINI_MODEL=\"%3\"\n").arg(kBaseUrl, apiKey, effectiveModel);

    return writeTextFile(path, result.toUtf8());
}
