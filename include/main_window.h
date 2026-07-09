#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QTextEdit>
#include <QJsonArray>
#include <QMap>
#include "api_client.h"
#include "tool_manager.h"
#include "profile_manager.h"

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

    // 档案相关
    void onProfileComboChanged(int index);
    void onAddProfileClicked();
    void onManageProfileClicked();

private:
    void setupUi();
    QWidget* createToolCard(AiTool tool);
    void refreshToolCard(AiTool tool);
    void refreshAllCards();

    struct KeyChoice {
        QString key;
        QString label;
    };
    QList<KeyChoice> keysForTool(AiTool tool) const;
    void configureTool(AiTool tool);

    // 档案操作
    void refreshProfileCombo();
    void applyProfile(const Profile &profile);

    void logMessage(const QString &message, const QString &color = "#94a3b8");
    static QString maskKey(const QString &key);

    ApiClient    *m_apiClient;
    ToolManager  *m_toolManager;
    ProfileManager *m_profileManager;

    // UI — 顶栏
    QLabel       *m_userLabel;
    QPushButton  *m_logoutButton;
    QComboBox    *m_profileCombo;
    QPushButton  *m_addProfileButton;
    QPushButton  *m_manageProfileButton;

    // UI — 工具卡片 + 高级区 + 日志
    QMap<AiTool, ToolCard> m_cards;
    QPushButton  *m_manageKeysButton;
    QPushButton  *m_viewModelsButton;
    QTextEdit    *m_logOutput;

    // 状态
    QString m_authToken;
    QJsonArray m_keys;
    bool m_keysLoaded = false;
    QMap<AiTool, KeyChoice> m_pendingChoice;
};

#endif // MAIN_WINDOW_H
