#ifndef ENV_CONFIG_DIALOG_H
#define ENV_CONFIG_DIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QTextEdit>
#include "config_manager.h"
#include "env_detector.h"

class EnvConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EnvConfigDialog(ConfigManager *configManager,
                            EnvDetector *envDetector,
                            QWidget *parent = nullptr);

    // 设置要配置的 API Key 和 Base URL
    void setConfiguration(const QString &apiKey, const QString &baseUrl);

signals:
    void configurationApplied();

private slots:
    void onApplyClicked();
    void onBackupClicked();
    void onRestoreClicked();

private:
    void setupUi();
    bool backupConfigurations();
    bool applyConfiguration();
    void updateProgress(int value, const QString &message);
    void logMessage(const QString &message, const QString &color = "#000");

    ConfigManager *m_configManager;
    EnvDetector *m_envDetector;

    // UI Elements
    QLineEdit *m_apiKeyEdit;
    QLineEdit *m_baseUrlEdit;

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
