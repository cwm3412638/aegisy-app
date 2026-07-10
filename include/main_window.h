#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QList>
#include <QButtonGroup>
#include <QSystemTrayIcon>
#include <QMenu>
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
    void onInstallFinished(AiTool tool, int requestId, bool success);
    void onManageKeysClicked();
    void onViewModelsClicked();
    void onBackupsClicked();
    void onTransferClicked();
    void onLogoutClicked();
    void onNewConnectClicked();
    void onFilterChanged(int typeId);

private:
    void closeEvent(QCloseEvent *event) override;
    void setupUi();
    void setupTray();
    void rebuildTrayMenu();

    // 档案卡片
    void rebuildCards();
    QWidget* createProfileCard(const Profile &profile, bool isActive);
    QWidget* createAddCard();

    // 档案操作
    void activateProfile(int index);
    void processActivationQueue();
    bool configureFromProfile(int profileIndex, AiTool tool);
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

    // UI — 档案列表区
    QPushButton  *m_newConnectButton;
    QScrollArea  *m_cardsScroll;
    QWidget      *m_cardsContainer;
    QVBoxLayout  *m_cardsLayout;
    QButtonGroup *m_filterGroup = nullptr;   // 类型筛选按钮组
    QLabel       *m_profileCountLabel = nullptr;
    QLabel       *m_activeProfileLabel = nullptr;

    // UI — 高级区 + 日志
    QPushButton *m_manageKeysButton;
    QPushButton *m_viewModelsButton;
    QPushButton *m_backupsButton;
    QPushButton *m_transferButton;
    QTextEdit   *m_logOutput;
    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;

    // 状态
    QString    m_authToken;
    QJsonArray m_keys;
    bool       m_keysLoaded = false;

    // 当前筛选类型：0 = 全部，其余值对应 ProfileType
    int m_filterType = 0;

    // 激活流程状态（含自动安装的异步队列）
    QList<AiTool> m_activationQueue;
    int           m_activatingIndex = -1;
    int           m_activationGeneration = 0;
    bool          m_quitting = false;
    bool          m_trayHintShown = false;
};

#endif // MAIN_WINDOW_H
