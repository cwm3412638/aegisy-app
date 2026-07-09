#ifndef ENV_CONFIG_DIALOG_H
#define ENV_CONFIG_DIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QTextEdit>
#include <QJsonArray>
#include "config_manager.h"
#include "env_detector.h"
#include "api_client.h"

class EnvConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EnvConfigDialog(ConfigManager *configManager,
                            EnvDetector *envDetector,
                            ApiClient *apiClient,
                            QWidget *parent = nullptr);

    // 设置要配置的 API Key 和 Base URL
    void setConfiguration(const QString &apiKey, const QString &baseUrl);

signals:
    void configurationApplied();

private slots:
    void onApplyClicked();
    void onBackupClicked();
    void onRestoreClicked();
    void onApiKeysReceived(const QJsonArray &keys);

private:
    void setupUi();
    void loadApiKeys();
    QString currentApiKey() const;
    bool backupConfigurations();
    bool applyConfiguration();
    void updateProgress(int value, const QString &message);
    void logMessage(const QString &message, const QString &color = "#000");

    ConfigManager *m_configManager;
    EnvDetector *m_envDetector;
    ApiClient *m_apiClient;

    // UI Elements
    QComboBox *m_apiKeyCombo;   // API Key 下拉（从账号列表选择，也可手动粘贴）
    QLineEdit *m_baseUrlEdit;   // Base URL：固定不可更改

    QCheckBox *m_claudeCheckBox;
    QCheckBox *m_cursorCheckBox;
    QCheckBox *m_continueCheckBox;

    QPushButton *m_applyButton;
    QPushButton *m_backupButton;
    QPushButton *m_restoreButton;
    QPushButton *m_closeButton;

    QProgressBar *m_progressBar;
    QTextEdit *m_logOutput;
    QLabel *m_statusLabel;

    QString m_apiKey;
    QString m_baseUrl;
    QString m_backupPath;
};

#endif // ENV_CONFIG_DIALOG_H
