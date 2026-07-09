#ifndef ENV_DETECTOR_H
#define ENV_DETECTOR_H

#include <QObject>
#include <QString>
#include <QMap>
#include <QVariant>

// 环境检测结果结构
struct EnvStatus {
    bool isConfigured;
    QString configPath;
    QString apiKey;
    QString baseUrl;
    QString error;
};

class EnvDetector : public QObject
{
    Q_OBJECT

public:
    explicit EnvDetector(QObject *parent = nullptr);

    // 检测 Claude Desktop 配置
    EnvStatus detectClaude();

    // 检测 Cursor 配置
    EnvStatus detectCursor();

    // 检测 Continue.dev 配置
    EnvStatus detectContinue();

    // 检测系统环境变量
    QMap<QString, QString> detectEnvVars();

    // 检测所有环境
    QMap<QString, EnvStatus> detectAll();

    // 检测应用程序是否安装
    bool isClaudeInstalled();
    bool isCursorInstalled();
    bool isContinueInstalled();

    // 检测 npm 全局包
    bool isNpmPackageInstalled(const QString &packageName);
    QStringList getInstalledNpmPackages();

    // 获取配置文件路径（公开方法）
    QString getClaudeConfigPath();
    QString getCursorConfigPath();
    QString getContinueConfigPath();

private:

    // 读取 JSON 配置
    EnvStatus readJsonConfig(const QString &path,
                            const QStringList &apiKeyPaths,
                            const QStringList &baseUrlPaths);

    // 辅助函数：从 JSON 对象获取嵌套值
    QVariant getNestedValue(const QVariantMap &map, const QString &path);
};

#endif // ENV_DETECTOR_H
