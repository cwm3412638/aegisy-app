#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QList>
#include <QButtonGroup>
#include "api_client.h"
#include "tool_manager.h"
#include "profile_manager.h"

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
    void onApiKeysReceived(const QJsonArray &keys);
    void onRequestFailed(const QString &error);
    void onInstallOutput(AiTool tool, const QString &line);
    void onInstallFinished(AiTool tool, bool success);
    void onManageKeysClicked();
    void onViewModelsClicked();
    void onLogoutClicked();
    void onNewConnectClicked();
    void onFilterChanged(int typeId);

private:
    void setupUi();

    // 档案卡片
    void rebuildCards();
    QWidget* createProfileCard(const Profile &profile, bool isActive);
    QWidget* createAddCard();

    // 档案操作
    void activateProfile(int index);
    void processActivationQueue();
    void configureFromProfile(int profileIndex, AiTool tool);
    void editProfile(int index);
    void deleteProfile(int index);

    // 保存后环境检测弹窗
    void showEnvCheckDialog(int profileIndex);

    void logMessage(const QString &message, const QString &color = "#94a3b8");
    static QString maskKey(const QString &key);

    ApiClient      *m_apiClient;
    ToolManager    *m_toolManager;
    ProfileManager *m_profileManager;

    // UI — 顶栏
    QLabel      *m_userLabel;
    QPushButton *m_logoutButton;

    // UI — 档案卡片区
    QPushButton  *m_newConnectButton;
    QScrollArea  *m_cardsScroll;
    QWidget      *m_cardsContainer;
    QHBoxLayout  *m_cardsLayout;
    QButtonGroup *m_filterGroup = nullptr;   // 类型筛选按钮组

    // UI — 高级区 + 日志
    QPushButton *m_manageKeysButton;
    QPushButton *m_viewModelsButton;
    QTextEdit   *m_logOutput;

    // 状态
    QString    m_authToken;
    QJsonArray m_keys;
    bool       m_keysLoaded = false;

    // 当前筛选类型（Mixed = 全部显示）
    ProfileType m_filterType = ProfileType::Mixed;

    // 激活流程状态（含自动安装的异步队列）
    QList<AiTool> m_activationQueue;
    int           m_activatingIndex = -1;
};

#endif // MAIN_WINDOW_H
