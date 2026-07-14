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
#include <QHash>
#include <QPair>
#include <QSet>
#include <QButtonGroup>
#include <QSystemTrayIcon>
#include <QMenu>
#include "api_client.h"
#include "tool_manager.h"
#include "profile_manager.h"

class QAction;
class UpdateManager;
class GatewayManager;
class DesktopEnhancementManager;
class SkillManager;
class BalanceOrb;
class QTimer;
class QEvent;
class QShowEvent;
class QFileSystemWatcher;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(UpdateManager *updateManager, QWidget *parent = nullptr);
    ~MainWindow();

    void setAuthToken(const QString &token);

signals:
    void loggedOut();

private slots:
    void onApiKeysReceived(const QJsonArray &keys);
    void onUserInfoReceived(const QJsonObject &userInfo);
    void onRequestFailed(const QString &error);
    void onAuthenticationExpired();
    void onInstallOutput(AiTool tool, const QString &line);
    void onInstallFinished(AiTool tool, int requestId, bool success);
    void onToolVersionDetected(AiTool tool, bool installed, const QString &version);
    void onToolLatestVersionDetected(AiTool tool, bool success,
                                     const QString &latestVersion,
                                     const QString &error);
    void refreshToolVersions();
    void refreshBalance();
    void onUsageClicked();
    void onAccountClicked();
    void onManageKeysClicked();
    void onViewModelsClicked();
    void onImageGenerationClicked();
    void onChatClicked();
    void onSystemDoctorClicked();
    void onGatewayClicked();
    void onDesktopEnhancementsClicked();
    void onDesktopDownloadClicked();
    void onSkillsClicked();
    void onMcpConfigClicked();
    void onHelpClicked();
    void onGatewayRunningChanged(bool running);
    void onBackupsClicked();
    void onTransferClicked();
    void onLogoutClicked();
    void onNewConnectClicked();
    void onBulkSwitchClicked();
    void onFilterChanged(int typeId);
    void onCardConnectionTested(const QString &requestId, bool success,
                                const QString &detail, int latencyMs);

private:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void setupUi();
    void setupTray();
    void setupConfigurationWatcher();
    void refreshConfigurationWatchers();
    void scheduleConfigurationRefresh();
    bool isProfileConfigurationReady(const Profile &profile,
                                     QString *issue = nullptr) const;
    void setupBalanceOrb();
    void rebuildTrayMenu();
    void updateBalanceOrb();
    void showBalanceOrb();
    void hideBalanceOrb();

    // 档案卡片
    void rebuildCards();
    QWidget* createProfileCard(const Profile &profile, bool isActive,
                               bool needsRepair = false,
                               const QString &repairReason = QString());
    QWidget* createAddCard();

    // 档案操作
    void activateProfile(int index);
    void processActivationQueue();
    bool configureFromProfile(int profileIndex, AiTool tool);
    void editProfile(int index);
    void deleteProfile(int index);
    void launchProfile(int index, bool embedded = false);
    bool confirmConfigurationPreview(const Profile &profile);
    void warnIfCliRunning(const Profile &profile);

    // 保存后环境检测弹窗
    void showEnvCheckDialog(int profileIndex);
    void installToolEnvironment(AiTool tool);

    void logMessage(const QString &message, const QString &color = "#94a3b8");
    static QString maskKey(const QString &key);

    ApiClient      *m_apiClient;
    ToolManager    *m_toolManager;
    ProfileManager *m_profileManager;
    UpdateManager  *m_updateManager;
    GatewayManager *m_gatewayManager;
    DesktopEnhancementManager *m_desktopEnhancementManager;
    SkillManager *m_skillManager;

    // UI — 顶栏
    QPushButton *m_userLabel;
    QPushButton *m_balanceButton;
    QPushButton *m_logoutButton;

    // UI — 档案列表区
    QPushButton  *m_bulkSwitchButton = nullptr;
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
    QPushButton *m_imageGenerationButton;
    QPushButton *m_backupsButton;
    QPushButton *m_transferButton;
    QPushButton *m_checkUpdatesButton;
    QPushButton *m_refreshToolVersionsButton = nullptr;
    QPushButton *m_doctorButton = nullptr;
    QPushButton *m_gatewayButton = nullptr;
    QPushButton *m_desktopEnhancementsButton = nullptr;
    QPushButton *m_desktopDownloadButton = nullptr;
    QPushButton *m_chatButton = nullptr;
    QPushButton *m_skillsButton = nullptr;
    QPushButton *m_mcpConfigButton = nullptr;
    QAction *m_checkUpdatesAction = nullptr;
    QAction *m_autoUpdateChecksAction = nullptr;
    QTextEdit   *m_logOutput;
    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;
    BalanceOrb *m_balanceOrb = nullptr;

    // 状态
    QString    m_authToken;
    QJsonArray m_keys;
    QJsonObject m_userInfo;
    bool       m_keysLoaded = false;
    double     m_balance = 0.0;
    bool       m_balanceKnown = false;
    QHash<int, QLabel *> m_toolVersionLabels;
    QHash<int, QPushButton *> m_toolInstallButtons;
    QHash<int, QString> m_toolVersionTexts;
    QHash<int, QString> m_toolLocalVersions;
    QHash<int, QString> m_toolLatestVersions;
    QSet<int> m_installingTools;
    int m_pendingToolVersionChecks = 0;
    QTimer *m_balanceRefreshTimer = nullptr;
    QFileSystemWatcher *m_configurationWatcher = nullptr;
    QTimer *m_configurationRefreshTimer = nullptr;

    // 当前筛选类型：0 = 全部，其余值对应 ProfileType
    int m_filterType = 0;

    // 激活流程状态（含自动安装的异步队列）
    QList<AiTool> m_activationQueue;
    int           m_activatingIndex = -1;
    int           m_activationGeneration = 0;
    bool          m_quitting = false;
    bool          m_trayHintShown = false;
    bool          m_authExpiredHandled = false;

    // 卡片测试：requestId -> {resultLabel, testButton}
    QHash<QString, QPair<QLabel*, QPushButton*>> m_cardTestWidgets;
};

#endif // MAIN_WINDOW_H
