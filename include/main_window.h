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

#include "companion_configuration_cache_presentation.h"
#include "companion_activation_journal.h"
#include "extension_enablement_workflow.h"
#include "extension_inventory_coordinator.h"
#include "extension_review_workflow.h"
#include "companion_activation_journal_secure_storage_adapter.h"
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
class RuntimeStatusBar;
class RuntimeStatusStore;
class QTimer;
class QEvent;
class QShowEvent;
class QFileSystemWatcher;
class StatusBadge;
class QStackedWidget;
class QTableWidget;
class AgentWorkbenchWidget;
class QThread;
class CompanionConfigurationCacheWorker;
class QSettings;
class ExtensionCenterDialog;

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
    void onCompanionConfigurationReceived(const QJsonObject &projection);
    void onCompanionConfigurationFailed(const QString &errorCode);
    void onCompanionWebsiteModelsObserved(
        const QString &accountIdentity,
        const QString &configurationSha256,
        const QString &keyIdentity,
        const QString &platform,
        const QJsonObject &projection,
        qint64 observedAtMs);
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
    void onExtensionCenterClicked();
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
    struct ActivationEntry {
        QString profileId;
        QString profileIdentity;
        bool gatewayMode = false;
    };

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
    void setupRuntimeStatusBar();
    void rebuildTrayMenu();
    void updateRuntimeProfileStatus();
    void showRuntimeStatusBar();
    void hideRuntimeStatusBar();
    void switchWorkspacePage(int pageIndex);
    void refreshDesktopPage();
    void refreshGatewayPage();
    void refreshGatewayLogs();
    void refreshCachedWorkbenchEmergencyPolicy();
    void applyWorkbenchEmergencyPolicy(const QJsonObject &policy);
    void updateCompanionProjectionStatus(const QJsonObject &projection, bool online);
    void initializeCompanionConfigurationCache();
    void updateCompanionCacheStatus(
        const CompanionConfigurationCacheView &view);
    void loadCompanionCacheStatus(bool showLoading = true);
    void clearCompanionCacheView();
    CompanionConfigurationCachePresentation currentCompanionCachePresentation() const;
    ExtensionInventoryInputs extensionInventoryInputs() const;
    void startExtensionReviewOperation(
        ExtensionCenterDialog *dialog,
        const ExtensionInventoryInputs &inputs,
        const ExtensionReviewRequest &request);
    void startExtensionEnablementOperation(
        ExtensionCenterDialog *dialog,
        const ExtensionInventoryInputs &inputs,
        const ExtensionEnablementRequest &request);
    // 收回记录改动两份账本，因此它的回执同时替换两者。部分完成绝不能报成成功。
    void startExtensionRemovalOperation(
        ExtensionCenterDialog *dialog,
        const ExtensionInventoryInputs &inputs,
        ExtensionKind kind, const QString &id);
    // 披露一个扩展包。这条路径不写任何账本，也不写磁盘：它只读一个目录。因此它不参与
    // 复核/授权/收回共用的那个 CAS 代号，用一个独立的线程槽位——让一次纯读取去阻塞一次
    // 账本写入，或者反过来，都是没有理由的。
    void startExtensionBundleDisclosure(ExtensionCenterDialog *dialog);
    // 检查一次更新。与披露一样，这条路径只读：它读一份候选包，重新读一次清单与复核账本，
    // 然后把证据摆出来。它不写账本、不写磁盘、不替换任何版本，因此同样用披露那个独立槽位。
    void startExtensionUpdateCheck(ExtensionCenterDialog *dialog,
                                   const ExtensionInventoryInputs &inputs,
                                   ExtensionKind kind, const QString &id);
    // 暂存备份浏览只读：它只清点暂存备份域（清点不触碰密钥、不写任何字节），因此与披露/
    // 更新同理用独立的线程槽位与独立代号，不与账本写入争用复核槽位。浏览区没有任何动作，
    // 这条路径是扩展中心里唯一读取暂存备份域的产品代码。
    void startExtensionBackupListing(ExtensionCenterDialog *dialog);

    // 档案卡片
    void rebuildCards();
    QWidget* createProfileCard(const Profile &profile, bool isActive,
                               bool needsRepair = false,
                               const QString &repairReason = QString());
    QWidget* createAddCard();

    // 档案操作
    bool activateProfile(int index);
    void startActivationQueue(const QList<int> &profileIndices);
    void processActivationQueue();
    void abortActivation(const QString &message);
    void recoverPendingActivation();
    void requireActivationRecovery(const QString &message);
    // 显式的人工恢复动作：不推断历史，而是重新建立一个可验证的当前状态。
    void runReviewedActivationRecovery();
    void discardPendingProfileReplacement();
    void finalizePendingProfileReplacement(const QString &activatedProfileId);
    void editProfile(int index);
    void deleteProfile(int index);
    void launchProfile(int index, bool embedded = false);
    bool confirmConfigurationPreview(const QList<Profile> &profiles,
                                     bool allowSkipPreference);
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
    RuntimeStatusStore *m_runtimeStatusStore;
    QSettings *m_activationJournalSettings = nullptr;
    SecureStorageCompanionActivationJournalAdapter *m_activationJournalAuthority =
        nullptr;
    CompanionActivationJournal *m_activationJournal = nullptr;
    AgentWorkbenchWidget *m_agentWorkbench = nullptr;
    QThread *m_companionCacheThread = nullptr;
    CompanionConfigurationCacheWorker *m_companionCacheWorker = nullptr;
    QThread *m_extensionReviewThread = nullptr;
    quint64 m_extensionReviewGeneration = 0;
    // 披露用独立的线程槽位与独立的代号：它不写账本，所以它与账本写入之间没有 CAS 竞争。
    QThread *m_extensionBundleThread = nullptr;
    quint64 m_extensionBundleGeneration = 0;
    // 暂存备份浏览用独立的线程槽位与独立的代号：它只读不写，与账本写入之间没有 CAS
    // 竞争。析构时与复核线程同样 join。
    QThread *m_extensionBackupThread = nullptr;
    quint64 m_extensionBackupGeneration = 0;
    quint64 m_companionCacheGeneration = 0;
    CompanionConfigurationCachePresentation m_companionCachePresentation;
    QString m_companionCacheViewAccountIdentity;
    bool m_companionLiveProjectionAvailable = false;

    // UI — 顶栏
    QPushButton *m_userLabel;
    QPushButton *m_balanceButton;
    QPushButton *m_logoutButton;
    QStackedWidget *m_workspaceStack = nullptr;
    QButtonGroup *m_navGroup = nullptr;

    // UI — 档案列表区
    QPushButton  *m_bulkSwitchButton = nullptr;
    QPushButton  *m_newConnectButton;
    QScrollArea  *m_cardsScroll;
    QWidget      *m_cardsContainer;
    QVBoxLayout  *m_cardsLayout;
    QButtonGroup *m_filterGroup = nullptr;   // 类型筛选按钮组
    StatusBadge  *m_profileCountLabel = nullptr;
    StatusBadge  *m_activeProfileLabel = nullptr;
    StatusBadge  *m_websiteProjectionLabel = nullptr;

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
    QPushButton *m_extensionCenterButton = nullptr;
    QAction *m_checkUpdatesAction = nullptr;
    QAction *m_autoUpdateChecksAction = nullptr;
    QTextEdit   *m_logOutput;
    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;
    RuntimeStatusBar *m_runtimeStatusBar = nullptr;

    // 状态
    QString    m_authToken;
    QJsonObject m_userInfo;
    QString    m_companionAccountIdentity;
    bool       m_waitingForCompanionAccount = false;
    double     m_balance = 0.0;
    bool       m_balanceKnown = false;
    QHash<int, QLabel *> m_toolVersionLabels;
    QHash<int, QPushButton *> m_toolInstallButtons;
    QHash<int, QPushButton *> m_toolLaunchButtons;
    QLabel *m_chatGptDesktopStatus = nullptr;
    QLabel *m_claudeDesktopStatus = nullptr;
    QPushButton *m_chatGptDesktopAction = nullptr;
    QPushButton *m_claudeDesktopAction = nullptr;
    QLabel *m_gatewayStateLabel = nullptr;
    QLabel *m_gatewayEndpointLabel = nullptr;
    QLabel *m_gatewayModeLabel = nullptr;
    QLabel *m_gatewayMessageLabel = nullptr;
    QPushButton *m_activationRecoveryButton = nullptr;
    QPushButton *m_gatewayStartButton = nullptr;
    QPushButton *m_gatewayRestartButton = nullptr;
    QPushButton *m_gatewayStopButton = nullptr;
    QTableWidget *m_gatewayLogTable = nullptr;
    QHash<int, QString> m_toolVersionTexts;
    QHash<int, QString> m_toolLocalVersions;
    QHash<int, QString> m_toolLatestVersions;
    QSet<int> m_installingTools;
    int m_pendingToolVersionChecks = 0;
    QTimer *m_balanceRefreshTimer = nullptr;
    QFileSystemWatcher *m_configurationWatcher = nullptr;
    QTimer *m_configurationRefreshTimer = nullptr;
    QTimer *m_workbenchPolicyRefreshTimer = nullptr;
    bool m_workbenchEmergencyDisabled = false;
    bool m_workbenchEmergencyPolicyVerified = false;
    QString m_workbenchEmergencyReasonCode;

    // 当前筛选类型：0 = 全部，其余值对应 ProfileType
    int m_filterType = 0;

    // 激活流程状态（含自动安装的异步队列）
    QList<ActivationEntry> m_activationQueue;
    int           m_activatingIndex = -1;
    int           m_activationGeneration = 0;
    QString       m_replacementOriginalProfileId;
    QString       m_replacementCandidateProfileId;
    bool          m_activationRecoveryRequired = false;
    bool          m_quitting = false;
    bool          m_trayHintShown = false;
    bool          m_authExpiredHandled = false;

    // 卡片测试：requestId -> {resultBadge, testAction}
    QHash<QString, QPair<StatusBadge*, QAction*>> m_cardTestWidgets;
};

#endif // MAIN_WINDOW_H
