#include "config_manager.h"
#include "env_detector.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDebug>

QJsonObject Environment::toJson() const
{
    QJsonObject obj;
    obj["id"] = id;
    obj["name"] = name;
    obj["active"] = active;
    obj["api_keys"] = QJsonArray::fromStringList(apiKeys);
    obj["active_key"] = activeKey;
    obj["base_url"] = baseUrl;
    obj["targets"] = QJsonArray::fromStringList(targets);
    return obj;
}

Environment Environment::fromJson(const QJsonObject &obj)
{
    Environment env;
    env.id = obj["id"].toString();
    env.name = obj["name"].toString();
    env.active = obj["active"].toBool();
    env.activeKey = obj["active_key"].toString();
    env.baseUrl = obj["base_url"].toString();

    QJsonArray keysArray = obj["api_keys"].toArray();
    for (const QJsonValue &val : keysArray) {
        env.apiKeys.append(val.toString());
    }

    QJsonArray targetsArray = obj["targets"].toArray();
    for (const QJsonValue &val : targetsArray) {
        env.targets.append(val.toString());
    }

    return env;
}

ConfigManager::ConfigManager(QObject *parent)
    : QObject(parent)
{
    m_configPath = getConfigPath();

    // 确保配置目录存在
    QFileInfo fileInfo(m_configPath);
    QDir dir = fileInfo.dir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }
}

QString ConfigManager::getConfigPath() const
{
    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return appData + "/environments.json";
}

bool ConfigManager::load()
{
    QFile file(m_configPath);
    if (!file.exists()) {
        // 创建默认配置
        return true;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Cannot open config file:" << m_configPath;
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        qWarning() << "Invalid config format";
        return false;
    }

    QJsonObject root = doc.object();
    QJsonArray envArray = root["environments"].toArray();

    m_environments.clear();
    for (const QJsonValue &val : envArray) {
        m_environments.append(Environment::fromJson(val.toObject()));
    }

    return true;
}

bool ConfigManager::save()
{
    QJsonObject root;
    QJsonArray envArray;

    for (const Environment &env : m_environments) {
        envArray.append(env.toJson());
    }

    root["environments"] = envArray;

    QJsonDocument doc(root);

    QFile file(m_configPath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Cannot write config file:" << m_configPath;
        return false;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    emit configSaved();
    return true;
}

QList<Environment> ConfigManager::getEnvironments() const
{
    return m_environments;
}

void ConfigManager::addEnvironment(const Environment &env)
{
    m_environments.append(env);
    save();
}

bool ConfigManager::updateEnvironment(const QString &id, const Environment &env)
{
    for (int i = 0; i < m_environments.size(); ++i) {
        if (m_environments[i].id == id) {
            m_environments[i] = env;
            save();
            return true;
        }
    }
    return false;
}

bool ConfigManager::removeEnvironment(const QString &id)
{
    for (int i = 0; i < m_environments.size(); ++i) {
        if (m_environments[i].id == id) {
            m_environments.removeAt(i);
            save();
            return true;
        }
    }
    return false;
}

Environment ConfigManager::getActiveEnvironment() const
{
    for (const Environment &env : m_environments) {
        if (env.active) {
            return env;
        }
    }
    return Environment();
}

bool ConfigManager::setActiveEnvironment(const QString &id)
{
    bool found = false;

    for (int i = 0; i < m_environments.size(); ++i) {
        if (m_environments[i].id == id) {
            m_environments[i].active = true;
            found = true;

            // 应用配置
            applyEnvironment(m_environments[i]);
            emit environmentChanged(m_environments[i]);
        } else {
            m_environments[i].active = false;
        }
    }

    if (found) {
        save();
    }

    return found;
}

bool ConfigManager::applyEnvironment(const Environment &env)
{
    bool success = true;

    QString apiKey = env.activeKey.isEmpty() ?
                     (env.apiKeys.isEmpty() ? "" : env.apiKeys.first()) :
                     env.activeKey;

    for (const QString &target : env.targets) {
        if (target == "claude") {
            success &= writeClaudeConfig(apiKey, env.baseUrl);
        } else if (target == "cursor") {
            success &= writeCursorConfig(apiKey, env.baseUrl);
        } else if (target == "continue") {
            success &= writeContinueConfig(apiKey, env.baseUrl);
        }
    }

    return success;
}

bool ConfigManager::writeClaudeConfig(const QString &apiKey, const QString &baseUrl)
{
    EnvDetector detector;
    QString configPath = detector.getClaudeConfigPath();

    QJsonObject config;
    QJsonObject anthropic;
    anthropic["api_key"] = apiKey;

    if (!baseUrl.isEmpty()) {
        anthropic["base_url"] = baseUrl;
    }

    config["anthropic"] = anthropic;

    // 确保目录存在
    QFileInfo fileInfo(configPath);
    QDir dir = fileInfo.dir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    QFile file(configPath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Cannot write Claude config:" << configPath;
        return false;
    }

    QJsonDocument doc(config);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    qDebug() << "Claude config written to:" << configPath;
    return true;
}

bool ConfigManager::writeCursorConfig(const QString &apiKey, const QString &baseUrl)
{
    EnvDetector detector;
    QString configPath = detector.getCursorConfigPath();

    // 读取现有配置
    QJsonObject config;
    QFile file(configPath);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            config = doc.object();
        }
        file.close();
    }

    // 更新配置
    config["cursor.general.apiKey"] = apiKey;
    if (!baseUrl.isEmpty()) {
        config["cursor.general.baseUrl"] = baseUrl;
    }

    // 写入配置
    QFileInfo fileInfo(configPath);
    QDir dir = fileInfo.dir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Cannot write Cursor config:" << configPath;
        return false;
    }

    QJsonDocument doc(config);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    qDebug() << "Cursor config written to:" << configPath;
    return true;
}

bool ConfigManager::writeContinueConfig(const QString &apiKey, const QString &baseUrl)
{
    EnvDetector detector;
    QString configPath = detector.getContinueConfigPath();

    // 读取现有配置
    QJsonObject config;
    QFile file(configPath);
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isObject()) {
            config = doc.object();
        }
        file.close();
    }

    // 更新 models 数组
    QJsonArray models = config["models"].toArray();
    QJsonObject model;
    model["provider"] = "openai";
    model["apiKey"] = apiKey;

    if (!baseUrl.isEmpty()) {
        model["apiBase"] = baseUrl;
    }

    // 如果没有模型配置，添加一个；否则更新第一个
    if (models.isEmpty()) {
        models.append(model);
    } else {
        models[0] = model;
    }

    config["models"] = models;

    // 写入配置
    QFileInfo fileInfo(configPath);
    QDir dir = fileInfo.dir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Cannot write Continue config:" << configPath;
        return false;
    }

    QJsonDocument doc(config);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    qDebug() << "Continue config written to:" << configPath;
    return true;
}
