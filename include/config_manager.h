#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>

// 环境配置结构
struct Environment {
    QString id;
    QString name;
    bool active;
    QStringList apiKeys;
    QString activeKey;
    QString baseUrl;
    QStringList targets;  // claude, cursor, continue

    QJsonObject toJson() const;
    static Environment fromJson(const QJsonObject &obj);
};

class ConfigManager : public QObject
{
    Q_OBJECT

public:
    explicit ConfigManager(QObject *parent = nullptr);

    // 加载配置
    bool load();

    // 保存配置
    bool save();

    // 获取所有环境
    QList<Environment> getEnvironments() const;

    // 添加环境
    void addEnvironment(const Environment &env);

    // 更新环境
    bool updateEnvironment(const QString &id, const Environment &env);

    // 删除环境
    bool removeEnvironment(const QString &id);

    // 获取激活的环境
    Environment getActiveEnvironment() const;

    // 设置激活的环境
    bool setActiveEnvironment(const QString &id);

    // 应用环境配置到目标应用
    bool applyEnvironment(const Environment &env);

    // 写入配置到目标应用（公有方法）
    bool writeClaudeConfig(const QString &apiKey, const QString &baseUrl);
    bool writeCursorConfig(const QString &apiKey, const QString &baseUrl);
    bool writeContinueConfig(const QString &apiKey, const QString &baseUrl);

signals:
    void environmentChanged(const Environment &env);
    void configSaved();

private:
    QString m_configPath;
    QList<Environment> m_environments;

    // 获取配置路径
    QString getConfigPath() const;
};

#endif // CONFIG_MANAGER_H
