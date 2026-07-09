#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QJsonArray>
#include <QMap>
#include "api_client.h"
#include "tool_manager.h"

// 单个工具卡片的控件集合
struct ToolCard {
    QLabel *statusLabel = nullptr;
    QLabel *warnLabel = nullptr;
    QPushButton *actionButton = nullptr;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void setAuthToken(const QString &token);

signals:
    // 用户点击退出登录（token 已清除），由 main.cpp 决定后续流程
    void loggedOut();

private slots:
    void onConnectToolClicked(AiTool tool);
    void onInstallOutput(AiTool tool, const QString &line);
    void onInstallFinished(AiTool tool, bool success);
    void onApiKeysReceived(const QJsonArray &keys);
    void onRequestFailed(const QString &error);
    void onManageKeysClicked();
    void onViewModelsClicked();
    void onLogoutClicked();

private:
    void setupUi();
    QWidget* createToolCard(AiTool tool);
    void refreshToolCard(AiTool tool);
    void refreshAllCards();

    // 按分组 platform 从缓存 keys 中挑选可用 Key；无则返回空
    QString pickKeyForTool(AiTool tool, QString *keyLabel = nullptr) const;
    // 一键接入的配置阶段（安装完成后也会走到这里）
    void configureTool(AiTool tool);

    void logMessage(const QString &message, const QString &color = "#333");
    static QString maskKey(const QString &key);

    ApiClient *m_apiClient;
    ToolManager *m_toolManager;

    // UI
    QLabel *m_userLabel;
    QPushButton *m_logoutButton;
    QMap<AiTool, ToolCard> m_cards;
    QPushButton *m_manageKeysButton;
    QPushButton *m_viewModelsButton;
    QTextEdit *m_logOutput;

    // 状态
    QString m_authToken;
    QJsonArray m_keys;          // 账号 API Keys（含 group）
    bool m_keysLoaded = false;
};

#endif // MAIN_WINDOW_H
