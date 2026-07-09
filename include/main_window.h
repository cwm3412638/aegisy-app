#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include "api_client.h"
#include "config_manager.h"
#include "env_detector.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void setAuthToken(const QString &token);

private slots:
    void onRefreshEnvClicked();
    void onConfigureEnvClicked();
    void onManageKeysClicked();
    void onManageEnvironmentsClicked();
    void onLogoutClicked();
    void onEnvDetectionFinished();

private:
    void setupUi();
    void updateEnvDisplay(const QMap<QString, EnvStatus> &envStatuses);

    ApiClient *m_apiClient;
    ConfigManager *m_configManager;
    EnvDetector *m_envDetector;

    // UI Elements
    QLabel *m_userLabel;
    QPushButton *m_refreshButton;
    QPushButton *m_configureButton;
    QPushButton *m_manageKeysButton;
    QPushButton *m_manageEnvsButton;
    QPushButton *m_logoutButton;
    QTableWidget *m_envTable;
    QTextEdit *m_logOutput;

    QString m_authToken;
};

#endif // MAIN_WINDOW_H
