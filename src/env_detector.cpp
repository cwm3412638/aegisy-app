#include "env_detector.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QProcess>

EnvDetector::EnvDetector(QObject *parent)
    : QObject(parent)
{
}

QString EnvDetector::getClaudeConfigPath()
{
#ifdef Q_OS_WIN
    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir::cleanPath(appData + "/../Claude/claude_desktop_config.json");
#elif defined(Q_OS_MACOS)
    return QDir::homePath() + "/Library/Application Support/Claude/claude_desktop_config.json";
#else
    return QDir::homePath() + "/.config/Claude/claude_desktop_config.json";
#endif
}

QString EnvDetector::getCursorConfigPath()
{
#ifdef Q_OS_WIN
    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir::cleanPath(appData + "/../Cursor/User/settings.json");
#elif defined(Q_OS_MACOS)
    return QDir::homePath() + "/Library/Application Support/Cursor/User/settings.json";
#else
    return QDir::homePath() + "/.config/Cursor/User/settings.json";
#endif
}

QString EnvDetector::getContinueConfigPath()
{
    return QDir::homePath() + "/.continue/config.json";
}

QString EnvDetector::getCodexConfigDir()
{
    // Codex CLI 配置目录：~/.codex（含 config.toml / auth.json）
    return QDir::homePath() + "/.codex";
}

EnvStatus EnvDetector::detectClaude()
{
    QString path = getClaudeConfigPath();
    QStringList apiKeyPaths = {"anthropic.api_key", "api_key"};
    QStringList baseUrlPaths = {"anthropic.base_url", "base_url"};

    return readJsonConfig(path, apiKeyPaths, baseUrlPaths);
}

EnvStatus EnvDetector::detectCursor()
{
    QString path = getCursorConfigPath();
    QStringList apiKeyPaths = {"cursor.general.apiKey", "apiKey"};
    QStringList baseUrlPaths = {"cursor.general.baseUrl", "baseUrl"};

    return readJsonConfig(path, apiKeyPaths, baseUrlPaths);
}

EnvStatus EnvDetector::detectContinue()
{
    QString path = getContinueConfigPath();
    EnvStatus status;
    status.isConfigured = false;
    status.configPath = path;

    QFile file(path);
    if (!file.exists()) {
        status.error = "Configuration file not found";
        return status;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        status.error = "Cannot read configuration file";
        return status;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        status.error = "Invalid JSON format";
        return status;
    }

    QJsonObject root = doc.object();
    QJsonArray models = root["models"].toArray();

    // 查找第一个配置的模型
    for (const QJsonValue &value : models) {
        QJsonObject model = value.toObject();
        QString apiKey = model["apiKey"].toString();
        QString apiBase = model["apiBase"].toString();

        if (!apiKey.isEmpty()) {
            status.isConfigured = true;
            status.apiKey = apiKey;
            status.baseUrl = apiBase;
            break;
        }
    }

    return status;
}

EnvStatus EnvDetector::detectCodex()
{
    // Codex CLI 使用 ~/.codex 目录，密钥存于 auth.json，base_url 常见于环境变量
    EnvStatus status;
    status.isConfigured = false;
    status.configPath = getCodexConfigDir();

    // 1) 读取 ~/.codex/auth.json 中的 OPENAI_API_KEY
    const QString authPath = getCodexConfigDir() + "/auth.json";
    QFile authFile(authPath);
    if (authFile.exists() && authFile.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(authFile.readAll());
        authFile.close();
        if (doc.isObject()) {
            const QJsonObject obj = doc.object();
            QString key = obj["OPENAI_API_KEY"].toString();
            if (key.isEmpty()) {
                key = obj["api_key"].toString();
            }
            if (!key.isEmpty()) {
                status.apiKey = key;
                status.isConfigured = true;
            }
        }
    }

    // 2) 回退到环境变量
    if (status.apiKey.isEmpty()) {
        QByteArray envKey = qgetenv("OPENAI_API_KEY");
        if (!envKey.isEmpty()) {
            status.apiKey = QString::fromUtf8(envKey);
            status.isConfigured = true;
        }
    }

    QByteArray envBase = qgetenv("OPENAI_BASE_URL");
    if (!envBase.isEmpty()) {
        status.baseUrl = QString::fromUtf8(envBase);
    }

    return status;
}

QMap<QString, QString> EnvDetector::detectEnvVars()
{
    QMap<QString, QString> envVars;
    QStringList keys = {
        "OPENAI_API_KEY",
        "OPENAI_BASE_URL",
        "ANTHROPIC_API_KEY",
        "ANTHROPIC_BASE_URL"
    };

    for (const QString &key : keys) {
        QByteArray value = qgetenv(key.toUtf8());
        if (!value.isEmpty()) {
            envVars[key] = QString::fromUtf8(value);
        }
    }

    return envVars;
}

