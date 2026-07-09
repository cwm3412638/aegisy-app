#include "env_detector.h"
#include <QFile>
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

    results["Claude"] = detectClaude();
    results["Cursor"] = detectCursor();
    results["Continue"] = detectContinue();

    // 环境变量检测
    QMap<QString, QString> envVars = detectEnvVars();
    EnvStatus envStatus;
    envStatus.configPath = "System Environment Variables";
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

    results["Environment Variables"] = envStatus;

    return results;
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
