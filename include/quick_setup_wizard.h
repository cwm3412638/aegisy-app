#ifndef QUICK_SETUP_WIZARD_H
#define QUICK_SETUP_WIZARD_H

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QCheckBox>
#include <QTableWidget>
#include <QHash>
#include "api_client.h"
#include "config_manager.h"
#include "env_detector.h"

class QuickSetupWizard : public QDialog
{
    Q_OBJECT

public:
    explicit QuickSetupWizard(ApiClient *apiClient,
                             ConfigManager *configManager,
                             EnvDetector *envDetector,
                             QWidget *parent = nullptr);

signals:
    void setupCompleted();

private slots:
    void onStartAccess();
    void onSkipSetup();
    void onApiKeysReceived(const QJsonArray &keys);
    void onApiKeyTested(const QString &keyId, bool supported, const QString &detail);
    void onKeySelectionChanged();
    void onRequestFailed(const QString &error);

private:
    void setupUi();
    void populateAppList();
    void loadApiKeys();
    QString selectedApiKey() const;
    void applyConfiguration();
    void showSuccess();
    void showError(const QString &error);

    void updateProgress(int value, const QString &status);
    void logMessage(const QString &message, const QString &color = "#333");

    ApiClient *m_apiClient;
    ConfigManager *m_configManager;
    EnvDetector *m_envDetector;

    // ① 应用勾选
    QCheckBox *m_claudeCheck;
    QCheckBox *m_cursorCheck;
    QCheckBox *m_continueCheck;
    QCheckBox *m_codexCheck;

    // ② API Key 测试表
    QTableWidget *m_keyTable;

    // ③ 接入
    QProgressBar *m_progressBar;
    QLabel *m_statusLabel;
    QTextEdit *m_logOutput;
    QPushButton *m_startButton;
    QPushButton *m_skipButton;
    QPushButton *m_closeButton;

    // 状态
    QString m_baseUrl;
    QString m_selectedKeyId;                 // 当前选中的 key id
    QHash<QString, QString> m_keyValue;      // id -> 完整 key
    QHash<QString, bool> m_keySupported;     // id -> 是否可用
    QHash<QString, int> m_keyRow;            // id -> 表格行号
    bool m_setupInProgress;
};

#endif // QUICK_SETUP_WIZARD_H