QMap<QString, EnvStatus> EnvDetector::detectAll()
{
    QMap<QString, EnvStatus> results;

    // 检测 Claude
    EnvStatus claudeStatus = detectClaude();
    claudeStatus.error = isClaudeInstalled() ?
        (claudeStatus.isConfigured ? "" : "已安装但未配置") :
        "未安装";
    results["Claude"] = claudeStatus;

    // 检测 Cursor
    EnvStatus cursorStatus = detectCursor();
    cursorStatus.error = isCursorInstalled() ?
        (cursorStatus.isConfigured ? "" : "已安装但未配置") :
        "未安装";
    results["Cursor"] = cursorStatus;

    // 检测 Continue
    EnvStatus continueStatus = detectContinue();
    continueStatus.error = isContinueInstalled() ?
        (continueStatus.isConfigured ? "" : "未安装") :
        "未安装";
    results["Continue"] = continueStatus;

    // 检测 Codex CLI（npm 全局包 / PATH）
    EnvStatus codexCliStatus = detectCodex();
    const bool codexCliInstalled = isCodexCliInstalled();
    codexCliStatus.isConfigured = codexCliInstalled;
    if (codexCliInstalled) {
        codexCliStatus.error = codexCliStatus.apiKey.isEmpty() ? "已安装(未配置密钥)" : "";
    } else {
        codexCliStatus.error = "未安装 (npm i -g @openai/codex)";
    }
    results["Codex CLI"] = codexCliStatus;

    // 检测 Codex 桌面版
    EnvStatus codexDesktopStatus;
    codexDesktopStatus.configPath = "Codex 桌面应用";
    const bool codexDesktopInstalled = isCodexDesktopInstalled();
    codexDesktopStatus.isConfigured = codexDesktopInstalled;
    codexDesktopStatus.error = codexDesktopInstalled ? "" : "未安装";
    results["Codex 桌面版"] = codexDesktopStatus;

    // 环境变量检测
    QMap<QString, QString> envVars = detectEnvVars();
    EnvStatus envStatus;
    envStatus.configPath = "系统环境变量";
    envStatus.isConfigured = !envVars.isEmpty();

    if (!envVars.isEmpty()) {
        // 提取 aegisy 相关的配置
        if (envVars.contains("ANTHROPIC_API_KEY")) {
            envStatus.apiKey = envVars["ANTHROPIC_API_KEY"];
        }
        if (envVars.contains("ANTHROPIC_BASE_URL")) {
            envStatus.baseUrl = envVars["ANTHROPIC_BASE_URL"];
        }
    }

    results["环境变量"] = envStatus;

    return results;
}

bool EnvDetector::isClaudeInstalled()
{
    // 检测 Claude Desktop 是否安装
#ifdef Q_OS_WIN
    // Windows: 检查应用程序是否存在
    QStringList possiblePaths = {
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + "/../Local/Programs/Claude"),
        "C:/Program Files/Claude",
        "C:/Program Files (x86)/Claude"
    };

    for (const QString &path : possiblePaths) {
        QDir dir(path);
        if (dir.exists()) {
            return true;
        }
    }

    // 检查配置文件是否存在（间接证明）
    return QFile::exists(getClaudeConfigPath());

#elif defined(Q_OS_MACOS)
    // macOS: 检查 Applications 文件夹
    QStringList possiblePaths = {
        "/Applications/Claude.app",
        QDir::homePath() + "/Applications/Claude.app"
    };

    for (const QString &path : possiblePaths) {
        if (QDir(path).exists()) {
            return true;
        }
    }

    return QFile::exists(getClaudeConfigPath());

#else
    // Linux: 检查配置文件和可执行文件
    QProcess process;
    process.start("which", QStringList() << "claude");
    process.waitForFinished();

    if (process.exitCode() == 0) {
        return true;
    }

    return QFile::exists(getClaudeConfigPath());
#endif
}

bool EnvDetector::isCursorInstalled()
{
    // 检测 Cursor 是否安装
#ifdef Q_OS_WIN
    QStringList possiblePaths = {
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + "/../Local/Programs/Cursor"),
        "C:/Program Files/Cursor",
        "C:/Program Files (x86)/Cursor"
    };

    for (const QString &path : possiblePaths) {
        QDir dir(path);
        if (dir.exists()) {
            return true;
        }
    }

    return QFile::exists(getCursorConfigPath());

#elif defined(Q_OS_MACOS)
    QStringList possiblePaths = {
        "/Applications/Cursor.app",
        QDir::homePath() + "/Applications/Cursor.app"
    };

    for (const QString &path : possiblePaths) {
        if (QDir(path).exists()) {
            return true;
        }
    }

    return QFile::exists(getCursorConfigPath());

