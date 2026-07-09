#ifndef QUICK_SETUP_WIZARD_H
#define QUICK_SETUP_WIZARD_H

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QCheckBox>
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
    void onStartSetup();
    void onSkipSetup();
    void onApiKeysReceived(const QJsonArray &keys);
    void onRequestFailed(const QString &error);

private:
    void setupUi();
    void startAutoSetup();
    void detectEnvironments();
    void selectBestApiKey();
    void applyConfiguration();
    void testConnection();
    void showSuccess();
    void showError(const QString &error);

    void updateProgress(int value, const QString &status);
    void logMessage(const QString &message, const QString &color = "#333");

    ApiClient *m_apiClient;
    ConfigManager *m_configManager;
    EnvDetector *m_envDetector;

    // UI Elements
    QLabel *m_titleLabel;
    QLabel *m_descLabel;
    QProgressBar *m_progressBar;
    QLabel *m_statusLabel;
    QTextEdit *m_logOutput;
    QPushButton *m_startButton;
    QPushButton *m_skipButton;
    QPushButton *m_closeButton;

    // Auto-detected settings
    QString m_selectedApiKey;
    QString m_baseUrl;
    QStringList m_targetApps;

    // Step tracking
    int m_currentStep;
    bool m_setupInProgress;
};

#endif // QUICK_SETUP_WIZARD_H