#else
    QProcess process;
    process.start("which", QStringList() << "cursor");
    process.waitForFinished();

    if (process.exitCode() == 0) {
        return true;
    }

    return QFile::exists(getCursorConfigPath());
#endif
}

bool EnvDetector::isContinueInstalled()
{
    // Continue.dev 是 VS Code 扩展，检查配置文件
    return QFile::exists(getContinueConfigPath());
}

bool EnvDetector::isCodexCliInstalled()
{
    // 1) npm 全局包 @openai/codex
    if (isNpmPackageInstalled("@openai/codex")) {
        return true;
    }

    // 2) PATH 中存在 codex 可执行文件
#ifdef Q_OS_WIN
    QProcess process;
    process.start("where", QStringList() << "codex");
#else
    QProcess process;
    process.start("which", QStringList() << "codex");
#endif
    if (process.waitForFinished(5000) && process.exitCode() == 0) {
        return true;
    }

    // 3) 存在配置目录，间接证明用过 Codex CLI
    return QDir(getCodexConfigDir()).exists();
}

bool EnvDetector::isCodexDesktopInstalled()
{
    // 检测 Codex 桌面版应用是否安装
#ifdef Q_OS_WIN
    QStringList possiblePaths = {
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + "/../Local/Programs/Codex"),
        QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + "/../Local/Programs/codex"),
        "C:/Program Files/Codex",
        "C:/Program Files (x86)/Codex"
    };
    for (const QString &path : possiblePaths) {
        if (QDir(path).exists()) {
            return true;
        }
    }
    return false;

#elif defined(Q_OS_MACOS)
    QStringList possiblePaths = {
        "/Applications/Codex.app",
        QDir::homePath() + "/Applications/Codex.app"
    };
    for (const QString &path : possiblePaths) {
        if (QDir(path).exists()) {
            return true;
        }
    }
    return false;

#else
    // Linux：检查常见桌面应用目录
    QStringList possiblePaths = {
        "/opt/Codex",
        "/usr/local/bin/codex-desktop",
        QDir::homePath() + "/.local/share/applications/codex.desktop"
    };
    for (const QString &path : possiblePaths) {
        if (QFileInfo::exists(path)) {
            return true;
        }
    }
    return false;
#endif
}

bool EnvDetector::isNpmPackageInstalled(const QString &packageName)
{
    // 检测 npm 全局包是否安装
    // 注意：Windows 上 npm 是 npm.cmd，直接用 "npm" QProcess 可能找不到
#ifdef Q_OS_WIN
    const QString npmCmd = "npm.cmd";
#else
    const QString npmCmd = "npm";
#endif

    QProcess process;
    process.start(npmCmd, QStringList() << "list" << "-g" << packageName << "--depth=0");
    if (!process.waitForFinished(5000)) {
        process.kill();
        return false;
    }

    QString output = process.readAllStandardOutput();
    return !output.contains("(empty)") && output.contains(packageName);
}

QStringList EnvDetector::getInstalledNpmPackages()
{
    QStringList packages;
    QStringList checkPackages = {
        "@anthropic-ai/sdk",
        "@anthropic-ai/claude-cli",
        "@openai/codex",
        "continue",
        "cursor"
    };

    for (const QString &pkg : checkPackages) {
        if (isNpmPackageInstalled(pkg)) {
            packages.append(pkg);
        }
    }

    return packages;
}

EnvStatus EnvDetector::readJsonConfig(const QString &path,
                                     const QStringList &apiKeyPaths,
                                     const QStringList &baseUrlPaths)
{
    EnvStatus status;
    status.isConfigured = false;
    status.configPath = path;

    QFile file(path);
    if (!file.exists()) {
        status.error = "Configuration file not found";
        return status;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        status.error = "Cannot read configuration file";
        return status;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        status.error = "Invalid JSON format";
        return status;
    }

    QJsonObject root = doc.object();
    QVariantMap map = root.toVariantMap();

    // 尝试所有可能的路径
    for (const QString &keyPath : apiKeyPaths) {
        QVariant value = getNestedValue(map, keyPath);
        if (!value.isNull() && !value.toString().isEmpty()) {
            status.apiKey = value.toString();
            status.isConfigured = true;
            break;
        }
    }

    for (const QString &urlPath : baseUrlPaths) {
        QVariant value = getNestedValue(map, urlPath);
        if (!value.isNull() && !value.toString().isEmpty()) {
            status.baseUrl = value.toString();
            break;
        }
    }

    return status;
}

QVariant EnvDetector::getNestedValue(const QVariantMap &map, const QString &path)
{
    QStringList parts = path.split('.');
    QVariant current = map;

    for (const QString &part : parts) {
        if (!current.canConvert<QVariantMap>()) {
            return QVariant();
        }

        QVariantMap currentMap = current.toMap();
        if (!currentMap.contains(part)) {
            return QVariant();
        }

        current = currentMap[part];
    }

    return current;
}
