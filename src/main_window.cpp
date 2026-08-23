#include "main_window.h"

#include "api_keys_dialog.h"
#include "account_dialog.h"
#include "connect_wizard.h"
#include "companion_config_projection.h"
#include "models_dialog.h"
#include "image_generation_dialog.h"
#include "chat_dialog.h"
#include "skill_manager.h"
#include "skills_dialog.h"
#include "system_doctor_dialog.h"
#include "usage_dialog.h"
#include "gateway_manager.h"
#include "gateway_dialog.h"
#include "desktop_enhancement_manager.h"
#include "desktop_enhancement_dialog.h"
#include "secure_storage.h"
#include "app_theme.h"
#include "update_manager.h"
#include "runtime_status_bar.h"
#include "runtime_status_store.h"
#include "desktop_downloader.h"
#include "mcp_config_dialog.h"
#include "help_dialog.h"
#include "status_badge.h"
#include "agent_workbench_widget.h"
#include "workbench_emergency_policy.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QInputDialog>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QShowEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QStyle>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>

namespace {

QString toolAccent(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("#c15f3c");
    case AiTool::CodexCli:   return QStringLiteral("#111827");
    case AiTool::GeminiCli:  return QStringLiteral("#165DFF");
    case AiTool::OpenCode:   return QStringLiteral("#059669");
    }
    return QStringLiteral("#165DFF");
}

QString toolSoftColor(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("#fff4ef");
    case AiTool::CodexCli:   return QStringLiteral("#f3f4f6");
    case AiTool::GeminiCli:  return QStringLiteral("#eef5ff");
    case AiTool::OpenCode:   return QStringLiteral("#ecfdf5");
    }
    return QStringLiteral("#ecfdf5");
}

QString toolLetter(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("C");
    case AiTool::CodexCli:   return QStringLiteral("O");
    case AiTool::GeminiCli:  return QStringLiteral("G");
    case AiTool::OpenCode:   return QStringLiteral("OC");
    }
    return QStringLiteral("A");
}

QString toolConfigPath(AiTool tool)
{
    return ToolManager::configFilePath(tool);
}

const QString kLogSuccess = QStringLiteral("#067647");
const QString kLogError   = QStringLiteral("#b42318");
const QString kLogInfo    = QStringLiteral("#165DFF");
const QString kLogWarn    = QStringLiteral("#b54708");
const QString kLogMuted   = QStringLiteral("#667085");

} // namespace

MainWindow::MainWindow(UpdateManager *updateManager, QWidget *parent)
    : QMainWindow(parent)
    , m_apiClient(new ApiClient(this))
    , m_toolManager(new ToolManager(this))
    , m_profileManager(new ProfileManager(this))
    , m_updateManager(updateManager)
    , m_gatewayManager(new GatewayManager(this))
    , m_desktopEnhancementManager(new DesktopEnhancementManager(this))
    , m_skillManager(new SkillManager(this))
    , m_runtimeStatusStore(new RuntimeStatusStore(this))
{
    refreshCachedWorkbenchEmergencyPolicy();
    setupUi();
    setWindowTitle(QStringLiteral("Aegisy - 网站配套助手"));
    resize(1280, 820);
    setMinimumSize(1040, 680);

    connect(m_apiClient, &ApiClient::companionConfigurationReceived,
            this, &MainWindow::onCompanionConfigurationReceived);
    connect(m_apiClient, &ApiClient::companionConfigurationFailed,
            this, &MainWindow::onCompanionConfigurationFailed);
    connect(m_apiClient, &ApiClient::userInfoReceived,
            this, &MainWindow::onUserInfoReceived);
    connect(m_apiClient, &ApiClient::requestFailed,
            this, &MainWindow::onRequestFailed);
    connect(m_apiClient, &ApiClient::connectionTested,
            this, &MainWindow::onCardConnectionTested);
    connect(m_apiClient, &ApiClient::authenticationExpired,
            this, &MainWindow::onAuthenticationExpired);
    connect(m_apiClient, &ApiClient::workbenchEmergencyPolicyReceived,
            this, &MainWindow::applyWorkbenchEmergencyPolicy);
    connect(m_apiClient, &ApiClient::workbenchEmergencyPolicyFailed,
            this, [this](const QString &) {
        refreshCachedWorkbenchEmergencyPolicy();
    });
    connect(m_toolManager, &ToolManager::installOutput,
            this, &MainWindow::onInstallOutput);
    connect(m_toolManager, &ToolManager::installFinished,
            this, &MainWindow::onInstallFinished);
    connect(m_toolManager, &ToolManager::toolVersionDetected,
            this, &MainWindow::onToolVersionDetected);
    connect(m_toolManager, &ToolManager::toolLatestVersionDetected,
            this, &MainWindow::onToolLatestVersionDetected);
    connect(m_gatewayManager, &GatewayManager::runningChanged,
            this, &MainWindow::onGatewayRunningChanged);
    connect(m_gatewayManager, &GatewayManager::requestLogged,
            this, [this](const QJsonObject &) { refreshGatewayLogs(); });
    connect(m_gatewayManager, &GatewayManager::runningChanged,
            m_runtimeStatusStore, &RuntimeStatusStore::setGatewayRunning);
    connect(m_gatewayManager, &GatewayManager::runtimeEvent,
            m_runtimeStatusStore, &RuntimeStatusStore::observeGatewayEvent);
    connect(m_gatewayManager, &GatewayManager::gatewayError,
            this, [this](const QString &error) {
        logMessage(QStringLiteral("本地网关错误：%1").arg(error), kLogError);
        if (m_gatewayMessageLabel) {
            m_gatewayMessageLabel->setText(QStringLiteral("网关错误：%1").arg(error));
        }
    });
    connect(m_profileManager, &ProfileManager::profilesChanged,
            this, [this]() {
        refreshConfigurationWatchers();
        rebuildTrayMenu();
        updateRuntimeProfileStatus();
    });
    connect(m_profileManager, &ProfileManager::activeProfileChanged,
            this, [this](int, int) {
        rebuildTrayMenu();
        updateRuntimeProfileStatus();
    });

    m_profileManager->backfillKeyHints();
    setupConfigurationWatcher();
    refreshToolVersions();
    rebuildCards();
    setupTray();
    setupRuntimeStatusBar();
    m_workbenchPolicyRefreshTimer = new QTimer(this);
    m_workbenchPolicyRefreshTimer->setInterval(15 * 60 * 1000);
    connect(m_workbenchPolicyRefreshTimer, &QTimer::timeout, this, [this]() {
        refreshCachedWorkbenchEmergencyPolicy();
        if (!m_authToken.isEmpty()) m_apiClient->getWorkbenchEmergencyPolicy();
    });
    m_workbenchPolicyRefreshTimer->start();
    updateRuntimeProfileStatus();
    if (!m_profileManager->lastError().isEmpty()) {
        logMessage(m_profileManager->lastError(), kLogError);
    }
}

MainWindow::~MainWindow()
{
    if (m_gatewayManager && m_gatewayManager->isRunning()) {
        m_gatewayManager->stop();
    }
    if (m_runtimeStatusBar) {
        delete m_runtimeStatusBar;   // 无父对象的顶层窗口，需手动释放
        m_runtimeStatusBar = nullptr;
    }
}

void MainWindow::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    m_trayMenu = new QMenu(this);
    m_trayIcon = new QSystemTrayIcon(this);
    QIcon icon = windowIcon();
    if (icon.isNull()) {
        icon = style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    m_trayIcon->setIcon(icon);
    m_trayIcon->setToolTip(QStringLiteral("Aegisy 连接管理"));
    m_trayIcon->setContextMenu(m_trayMenu);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger
                || reason == QSystemTrayIcon::DoubleClick) {
            showNormal();
            raise();
            activateWindow();
        }
    });
    rebuildTrayMenu();
    m_trayIcon->show();
}

void MainWindow::setupRuntimeStatusBar()
{
    m_runtimeStatusBar = new RuntimeStatusBar();  // 顶层窗口，不设父对象
    connect(m_runtimeStatusBar, &RuntimeStatusBar::restoreRequested, this, [this]() {
        showNormal();
        raise();
        activateWindow();
    });
    connect(m_runtimeStatusBar, &RuntimeStatusBar::quitRequested, this, [this]() {
        m_quitting = true;
        if (m_trayIcon) m_trayIcon->hide();
        QApplication::quit();
    });
    connect(m_runtimeStatusStore, &RuntimeStatusStore::statusChanged,
            m_runtimeStatusBar, &RuntimeStatusBar::setSnapshot);
    m_runtimeStatusBar->setSnapshot(m_runtimeStatusStore->snapshot());
}

void MainWindow::updateRuntimeProfileStatus()
{
    const QList<Profile> profiles = m_profileManager->allProfiles();
    int index = m_profileManager->lastActivatedIndex();
    if (index < 0 || index >= profiles.size()) {
        for (ProfileType type : allProfileTypes()) {
            const int activeIndex = m_profileManager->activeIndex(type);
            if (activeIndex >= 0 && activeIndex < profiles.size()) {
                index = activeIndex;
                break;
            }
        }
    }
    if (index >= 0 && index < profiles.size()) {
        const Profile &profile = profiles.at(index);
        m_runtimeStatusStore->setConfiguredProfile(
            profile.tool(), profile.model,
            ToolManager::configuredReasoning(profile.tool(), profile.model),
            ToolManager::configuredContextLimit(profile.tool(), profile.model));
    } else {
        m_runtimeStatusStore->clearConfiguredProfile();
    }
}

void MainWindow::showRuntimeStatusBar()
{
    if (m_runtimeStatusBar) {
        m_runtimeStatusBar->setSnapshot(m_runtimeStatusStore->snapshot());
        m_runtimeStatusBar->show();
        m_runtimeStatusBar->raise();
    }
}

void MainWindow::hideRuntimeStatusBar()
{
    if (m_runtimeStatusBar) {
        m_runtimeStatusBar->hide();
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange) {
        if (isMinimized()) {
            showRuntimeStatusBar();
        } else if (isVisible()) {
            hideRuntimeStatusBar();
        }
    } else if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        scheduleConfigurationRefresh();
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::rebuildTrayMenu()
{
    if (!m_trayMenu) {
        return;
    }
    m_trayMenu->clear();

    QAction *showAction = m_trayMenu->addAction(QStringLiteral("打开 Aegisy"));
    connect(showAction, &QAction::triggered, this, [this]() {
        showNormal();
        raise();
        activateWindow();
    });
    m_trayMenu->addSeparator();

    const QList<Profile> profiles = m_profileManager->allProfiles();
    for (ProfileType type : allProfileTypes()) {
        const int activeIndex = m_profileManager->activeIndex(type);
        QString activeIssue;
        const bool activeReady = activeIndex >= 0 && activeIndex < profiles.size()
            && profiles[activeIndex].type == type
            && profiles[activeIndex].hasAnyKey()
            && isProfileConfigurationReady(profiles[activeIndex], &activeIssue);
        QAction *section = m_trayMenu->addAction(profileTypeName(type));
        section->setEnabled(false);
        bool hasProfile = false;
        for (const Profile &profile : profiles) {
            if (profile.type != type) {
                continue;
            }
            hasProfile = true;
            const bool needsRepair = profile.index == activeIndex
                && profile.hasAnyKey() && !activeReady;
            QAction *profileAction = m_trayMenu->addAction(
                needsRepair
                    ? QStringLiteral("%1（需修复）").arg(profile.name)
                    : profile.name);
            profileAction->setCheckable(true);
            profileAction->setChecked(profile.index == activeIndex && activeReady);
            profileAction->setEnabled(profile.hasAnyKey());
            if (needsRepair) profileAction->setToolTip(activeIssue);
            const int profileIndex = profile.index;
            connect(profileAction, &QAction::triggered, this,
                    [this, profileIndex]() { activateProfile(profileIndex); });
        }
        if (!hasProfile) {
            QAction *empty = m_trayMenu->addAction(QStringLiteral("  暂无档案"));
            empty->setEnabled(false);
        } else if (activeReady) {
            QAction *launchAction = m_trayMenu->addAction(
                QStringLiteral("启动当前 %1").arg(profileTypeName(type)));
            connect(launchAction, &QAction::triggered, this,
                    [this, activeIndex]() {
                showNormal();
                raise();
                launchProfile(activeIndex);
            });
        }
        m_trayMenu->addSeparator();
    }

    QAction *backupAction = m_trayMenu->addAction(QStringLiteral("备份与恢复"));
    connect(backupAction, &QAction::triggered, this, [this]() {
        showNormal();
        raise();
        onBackupsClicked();
    });
    if (m_updateManager && m_updateManager->isSupported()) {
        QAction *updateAction = m_trayMenu->addAction(QStringLiteral("检查更新"));
        connect(updateAction, &QAction::triggered,
                m_updateManager, &UpdateManager::checkForUpdates);
    }
    QAction *quitAction = m_trayMenu->addAction(QStringLiteral("退出"));
    connect(quitAction, &QAction::triggered, this, [this]() {
        m_quitting = true;
        if (m_trayIcon) {
            m_trayIcon->hide();
        }
        QApplication::quit();
    });
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!m_quitting && m_trayIcon && m_trayIcon->isVisible()) {
        hide();
        showRuntimeStatusBar();
        event->ignore();
        if (!m_trayHintShown) {
            m_trayHintShown = true;
            m_trayIcon->showMessage(
                QStringLiteral("Aegisy 仍在运行"),
                QStringLiteral("可从系统托盘快速切换档案或彻底退出。"),
                QSystemTrayIcon::Information,
                3500);
        }
        return;
    }
    hideRuntimeStatusBar();
    QMainWindow::closeEvent(event);
}

void MainWindow::showEvent(QShowEvent *event)
{
    // 主窗口重新可见（托盘/小球还原或最小化恢复）时收起小球。
    hideRuntimeStatusBar();
    scheduleConfigurationRefresh();
    QMainWindow::showEvent(event);
}

void MainWindow::setupConfigurationWatcher()
{
    m_configurationWatcher = new QFileSystemWatcher(this);
    m_configurationRefreshTimer = new QTimer(this);
    m_configurationRefreshTimer->setSingleShot(true);
    m_configurationRefreshTimer->setInterval(150);

    connect(m_configurationWatcher, &QFileSystemWatcher::fileChanged,
            this, [this](const QString &) { scheduleConfigurationRefresh(); });
    connect(m_configurationWatcher, &QFileSystemWatcher::directoryChanged,
            this, [this](const QString &) { scheduleConfigurationRefresh(); });
    connect(m_configurationRefreshTimer, &QTimer::timeout, this, [this]() {
        refreshConfigurationWatchers();
        rebuildCards();
        rebuildTrayMenu();
    });
    refreshConfigurationWatchers();
}

void MainWindow::refreshConfigurationWatchers()
{
    if (!m_configurationWatcher) return;

    QStringList desiredFiles;
    QStringList desiredDirectories;
    for (AiTool tool : { AiTool::ClaudeCode, AiTool::CodexCli,
                         AiTool::GeminiCli, AiTool::OpenCode }) {
        for (const QString &path : m_toolManager->configurationFiles(tool)) {
            const QFileInfo info(path);
            if (info.exists()) desiredFiles.append(info.absoluteFilePath());
            const QString directory = info.absolutePath();
            if (QDir(directory).exists()) desiredDirectories.append(directory);
        }
    }
    desiredFiles.removeDuplicates();
    desiredDirectories.removeDuplicates();

    const QStringList watchedFiles = m_configurationWatcher->files();
    for (const QString &path : desiredFiles) {
        if (!watchedFiles.contains(path)) m_configurationWatcher->addPath(path);
    }
    const QStringList watchedDirectories = m_configurationWatcher->directories();
    for (const QString &path : desiredDirectories) {
        if (!watchedDirectories.contains(path)) m_configurationWatcher->addPath(path);
    }
}

void MainWindow::scheduleConfigurationRefresh()
{
    if (m_configurationRefreshTimer) {
        m_configurationRefreshTimer->start();
    }
}

bool MainWindow::isProfileConfigurationReady(const Profile &profile,
                                             QString *issue) const
{
    const LocalConfigurationStatus status =
        m_toolManager->inspectConfiguration(profile.tool());
    if (!status.isReady()) {
        if (issue) *issue = status.detail;
        return false;
    }

    const bool gatewayExpected = QSettings().value(
        QStringLiteral("gateway/enabled"), false).toBool();
    if (status.gatewayMode != gatewayExpected) {
        if (issue) {
            *issue = gatewayExpected
                ? QStringLiteral("当前本地配置不是网关模式，请重新激活")
                : QStringLiteral("当前本地配置仍指向本地网关，请重新激活");
        }
        return false;
    }

    const QString expectedHint = gatewayExpected
        ? ProfileManager::maskedKeyHint(m_gatewayManager->localToken())
        : profile.keyHint;
    if (!expectedHint.isEmpty() && status.keyHint != expectedHint) {
        if (issue) *issue = QStringLiteral("磁盘认证与当前档案不一致");
        return false;
    }
    if (issue) issue->clear();
    return true;
}

void MainWindow::setAuthToken(const QString &token)
{
    m_authToken = token;
    m_companionAccountIdentity.clear();
    m_waitingForCompanionAccount = !token.isEmpty();
    if (m_websiteProjectionLabel) {
        m_websiteProjectionLabel->setState(
            QStringLiteral("网站配置 待验证"), StatusBadge::Tone::Neutral,
            style()->standardIcon(QStyle::SP_DriveNetIcon));
        m_websiteProjectionLabel->setToolTip(
            QStringLiteral("等待验证当前网站账号；不会显示其他账号的缓存"));
    }
    m_apiClient->setAuthToken(token);
    m_apiClient->getWorkbenchEmergencyPolicy();
    logMessage(QStringLiteral("正在验证网站账号并同步配置元数据..."), kLogInfo);
    refreshBalance();
    if (m_balanceRefreshTimer) {
        m_balanceRefreshTimer->start();
    }
    if (QSettings().value(QStringLiteral("gateway/enabled"), false).toBool()) {
        QTimer::singleShot(0, this, [this]() {
            if (!m_gatewayManager->start()) {
                QSettings().setValue(QStringLiteral("gateway/enabled"), false);
                logMessage(QStringLiteral("本地网关自动启动失败：%1")
                    .arg(m_gatewayManager->lastError()), kLogError);
                scheduleConfigurationRefresh();
            }
        });
    }
}

void MainWindow::refreshCachedWorkbenchEmergencyPolicy()
{
    QSettings settings;
    const auto decision = WorkbenchEmergencyPolicy::load(
        &settings, QByteArrayLiteral(AEGISY_UPDATE_PUBLIC_KEY),
        QDateTime::currentMSecsSinceEpoch());
    m_workbenchEmergencyDisabled = decision.blocksNewWork;
    m_workbenchEmergencyPolicyVerified =
        decision.state == WorkbenchEmergencyPolicy::State::Disabled;
    m_workbenchEmergencyReasonCode = decision.reasonCode.isEmpty()
        ? decision.errorCode : decision.reasonCode;
    if (m_agentWorkbench) {
        m_agentWorkbench->setEmergencyDisabled(
            m_workbenchEmergencyDisabled, m_workbenchEmergencyReasonCode,
            m_workbenchEmergencyPolicyVerified);
    }
}

void MainWindow::applyWorkbenchEmergencyPolicy(const QJsonObject &policy)
{
    QSettings settings;
    const auto result = WorkbenchEmergencyPolicy::install(
        &settings, policy, QByteArrayLiteral(AEGISY_UPDATE_PUBLIC_KEY),
        QDateTime::currentMSecsSinceEpoch());
    if (!result.accepted) {
        logMessage(QStringLiteral("工作台应急策略未通过签名、时效或序号校验"), kLogWarn);
    }
    refreshCachedWorkbenchEmergencyPolicy();
}

void MainWindow::refreshBalance()
{
    if (!m_authToken.isEmpty()) {
        m_apiClient->getUserInfo();
    }
}

void MainWindow::refreshToolVersions()
{
    if (m_pendingToolVersionChecks > 0) {
        return;
    }

    const QList<AiTool> tools = {
        AiTool::ClaudeCode,
        AiTool::CodexCli,
        AiTool::GeminiCli,
        AiTool::OpenCode,
    };
    m_pendingToolVersionChecks = tools.size();
    if (m_refreshToolVersionsButton) {
        m_refreshToolVersionsButton->setEnabled(false);
    }

    for (AiTool tool : tools) {
        const int id = static_cast<int>(tool);
        m_toolVersionTexts.insert(id, QStringLiteral("检测中..."));
        if (QLabel *label = m_toolVersionLabels.value(id, nullptr)) {
            label->setText(QStringLiteral("检测中..."));
            label->setToolTip(QString());
            label->setStyleSheet(QStringLiteral(
                "font-family: monospace; font-size: 10px; color: #98a2b3;"));
        }
        m_toolManager->detectVersion(tool);
    }
}

void MainWindow::onToolVersionDetected(AiTool tool, bool installed, const QString &version)
{
    const int id = static_cast<int>(tool);
    QString displayText;
    QString tooltip;
    QString color;
    if (!installed) {
        m_toolLocalVersions.remove(id);
        m_toolLatestVersions.remove(id);
        displayText = version.isEmpty() ? QStringLiteral("未安装")
                                        : QStringLiteral("需修复");
        tooltip = version.isEmpty()
            ? QStringLiteral("未检测到 %1").arg(ToolManager::toolName(tool))
            : QStringLiteral("检测到 npm 包 %1，但 CLI 命令缺失或无法运行")
                  .arg(version);
        color = QStringLiteral("#b54708");
    } else if (version.isEmpty()) {
        displayText = QStringLiteral("已安装");
        tooltip = QStringLiteral("已安装，但未能读取版本号");
        color = QStringLiteral("#067647");
    } else {
        m_toolLocalVersions.insert(id, version);
        displayText = QStringLiteral("v%1").arg(version);
        tooltip = QStringLiteral("%1 %2").arg(ToolManager::toolName(tool), version);
        color = QStringLiteral("#067647");
    }

    m_toolVersionTexts.insert(id, displayText);
    if (QLabel *label = m_toolVersionLabels.value(id, nullptr)) {
        label->setText(displayText);
        label->setToolTip(tooltip);
        label->setStyleSheet(QStringLiteral(
            "font-family: monospace; font-size: 10px; color: %1; font-weight: 600;")
            .arg(color));
    }
    if (QPushButton *button = m_toolInstallButtons.value(id, nullptr)) {
        button->setVisible(!installed);
        button->setEnabled(!m_installingTools.contains(id));
        button->setText(m_installingTools.contains(id)
            ? QStringLiteral("...")
            : (version.isEmpty() ? QStringLiteral("安装")
                                 : QStringLiteral("修复")));
    }
    if (QPushButton *button = m_toolLaunchButtons.value(id, nullptr)) {
        button->setEnabled(installed);
        button->setToolTip(installed
            ? QStringLiteral("使用当前已激活档案启动 %1").arg(ToolManager::toolName(tool))
            : QStringLiteral("请先安装 %1").arg(ToolManager::toolName(tool)));
    }

    if (installed) {
        m_toolManager->checkLatestVersion(tool);
    }

    m_pendingToolVersionChecks = qMax(0, m_pendingToolVersionChecks - 1);
    if (m_pendingToolVersionChecks == 0) {
        if (m_refreshToolVersionsButton) {
            m_refreshToolVersionsButton->setEnabled(true);
        }
        rebuildCards();
    }
}

void MainWindow::onToolLatestVersionDetected(AiTool tool,
                                             bool success,
                                             const QString &latestVersion,
                                             const QString &error)
{
    const int id = static_cast<int>(tool);
    QLabel *label = m_toolVersionLabels.value(id, nullptr);
    if (!label || !success || latestVersion.isEmpty()) {
        if (label && !error.isEmpty()) {
            const QString original = label->toolTip();
            label->setToolTip(original.isEmpty()
                ? QStringLiteral("在线版本查询失败：%1").arg(error)
                : original + QStringLiteral("\n在线版本查询失败：%1").arg(error));
        }
        return;
    }

    m_toolLatestVersions.insert(id, latestVersion);
    const QString localVersion = m_toolLocalVersions.value(id);
    const bool updateAvailable = !localVersion.isEmpty()
        && QVersionNumber::compare(QVersionNumber::fromString(localVersion),
                                   QVersionNumber::fromString(latestVersion)) < 0;
    if (updateAvailable) {
        label->setText(QStringLiteral("v%1 ↑").arg(localVersion));
        label->setStyleSheet(QStringLiteral(
            "font-family: monospace; font-size: 10px; color: #b54708; font-weight: 700;"));
        label->setToolTip(QStringLiteral("当前 %1，最新 %2，可在系统体检中更新")
            .arg(localVersion, latestVersion));
    } else {
        label->setToolTip(QStringLiteral("当前 %1，已是最新版本").arg(localVersion));
    }
}

QString MainWindow::maskKey(const QString &key)
{
    if (key.isEmpty()) {
        return QString();
    }
    if (key.length() <= 8) {
        return QStringLiteral("********");
    }
    return key.left(5) + QStringLiteral("••••") + key.right(4);
}

void MainWindow::logMessage(const QString &message, const QString &color)
{
    m_logOutput->append(QStringLiteral("<span style='color:%1'>%2</span>")
                            .arg(color, message.toHtmlEscaped()));
}

void MainWindow::setupUi()
{
    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("appRoot"));
    setCentralWidget(central);
    central->setStyleSheet(QStringLiteral(
        "QWidget#appRoot { background: #f4f7f9; }"
        "QLabel { color: #182230; }"
        "QToolTip { background: #182230; color: white; border: none; padding: 5px; }"));

    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *topBar = new QFrame(central);
    topBar->setObjectName(QStringLiteral("topBar"));
    topBar->setFixedHeight(64);
    topBar->setStyleSheet(QStringLiteral(
        "QFrame#topBar { background: #ffffff; border-bottom: 1px solid #e4e7ec; }"));
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(20, 10, 20, 10);
    topLayout->setSpacing(10);

    auto *brandMark = new QLabel(QStringLiteral("A"), topBar);
    brandMark->setFixedSize(38, 38);
    brandMark->setAlignment(Qt::AlignCenter);
    brandMark->setStyleSheet(QStringLiteral(
        "background: #101828; color: #ffffff; border-radius: 8px;"
        "font-size: 18px; font-weight: 700;"));
    topLayout->addWidget(brandMark);

    auto *brandColumn = new QVBoxLayout;
    brandColumn->setSpacing(0);
    auto *brandName = new QLabel(QStringLiteral("Aegisy"), topBar);
    brandName->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700; color: #101828;"));
    auto *brandSection = new QLabel(QStringLiteral("连接管理"), topBar);
    brandSection->setStyleSheet(QStringLiteral("font-size: 11px; color: #667085;"));
    brandColumn->addWidget(brandName);
    brandColumn->addWidget(brandSection);
    topLayout->addLayout(brandColumn);
    topLayout->addStretch();

    m_userLabel = new QPushButton(QStringLiteral("U"), topBar);
    m_userLabel->setFixedSize(38, 38);
    m_userLabel->setCursor(Qt::PointingHandCursor);
    m_userLabel->setToolTip(QStringLiteral("账号中心：修改密码与密卡充值"));
    m_userLabel->setStyleSheet(QStringLiteral(
        "QPushButton { color: #165DFF; background: #EEF4FF; border: 1px solid #C8D8FF;"
        "border-radius: 19px; font-size: 14px; font-weight: 700; }"
        "QPushButton:hover { background: #dbeeff; border-color: #84caff; }"));
    topLayout->addWidget(m_userLabel);

    m_balanceButton = new QPushButton(QStringLiteral("余额  --"), topBar);
    m_balanceButton->setToolTip(QStringLiteral("查看账号与 API Key 用量"));
    m_balanceButton->setFixedHeight(36);
    m_balanceButton->setMinimumWidth(112);
    m_balanceButton->setMaximumWidth(150);
    m_balanceButton->setCursor(Qt::PointingHandCursor);
    m_balanceButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #ffffff; color: #344054; border: 1px solid #d0d5dd;"
        "border-radius: 7px; padding: 0 10px; font-size: 11px; font-weight: 700; }"
        "QPushButton:hover { background: #f8fafc; border-color: #98a2b3; }"));
    topLayout->addWidget(m_balanceButton);

    // ── 顶栏功能按钮（用 QStyle 系统图标 + 纯文字，Windows/macOS/Linux 全兼容）──
    m_manageKeysButton = new QPushButton(QStringLiteral("API Keys"), topBar);
    m_manageKeysButton->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    m_manageKeysButton->setToolTip(QStringLiteral("管理账号 API Keys"));
    m_viewModelsButton = new QPushButton(QStringLiteral("模型"), topBar);
    m_viewModelsButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogListView));
    m_viewModelsButton->setToolTip(QStringLiteral("浏览当前 Key 支持的模型列表"));
    m_imageGenerationButton = new QPushButton(QStringLiteral("生图"), topBar);
    m_imageGenerationButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));
    m_imageGenerationButton->setToolTip(QStringLiteral("GPT Image 文生图"));
    m_backupsButton = new QPushButton(QStringLiteral("备份"), topBar);
    m_backupsButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_backupsButton->setToolTip(QStringLiteral("查看并恢复本地配置备份"));
    m_transferButton = new QPushButton(QStringLiteral("迁移"), topBar);
    m_transferButton->setIcon(style()->standardIcon(QStyle::SP_DirLinkIcon));
    m_transferButton->setToolTip(QStringLiteral("导入 / 导出加密档案"));
    m_checkUpdatesButton = new QPushButton(QStringLiteral("更新"), topBar);
    m_checkUpdatesButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_checkUpdatesButton->setToolTip(QStringLiteral("检查应用更新"));

    auto *updatesMenu = new QMenu(m_checkUpdatesButton);
    m_checkUpdatesAction = updatesMenu->addAction(
        style()->standardIcon(QStyle::SP_BrowserReload), QStringLiteral("检查更新"));
    m_autoUpdateChecksAction = updatesMenu->addAction(QStringLiteral("自动检查更新"));
    m_autoUpdateChecksAction->setCheckable(true);
    updatesMenu->addSeparator();
    QAction *currentVersionAction = updatesMenu->addAction(
        QStringLiteral("当前版本  v%1").arg(QApplication::applicationVersion()));
    currentVersionAction->setEnabled(false);
    m_checkUpdatesButton->setMenu(updatesMenu);

    for (QPushButton *button : {
             m_manageKeysButton, m_viewModelsButton, m_imageGenerationButton,
             m_backupsButton, m_transferButton, m_checkUpdatesButton }) {
        button->setCursor(Qt::PointingHandCursor);
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    m_logoutButton = new QPushButton(topBar);
    m_logoutButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    m_logoutButton->setIconSize(QSize(16, 16));
    m_logoutButton->setToolTip(QStringLiteral("退出登录"));
    m_logoutButton->setFixedSize(32, 32);
    m_logoutButton->setCursor(Qt::PointingHandCursor);
    m_logoutButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: transparent; color: #667085; border: 1px solid #d0d5dd;"
        "  border-radius: 7px; font-size: 16px;"
        "}"
        "QPushButton:hover { background: #3a1f25; color: #fecaca; border-color: #7f3540; }"
        "QPushButton:pressed { background: #4b2029; }"));
    topLayout->addWidget(m_logoutButton);
    root->addWidget(topBar);

    auto *body = new QWidget(central);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    auto *sidebar = new QFrame(body);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setMinimumWidth(214);
    sidebar->setStyleSheet(QStringLiteral(
        "QFrame#sidebar { background: #ffffff; border-right: 1px solid #e4e7ec; }"));
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(14, 18, 14, 18);
    sideLayout->setSpacing(6);

    auto *navLabel = new QLabel(QStringLiteral("工作区"), sidebar);
    navLabel->setStyleSheet(QStringLiteral(
        "font-size: 10px; font-weight: 700; color: #98a2b3; padding: 0 10px 4px 10px;"));
    sideLayout->addWidget(navLabel);

    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);
    const struct {
        int page;
        const char *label;
        QStyle::StandardPixmap icon;
    } navItems[] = {
        { 0, "配置中心", QStyle::SP_ComputerIcon },
        { 1, "桌面增强", QStyle::SP_DesktopIcon },
        { 2, "接入配置", QStyle::SP_DirLinkIcon },
        { 3, "本地网关", QStyle::SP_DriveNetIcon },
        { 4, "插件与 Skills", QStyle::SP_FileDialogDetailedView },
        { 5, "Codex 编程", QStyle::SP_CommandLink },
    };
    for (const auto &item : navItems) {
        auto *button = new QPushButton(QString::fromUtf8(item.label), sidebar);
        button->setIcon(style()->standardIcon(item.icon));
        button->setCheckable(true);
        button->setChecked(item.page == 0);
        button->setFixedHeight(42);
        button->setCursor(Qt::PointingHandCursor);
        if (item.page == 5) {
            button->setToolTip(QStringLiteral("仅使用已验证的 Codex 运行时；Claude / Gemini 编程适配暂缓"));
        }
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; color: #344054; border: none;"
            "border-radius: 7px; text-align: left; padding: 0 12px; font-size: 13px; }"
            "QPushButton:hover { background: #f2f4f7; color: #101828; }"
            "QPushButton:checked { background: #EEF4FF; color: #165DFF;"
            "font-weight: 700; border-right: 3px solid #4B7DFF; }"));
        m_navGroup->addButton(button, item.page);
        sideLayout->addWidget(button);
    }
    sideLayout->addSpacing(12);

    auto *filterLabel = new QLabel(QStringLiteral("配置筛选"), sidebar);
    filterLabel->setStyleSheet(QStringLiteral(
        "font-size: 10px; font-weight: 700; color: #98a2b3; letter-spacing: 0px;"
        "padding: 0 10px 4px 10px; text-transform: uppercase;"));
    filterLabel->hide();

    auto *filterStrip = new QWidget(body);
    auto *filterStripLayout = new QHBoxLayout(filterStrip);
    filterStripLayout->setContentsMargins(0, 0, 0, 0);
    filterStripLayout->setSpacing(6);

    m_filterGroup = new QButtonGroup(this);
    m_filterGroup->setExclusive(true);

    // 每个工具对应的彩色指示点 + 显示名
    const struct {
        int id;
        const char *label;
        const char *dotColor;   // 工具品牌色
    } filters[] = {
        { 0,                                    "全部配置",   "#165DFF" },
        { static_cast<int>(ProfileType::Claude),   "Claude Code", "#c15f3c" },
        { static_cast<int>(ProfileType::Codex),    "Codex CLI",   "#6366f1" },
        { static_cast<int>(ProfileType::Gemini),   "Gemini CLI",  "#165DFF" },
        { static_cast<int>(ProfileType::OpenCode), "OpenCode",    "#059669" },
    };

    for (const auto &filter : filters) {
        // 彩色圆点 + 文字组合，避免依赖系统图标
        const QString dotHtml = QStringLiteral(
            "<span style='color:%1; font-size:16px;'>●</span>  %2")
            .arg(QString::fromUtf8(filter.dotColor),
                 QString::fromUtf8(filter.label));
        auto *button = new QPushButton(sidebar);
        button->setText(QString::fromUtf8(filter.label));
        button->setCheckable(true);
        button->setChecked(filter.id == 0);
        button->setFixedHeight(34);
        button->setCursor(Qt::PointingHandCursor);
        // 用 icon() 放彩色指示点：改用 padding-left 让文字左对齐，在前面手动加色块
        button->setStyleSheet(QStringLiteral(
            "QPushButton {"
            "  background: #ffffff; color: #475467; border: 1px solid #d0d5dd; border-radius: 7px;"
            "  text-align: center; padding: 0 12px; font-size: 12px; font-weight: 500;"
            "}"
            "QPushButton:hover { background: #f8fafc; color: #101828; }"
            "QPushButton:checked {"
            "  background: #EEF4FF; color: #165DFF; font-weight: 700;"
            "  border: 1px solid #84caff;"
            "}"));
        m_filterGroup->addButton(button, filter.id);
        filterStripLayout->addWidget(button);
    }
    filterStripLayout->addStretch();

    auto *terminalPanel = new QFrame(body);
    terminalPanel->setObjectName(QStringLiteral("runtimePanel"));
    terminalPanel->setStyleSheet(QStringLiteral(
        "QFrame#runtimePanel { background: #ffffff; border: 1px solid #e4e7ec; border-radius: 8px; }"));
    auto *terminalPanelLayout = new QVBoxLayout(terminalPanel);
    terminalPanelLayout->setContentsMargins(16, 14, 16, 14);
    terminalPanelLayout->setSpacing(8);

    auto *terminalHeader = new QHBoxLayout;
    terminalHeader->setContentsMargins(8, 0, 2, 0);
    auto *terminalTitle = new QLabel(QStringLiteral("本地终端"), sidebar);
    terminalTitle->setText(QStringLiteral("CLI 运行环境"));
    terminalTitle->setStyleSheet(QStringLiteral(
        "font-size: 13px; font-weight: 700; color: #344054;"));
    terminalHeader->addWidget(terminalTitle);
    terminalHeader->addStretch();
    m_refreshToolVersionsButton = new QPushButton(sidebar);
    m_refreshToolVersionsButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_refreshToolVersionsButton->setToolTip(QStringLiteral("刷新本地终端版本"));
    m_refreshToolVersionsButton->setFixedSize(26, 26);
    m_refreshToolVersionsButton->setCursor(Qt::PointingHandCursor);
    m_refreshToolVersionsButton->setStyleSheet(AppTheme::iconButtonStyle());
    terminalHeader->addWidget(m_refreshToolVersionsButton);
    terminalPanelLayout->addLayout(terminalHeader);

    const struct {
        AiTool tool;
        const char *name;
    } terminals[] = {
        { AiTool::ClaudeCode, "Claude" },
        { AiTool::CodexCli, "Codex" },
        { AiTool::GeminiCli, "Gemini" },
        { AiTool::OpenCode, "OpenCode" },
    };
    for (const auto &terminal : terminals) {
        auto *row = new QFrame(terminalPanel);
        row->setStyleSheet(QStringLiteral(
            "QFrame { background: #f8fafc; border: 1px solid #eaecf0; border-radius: 7px; }"));
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 2, 4, 2);
        rowLayout->setSpacing(6);
        auto *name = new QLabel(QString::fromUtf8(terminal.name), row);
        name->setStyleSheet(QStringLiteral("font-size: 12px; color: #344054; font-weight: 600; border: none;"));
        rowLayout->addWidget(name);
        rowLayout->addStretch();
        auto *version = new QLabel(QStringLiteral("检测中..."), row);
        version->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        version->setStyleSheet(QStringLiteral(
            "font-family: monospace; font-size: 10px; color: #667085; border: none;"));
        rowLayout->addWidget(version);
        m_toolVersionLabels.insert(static_cast<int>(terminal.tool), version);
        m_toolVersionTexts.insert(static_cast<int>(terminal.tool), QStringLiteral("检测中..."));

        auto *installButton = new QPushButton(QStringLiteral("安装"), row);
        installButton->setFixedSize(44, 26);
        installButton->setCursor(Qt::PointingHandCursor);
        installButton->setToolTip(
            QStringLiteral("一键安装 %1 运行环境").arg(ToolManager::toolName(terminal.tool)));
        installButton->setStyleSheet(QStringLiteral(
            "QPushButton { background: #fff7ed; color: #b54708; border: 1px solid #fed7aa;"
            "border-radius: 6px; font-size: 10px; font-weight: 600; }"
            "QPushButton:hover { background: #ffedd5; border-color: #fdba74; }"
            "QPushButton:disabled { color: #657382; background: #202933; border-color: #303b47; }"));
        installButton->hide();
        rowLayout->addWidget(installButton);
        m_toolInstallButtons.insert(static_cast<int>(terminal.tool), installButton);
        connect(installButton, &QPushButton::clicked, this,
                [this, tool = terminal.tool]() { installToolEnvironment(tool); });
        terminalPanelLayout->addWidget(row);
    }

    // ── 功能入口（QStyle 系统图标 + 文字，Windows/macOS/Linux 全兼容）──
    auto *toolsHeader = new QLabel(QStringLiteral("工作区"), sidebar);
    toolsHeader->setStyleSheet(QStringLiteral(
        "font-size: 10px; font-weight: 700; color: #768696; letter-spacing: 0px;"
        "padding: 6px 10px 2px 10px;"));
    sideLayout->addWidget(toolsHeader);

    const QString sideNavStyle = AppTheme::sideNavButtonStyle();

    m_chatButton = new QPushButton(QStringLiteral("AI 对话"), sidebar);
    m_chatButton->setIcon(style()->standardIcon(QStyle::SP_MessageBoxInformation));
    m_chatButton->setToolTip(QStringLiteral("选择 API Key 和模型进行流式对话"));
    m_chatButton->setFixedHeight(38);
    m_chatButton->setCursor(Qt::PointingHandCursor);
    m_chatButton->setStyleSheet(sideNavStyle);
    sideLayout->addWidget(m_chatButton);

    for (QPushButton *button : {
             m_imageGenerationButton, m_viewModelsButton, m_manageKeysButton }) {
        button->setFixedHeight(38);
        button->setStyleSheet(sideNavStyle);
        sideLayout->addWidget(button);
    }

    m_skillsButton = new QPushButton(QStringLiteral("Skills"), sidebar);
    m_skillsButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    m_skillsButton->setToolTip(QStringLiteral("安装、启用和管理对话自动调用的 Skills"));
    m_skillsButton->setFixedHeight(38);
    m_skillsButton->setCursor(Qt::PointingHandCursor);
    m_skillsButton->setStyleSheet(sideNavStyle);
    sideLayout->addWidget(m_skillsButton);

    m_mcpConfigButton = new QPushButton(QStringLiteral("MCP 配置"), sidebar);
    m_mcpConfigButton->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    m_mcpConfigButton->setToolTip(
        QStringLiteral("管理 ~/.claude/settings.json 中的 mcpServers 共享配置\n（切换档案时自动保留）"));
    m_mcpConfigButton->setFixedHeight(38);
    m_mcpConfigButton->setCursor(Qt::PointingHandCursor);
    m_mcpConfigButton->setStyleSheet(sideNavStyle);
    sideLayout->addWidget(m_mcpConfigButton);

    auto *dataHeader = new QLabel(QStringLiteral("配置与数据"), sidebar);
    dataHeader->setStyleSheet(toolsHeader->styleSheet());
    sideLayout->addWidget(dataHeader);
    for (QPushButton *button : { m_backupsButton, m_transferButton }) {
        button->setFixedHeight(38);
        button->setStyleSheet(sideNavStyle);
        sideLayout->addWidget(button);
    }

    auto *helpButton = new QPushButton(QStringLiteral("使用文档"), sidebar);
    helpButton->setIcon(style()->standardIcon(QStyle::SP_DialogHelpButton));
    helpButton->setToolTip(QStringLiteral("查看 Aegisy 使用说明文档"));
    helpButton->setFixedHeight(38);
    helpButton->setCursor(Qt::PointingHandCursor);
    helpButton->setStyleSheet(sideNavStyle);
    connect(helpButton, &QPushButton::clicked, this, &MainWindow::onHelpClicked);
    auto *systemHeader = new QLabel(QStringLiteral("系统"), sidebar);
    systemHeader->setStyleSheet(toolsHeader->styleSheet());
    sideLayout->addWidget(systemHeader);

    m_doctorButton = new QPushButton(QStringLiteral("系统体检"), sidebar);
    m_doctorButton->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    m_doctorButton->setToolTip(QStringLiteral("检查系统依赖、CLI、配置与安全存储"));
    m_doctorButton->setFixedHeight(38);
    m_doctorButton->setCursor(Qt::PointingHandCursor);
    m_doctorButton->setStyleSheet(sideNavStyle);
    sideLayout->addWidget(m_doctorButton);

    m_gatewayButton = new QPushButton(QStringLiteral("本地网关"), sidebar);
    m_gatewayButton->setIcon(style()->standardIcon(QStyle::SP_DriveNetIcon));
    m_gatewayButton->setToolTip(QStringLiteral("本地转发、快速档案切换与请求监控"));
    m_gatewayButton->setFixedHeight(38);
    m_gatewayButton->setCursor(Qt::PointingHandCursor);
    m_gatewayButton->setStyleSheet(sideNavStyle);
    sideLayout->addWidget(m_gatewayButton);

    m_desktopEnhancementsButton = new QPushButton(QStringLiteral("桌面增强"), sidebar);
    m_desktopEnhancementsButton->setIcon(style()->standardIcon(QStyle::SP_DesktopIcon));
    m_desktopEnhancementsButton->setToolTip(
        QStringLiteral("全量模型、Codex 插件、历史会话、Computer Use 与 Claude 汉化"));
    m_desktopEnhancementsButton->setFixedHeight(38);
    m_desktopEnhancementsButton->setCursor(Qt::PointingHandCursor);
    m_desktopEnhancementsButton->setStyleSheet(sideNavStyle);
    sideLayout->addWidget(m_desktopEnhancementsButton);

    m_desktopDownloadButton = new QPushButton(QStringLiteral("下载桌面端"), sidebar);
    m_desktopDownloadButton->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    m_desktopDownloadButton->setToolTip(
        QStringLiteral("下载并安装 Claude / ChatGPT 桌面客户端"));
    m_desktopDownloadButton->setFixedHeight(38);
    m_desktopDownloadButton->setCursor(Qt::PointingHandCursor);
    m_desktopDownloadButton->setStyleSheet(sideNavStyle);
    sideLayout->addWidget(m_desktopDownloadButton);

    m_checkUpdatesButton->setFixedHeight(38);
    m_checkUpdatesButton->setStyleSheet(sideNavStyle);
    sideLayout->addWidget(m_checkUpdatesButton);
    sideLayout->addWidget(helpButton);

    sideLayout->addStretch();

    auto *sidebarScroll = new QScrollArea(body);
    sidebarScroll->setObjectName(QStringLiteral("sidebarScroll"));
    sidebarScroll->setWidgetResizable(true);
    sidebarScroll->setFixedWidth(230);
    sidebarScroll->setFrameShape(QFrame::NoFrame);
    sidebarScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidebarScroll->setStyleSheet(QStringLiteral(
        "QScrollArea#sidebarScroll { background: #141c24; border: none; }"
        "QScrollBar:vertical { background: #141c24; width: 7px; }"
        "QScrollBar::handle:vertical { background: #3a4855; border-radius: 3px; min-height: 30px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"));
    sidebarScroll->setWidget(sidebar);
    bodyLayout->addWidget(sidebarScroll);

    auto *content = new QWidget(body);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(26, 22, 26, 20);
    contentLayout->setSpacing(12);

    auto *headingRow = new QHBoxLayout;
    auto *headingColumn = new QVBoxLayout;
    headingColumn->setSpacing(2);
    auto *pageTitle = new QLabel(QStringLiteral("连接配置"), content);
    pageTitle->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 700; color: #101828;"));
    auto *pageSubtitle = new QLabel(
        QStringLiteral("每个档案只对应一个终端，切换时仅更新对应的本地认证"), content);
    pageSubtitle->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    headingColumn->addWidget(pageTitle);
    headingColumn->addWidget(pageSubtitle);
    headingRow->addLayout(headingColumn);
    headingRow->addStretch();

    m_bulkSwitchButton = new QPushButton(QStringLiteral("全部切换"), content);
    m_bulkSwitchButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_bulkSwitchButton->setToolTip(QStringLiteral("为四个工具分别选择已有档案，一次性激活"));
    m_bulkSwitchButton->setFixedHeight(40);
    m_bulkSwitchButton->setCursor(Qt::PointingHandCursor);
    m_bulkSwitchButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    headingRow->addWidget(m_bulkSwitchButton);

    m_newConnectButton = new QPushButton(QStringLiteral("新建配置"), content);
    m_newConnectButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    m_newConnectButton->setFixedHeight(40);
    m_newConnectButton->setCursor(Qt::PointingHandCursor);
    m_newConnectButton->setStyleSheet(AppTheme::primaryButtonStyle());
    headingRow->addWidget(m_newConnectButton);
    contentLayout->addLayout(headingRow);
    contentLayout->addWidget(filterStrip);

    auto *summaryRow = new QHBoxLayout;
    summaryRow->setSpacing(8);
    m_profileCountLabel = new StatusBadge(content);
    m_profileCountLabel->setState(
        QStringLiteral("0 个配置"), StatusBadge::Tone::Neutral,
        style()->standardIcon(QStyle::SP_FileDialogListView));
    summaryRow->addWidget(m_profileCountLabel);
    m_activeProfileLabel = new StatusBadge(content);
    m_activeProfileLabel->setState(
        QStringLiteral("未激活"), StatusBadge::Tone::Neutral,
        style()->standardIcon(QStyle::SP_MessageBoxInformation));
    summaryRow->addWidget(m_activeProfileLabel);
    m_websiteProjectionLabel = new StatusBadge(content);
    m_websiteProjectionLabel->setState(
        QStringLiteral("网站配置 未同步"), StatusBadge::Tone::Neutral,
        style()->standardIcon(QStyle::SP_DriveNetIcon));
    summaryRow->addWidget(m_websiteProjectionLabel);
    summaryRow->addStretch();
    contentLayout->addLayout(summaryRow);

    m_cardsScroll = new QScrollArea(content);
    m_cardsScroll->setWidgetResizable(true);
    m_cardsScroll->setFrameShape(QFrame::NoFrame);
    m_cardsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_cardsScroll->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
        "QScrollBar::handle:vertical { background: #cfd4dc; border-radius: 4px; min-height: 28px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"));

    m_cardsContainer = new QWidget(m_cardsScroll);
    m_cardsContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    m_cardsLayout = new QVBoxLayout(m_cardsContainer);
    m_cardsLayout->setContentsMargins(0, 2, 4, 4);
    m_cardsLayout->setSpacing(10);
    m_cardsLayout->setAlignment(Qt::AlignTop);
    m_cardsScroll->setWidget(m_cardsContainer);
    contentLayout->addWidget(m_cardsScroll, 1);

    auto *activityHeader = new QHBoxLayout;
    auto *activityTitle = new QLabel(QStringLiteral("活动记录"), content);
    activityTitle->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 700; color: #344054;"));
    activityHeader->addWidget(activityTitle);
    activityHeader->addStretch();
    auto *clearLogButton = new QPushButton(content);
    clearLogButton->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    clearLogButton->setToolTip(QStringLiteral("清空活动记录"));
    clearLogButton->setFixedSize(30, 30);
    clearLogButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; border-radius: 6px; }"
        "QPushButton:hover { background: #eaecf0; }"));
    activityHeader->addWidget(clearLogButton);

    m_logOutput = new QTextEdit(content);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMinimumHeight(88);
    m_logOutput->setMaximumHeight(112);
    m_logOutput->setStyleSheet(QStringLiteral(
        "QTextEdit {"
        "  background: white; color: #475467; border: 1px solid #e4e7ec;"
        "  border-radius: 8px; padding: 8px 10px; font-family: monospace; font-size: 11px;"
        "}"
        "QScrollBar:vertical { background: transparent; width: 7px; }"
        "QScrollBar::handle:vertical { background: #d0d5dd; border-radius: 3px; }"));

    m_workspaceStack = new QStackedWidget(body);
    m_workspaceStack->setObjectName(QStringLiteral("workspaceStack"));
    m_workspaceStack->setStyleSheet(QStringLiteral(
        "QStackedWidget#workspaceStack { background: #f5f7fb; border: none; }"));

    auto makePage = [body](const QString &objectName) {
        auto *scroll = new QScrollArea(body);
        scroll->setObjectName(objectName);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet(QStringLiteral("QScrollArea { background: #f5f7fb; border: none; }"));
        auto *page = new QWidget(scroll);
        page->setObjectName(objectName + QStringLiteral("Content"));
        page->setStyleSheet(QStringLiteral(
            "QWidget#%1 { background: #f5f7fb; }"
            "QWidget#%1 QLabel { background: transparent; }").arg(page->objectName()));
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(26, 22, 26, 24);
        layout->setSpacing(14);
        layout->setAlignment(Qt::AlignTop);
        scroll->setWidget(page);
        return qMakePair(scroll, layout);
    };

    auto makeHeader = [this](QWidget *parent, const QString &titleText,
                             const QString &subtitleText) {
        auto *frame = new QFrame(parent);
        frame->setObjectName(QStringLiteral("pageHeader"));
        frame->setStyleSheet(QStringLiteral(
            "QFrame#pageHeader { background: #ffffff; border: 1px solid #e4e7ec; border-radius: 8px; }"));
        auto *layout = new QHBoxLayout(frame);
        layout->setContentsMargins(20, 16, 20, 16);
        layout->setSpacing(12);
        auto *column = new QVBoxLayout;
        column->setSpacing(3);
        auto *title = new QLabel(titleText, frame);
        title->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 700; color: #101828;"));
        auto *subtitle = new QLabel(subtitleText, frame);
        subtitle->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
        subtitle->setWordWrap(true);
        column->addWidget(title);
        column->addWidget(subtitle);
        layout->addLayout(column, 1);
        return qMakePair(frame, layout);
    };

    // 启动台：高频状态与操作集中在首屏。
    const auto dashboardPage = makePage(QStringLiteral("launchpadPage"));
    const auto dashboardHeader = makeHeader(
        dashboardPage.first->widget(), QStringLiteral("Aegisy 配置中心"),
        QStringLiteral("一键连接网站账号，配置本地 CLI，并管理插件、中文体验与 Skills"));
    auto *dashboardRefresh = new QPushButton(QStringLiteral("刷新"), dashboardHeader.first);
    dashboardRefresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    dashboardRefresh->setStyleSheet(AppTheme::secondaryButtonStyle());
    m_refreshToolVersionsButton = dashboardRefresh;
    dashboardHeader.second->addWidget(dashboardRefresh);
    dashboardPage.second->addWidget(dashboardHeader.first);

    auto *toolGrid = new QGridLayout;
    toolGrid->setHorizontalSpacing(12);
    toolGrid->setVerticalSpacing(12);
    const struct { AiTool tool; const char *accent; const char *letter; } toolCards[] = {
        { AiTool::CodexCli, "#111827", "O" },
        { AiTool::ClaudeCode, "#c15f3c", "C" },
        { AiTool::GeminiCli, "#165DFF", "G" },
        { AiTool::OpenCode, "#059669", "OC" },
    };
    for (int index = 0; index < 4; ++index) {
        const auto item = toolCards[index];
        auto *card = new QFrame(dashboardPage.first->widget());
        card->setObjectName(QStringLiteral("toolLaunchCard"));
        card->setMinimumHeight(142);
        card->setStyleSheet(QStringLiteral(
            "QFrame#toolLaunchCard { background: #ffffff; border: 1px solid #e4e7ec; border-radius: 8px; }"));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(16, 14, 16, 14);
        cardLayout->setSpacing(9);
        auto *top = new QHBoxLayout;
        auto *badge = new QLabel(QString::fromLatin1(item.letter), card);
        badge->setFixedSize(40, 40);
        badge->setAlignment(Qt::AlignCenter);
        badge->setStyleSheet(QStringLiteral(
            "background:%1; color:white; border-radius:8px; font-size:15px; font-weight:700;")
            .arg(QString::fromLatin1(item.accent)));
        top->addWidget(badge);
        auto *copy = new QVBoxLayout;
        copy->setSpacing(2);
        auto *name = new QLabel(ToolManager::toolName(item.tool), card);
        name->setStyleSheet(QStringLiteral("font-size:14px; font-weight:700; color:#101828;"));
        auto *detail = new QLabel(QStringLiteral("检测中..."), card);
        detail->setStyleSheet(QStringLiteral("font-family:monospace; font-size:11px; color:#667085;"));
        copy->addWidget(name);
        copy->addWidget(detail);
        top->addLayout(copy, 1);
        cardLayout->addLayout(top);
        m_toolVersionLabels.insert(static_cast<int>(item.tool), detail);

        auto *actions = new QHBoxLayout;
        actions->setSpacing(7);
        auto *install = new QPushButton(QStringLiteral("安装"), card);
        install->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
        install->setStyleSheet(AppTheme::primaryButtonStyle());
        install->hide();
        m_toolInstallButtons.insert(static_cast<int>(item.tool), install);
        auto *launch = new QPushButton(QStringLiteral("启动"), card);
        launch->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
        launch->setStyleSheet(AppTheme::secondaryButtonStyle());
        launch->setEnabled(false);
        m_toolLaunchButtons.insert(static_cast<int>(item.tool), launch);
        auto *configure = new QPushButton(QStringLiteral("配置"), card);
        configure->setStyleSheet(AppTheme::secondaryButtonStyle());
        actions->addStretch();
        actions->addWidget(install);
        actions->addWidget(launch);
        actions->addWidget(configure);
        cardLayout->addLayout(actions);
        connect(install, &QPushButton::clicked, this,
                [this, tool = item.tool]() { installToolEnvironment(tool); });
        connect(launch, &QPushButton::clicked, this, [this, tool = item.tool]() {
            const QList<Profile> profiles = m_profileManager->allProfiles();
            const ProfileType type = profileTypeForTool(tool);
            const int profileIndex = m_profileManager->activeIndex(type);
            if (profileIndex >= 0 && profileIndex < profiles.size()) {
                launchProfile(profileIndex);
            } else {
                switchWorkspacePage(2);
                if (QAbstractButton *filter = m_filterGroup->button(static_cast<int>(type))) {
                    filter->setChecked(true);
                    onFilterChanged(static_cast<int>(type));
                }
            }
        });
        connect(configure, &QPushButton::clicked, this, [this, tool = item.tool]() {
            const ProfileType type = profileTypeForTool(tool);
            switchWorkspacePage(2);
            if (QAbstractButton *filter = m_filterGroup->button(static_cast<int>(type))) {
                filter->setChecked(true);
                onFilterChanged(static_cast<int>(type));
            }
        });
        toolGrid->addWidget(card, index / 2, index % 2);
    }
    dashboardPage.second->addLayout(toolGrid);

    auto *dashboardLower = new QHBoxLayout;
    dashboardLower->setSpacing(12);
    terminalPanel->deleteLater();
    auto *desktopSummary = new QFrame(dashboardPage.first->widget());
    desktopSummary->setObjectName(QStringLiteral("desktopSummary"));
    desktopSummary->setStyleSheet(QStringLiteral(
        "QFrame#desktopSummary { background:#ffffff; border:1px solid #e4e7ec; border-radius:8px; }"));
    auto *desktopSummaryLayout = new QVBoxLayout(desktopSummary);
    desktopSummaryLayout->setContentsMargins(16, 14, 16, 14);
    desktopSummaryLayout->setSpacing(10);
    auto *desktopTitle = new QLabel(QStringLiteral("桌面客户端"), desktopSummary);
    desktopTitle->setStyleSheet(QStringLiteral("font-size:13px; font-weight:700; color:#344054;"));
    desktopSummaryLayout->addWidget(desktopTitle);
    for (const auto &desktop : { qMakePair(AiTool::CodexCli, QStringLiteral("ChatGPT / Codex")),
                                 qMakePair(AiTool::ClaudeCode, QStringLiteral("Claude Desktop")) }) {
        auto *row = new QHBoxLayout;
        auto *name = new QLabel(desktop.second, desktopSummary);
        name->setStyleSheet(QStringLiteral("font-size:12px; color:#344054;"));
        row->addWidget(name);
        row->addStretch();
        const DesktopAppStatus state = m_toolManager->detectDesktop(desktop.first);
        auto *status = new QLabel(state.installed ? QStringLiteral("已安装") : QStringLiteral("缺失"), desktopSummary);
        status->setStyleSheet(state.installed
            ? QStringLiteral("color:#067647; font-size:11px; font-weight:700;")
            : QStringLiteral("color:#b54708; font-size:11px; font-weight:700;"));
        row->addWidget(status);
        desktopSummaryLayout->addLayout(row);
    }
    auto *manageDesktop = new QPushButton(QStringLiteral("管理桌面客户端"), desktopSummary);
    manageDesktop->setStyleSheet(AppTheme::secondaryButtonStyle());
    connect(manageDesktop, &QPushButton::clicked, this, [this]() { switchWorkspacePage(1); });
    desktopSummaryLayout->addWidget(manageDesktop);
    dashboardLower->addWidget(desktopSummary, 1);
    dashboardPage.second->addLayout(dashboardLower);

    auto *activityPanel = new QFrame(dashboardPage.first->widget());
    activityPanel->setObjectName(QStringLiteral("activityPanel"));
    activityPanel->setStyleSheet(QStringLiteral(
        "QFrame#activityPanel { background:#ffffff; border:1px solid #e4e7ec; border-radius:8px; }"));
    auto *activityPanelLayout = new QVBoxLayout(activityPanel);
    activityPanelLayout->setContentsMargins(16, 12, 16, 14);
    activityPanelLayout->addLayout(activityHeader);
    activityPanelLayout->addWidget(m_logOutput);
    dashboardPage.second->addWidget(activityPanel);

    // 桌面客户端页。
    const auto desktopPage = makePage(QStringLiteral("desktopClientsPage"));
    const auto desktopHeader = makeHeader(
        desktopPage.first->widget(), QStringLiteral("桌面增强"),
        QStringLiteral("检测和管理桌面客户端、Codex 插件与受支持的中文界面增强"));
    m_desktopEnhancementsButton->setText(QStringLiteral("桌面增强"));
    m_desktopEnhancementsButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    desktopHeader.second->addWidget(m_desktopEnhancementsButton);
    auto *desktopRefresh = new QPushButton(QStringLiteral("刷新"), desktopHeader.first);
    desktopRefresh->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    desktopRefresh->setStyleSheet(AppTheme::secondaryButtonStyle());
    desktopHeader.second->addWidget(desktopRefresh);
    desktopPage.second->addWidget(desktopHeader.first);

    auto addDesktopCard = [this, &desktopPage](AiTool tool, const QString &titleText,
                                               const QString &detailText) {
        auto *card = new QFrame(desktopPage.first->widget());
        card->setObjectName(QStringLiteral("desktopClientCard"));
        card->setStyleSheet(QStringLiteral(
            "QFrame#desktopClientCard { background:#ffffff; border:1px solid #e4e7ec; border-radius:8px; }"));
        auto *layout = new QHBoxLayout(card);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->setSpacing(14);
        auto *icon = new QLabel(tool == AiTool::CodexCli ? QStringLiteral("O") : QStringLiteral("C"), card);
        icon->setFixedSize(46, 46);
        icon->setAlignment(Qt::AlignCenter);
        icon->setStyleSheet(tool == AiTool::CodexCli
            ? QStringLiteral("background:#101828; color:white; border-radius:8px; font-size:18px; font-weight:700;")
            : QStringLiteral("background:#fff4ef; color:#c15f3c; border:1px solid #fed7c3; border-radius:8px; font-size:18px; font-weight:700;"));
        layout->addWidget(icon);
        auto *copy = new QVBoxLayout;
        copy->setSpacing(4);
        auto *title = new QLabel(titleText, card);
        title->setStyleSheet(QStringLiteral("font-size:16px; font-weight:700; color:#101828;"));
        auto *detail = new QLabel(detailText, card);
        detail->setWordWrap(true);
        detail->setStyleSheet(QStringLiteral("font-size:12px; color:#667085;"));
        auto *status = new QLabel(QStringLiteral("正在检测..."), card);
        status->setStyleSheet(QStringLiteral("font-size:12px; color:#667085;"));
        copy->addWidget(title);
        copy->addWidget(detail);
        copy->addWidget(status);
        layout->addLayout(copy, 1);
        auto *action = new QPushButton(QStringLiteral("下载并安装"), card);
        action->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
        action->setStyleSheet(AppTheme::primaryButtonStyle());
        layout->addWidget(action);
        if (tool == AiTool::CodexCli) {
            m_chatGptDesktopStatus = status;
            m_chatGptDesktopAction = action;
        } else {
            m_claudeDesktopStatus = status;
            m_claudeDesktopAction = action;
        }
        connect(action, &QPushButton::clicked, this, [this, tool]() {
            const DesktopAppStatus state = m_toolManager->detectDesktop(tool);
            const auto product = DesktopDownloader::proxiedProductForTool(tool);
            if (!product) {
                QDesktopServices::openUrl(QUrl(state.downloadUrl));
                return;
            }
            auto *downloader = new DesktopDownloader(
                *product, state.appName, m_authToken, m_apiClient->baseUrl(), this);
            downloader->exec();
            downloader->deleteLater();
            refreshDesktopPage();
        });
        desktopPage.second->addWidget(card);
    };
    addDesktopCard(AiTool::CodexCli, QStringLiteral("ChatGPT 桌面版（含 Codex）"),
                   QStringLiteral("通过 Aegisy 认证代理获取官方桌面安装程序。"));
    addDesktopCard(AiTool::ClaudeCode, QStringLiteral("Claude Desktop"),
                   QStringLiteral("支持官方安装包代理下载与运行时中文界面增强。"));
    auto *desktopNote = new QLabel(
        QStringLiteral("安装包完成平台签名与格式校验后才会保存到下载目录。"), desktopPage.first->widget());
    desktopNote->setWordWrap(true);
    desktopNote->setStyleSheet(QStringLiteral(
        "background:#EEF4FF; color:#165DFF; border:1px solid #C8D8FF; border-radius:8px; padding:11px 13px; font-size:12px;"));
    desktopPage.second->addWidget(desktopNote);

    // 本地网关页。
    const auto gatewayPage = makePage(QStringLiteral("gatewayPage"));
    const auto gatewayHeader = makeHeader(
        gatewayPage.first->widget(), QStringLiteral("本地网关"),
        QStringLiteral("管理本机转发状态、端点和不含提示词内容的请求记录"));
    m_gatewayStartButton = new QPushButton(QStringLiteral("启动"), gatewayHeader.first);
    m_gatewayStartButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_gatewayStartButton->setStyleSheet(AppTheme::primaryButtonStyle());
    m_gatewayRestartButton = new QPushButton(QStringLiteral("重启"), gatewayHeader.first);
    m_gatewayRestartButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_gatewayRestartButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    m_gatewayStopButton = new QPushButton(QStringLiteral("停止"), gatewayHeader.first);
    m_gatewayStopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    m_gatewayStopButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    m_gatewayButton->setText(QStringLiteral("详细监控"));
    m_gatewayButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    gatewayHeader.second->addWidget(m_gatewayStartButton);
    gatewayHeader.second->addWidget(m_gatewayRestartButton);
    gatewayHeader.second->addWidget(m_gatewayStopButton);
    gatewayHeader.second->addWidget(m_gatewayButton);
    gatewayPage.second->addWidget(gatewayHeader.first);

    auto *gatewayMetrics = new QFrame(gatewayPage.first->widget());
    gatewayMetrics->setObjectName(QStringLiteral("gatewayMetrics"));
    gatewayMetrics->setStyleSheet(QStringLiteral(
        "QFrame#gatewayMetrics { background:#ffffff; border:1px solid #e4e7ec; border-radius:8px; }"));
    auto *gatewayMetricsLayout = new QGridLayout(gatewayMetrics);
    gatewayMetricsLayout->setContentsMargins(18, 16, 18, 16);
    gatewayMetricsLayout->setSpacing(12);
    auto addMetric = [gatewayMetrics, gatewayMetricsLayout](int column, const QString &labelText) {
        auto *cell = new QFrame(gatewayMetrics);
        cell->setStyleSheet(QStringLiteral("QFrame { background:#f8fafc; border:1px solid #eaecf0; border-radius:7px; }"));
        auto *layout = new QVBoxLayout(cell);
        layout->setContentsMargins(12, 10, 12, 10);
        auto *label = new QLabel(labelText, cell);
        label->setStyleSheet(QStringLiteral("font-size:11px; color:#667085; border:none;"));
        auto *value = new QLabel(cell);
        value->setWordWrap(true);
        value->setStyleSheet(QStringLiteral("font-size:13px; font-weight:700; color:#101828; border:none;"));
        layout->addWidget(label);
        layout->addWidget(value);
        gatewayMetricsLayout->addWidget(cell, 0, column);
        return value;
    };
    m_gatewayStateLabel = addMetric(0, QStringLiteral("状态"));
    m_gatewayEndpointLabel = addMetric(1, QStringLiteral("URL"));
    m_gatewayModeLabel = addMetric(2, QStringLiteral("连接模式"));
    auto *privacyLabel = addMetric(3, QStringLiteral("隐私"));
    privacyLabel->setText(QStringLiteral("仅元数据"));
    gatewayPage.second->addWidget(gatewayMetrics);
    m_gatewayMessageLabel = new QLabel(gatewayPage.first->widget());
    m_gatewayMessageLabel->setWordWrap(true);
    m_gatewayMessageLabel->setStyleSheet(QStringLiteral(
        "background:#f8fafc; color:#475467; border:1px solid #e4e7ec; border-radius:8px; padding:10px 12px; font-size:12px;"));
    gatewayPage.second->addWidget(m_gatewayMessageLabel);
    auto *logPanel = new QFrame(gatewayPage.first->widget());
    logPanel->setObjectName(QStringLiteral("gatewayLogPanel"));
    logPanel->setStyleSheet(QStringLiteral(
        "QFrame#gatewayLogPanel { background:#ffffff; border:1px solid #e4e7ec; border-radius:8px; }"));
    auto *logPanelLayout = new QVBoxLayout(logPanel);
    logPanelLayout->setContentsMargins(16, 14, 16, 16);
    auto *logHeader = new QHBoxLayout;
    auto *logTitle = new QLabel(QStringLiteral("请求记录"), logPanel);
    logTitle->setStyleSheet(QStringLiteral("font-size:14px; font-weight:700; color:#101828;"));
    logHeader->addWidget(logTitle);
    logHeader->addStretch();
    auto *clearGatewayLogs = new QPushButton(QStringLiteral("清空"), logPanel);
    clearGatewayLogs->setStyleSheet(AppTheme::secondaryButtonStyle());
    logHeader->addWidget(clearGatewayLogs);
    logPanelLayout->addLayout(logHeader);
    m_gatewayLogTable = new QTableWidget(logPanel);
    m_gatewayLogTable->setColumnCount(6);
    m_gatewayLogTable->setHorizontalHeaderLabels({
        QStringLiteral("时间"), QStringLiteral("工具"), QStringLiteral("请求"),
        QStringLiteral("模型"), QStringLiteral("状态"), QStringLiteral("耗时") });
    m_gatewayLogTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_gatewayLogTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_gatewayLogTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_gatewayLogTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_gatewayLogTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_gatewayLogTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_gatewayLogTable->verticalHeader()->hide();
    m_gatewayLogTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_gatewayLogTable->setMinimumHeight(250);
    logPanelLayout->addWidget(m_gatewayLogTable);
    gatewayPage.second->addWidget(logPanel);

    // 系统与扩展页：把次级操作收敛为安静的操作网格。
    const auto settingsPage = makePage(QStringLiteral("settingsPage"));
    const auto settingsHeader = makeHeader(
        settingsPage.first->widget(), QStringLiteral("插件、Skills 与系统"),
        QStringLiteral("管理 Codex 插件、自定义 Skills、MCP、配置数据与应用维护"));
    settingsPage.second->addWidget(settingsHeader.first);
    auto *settingsGrid = new QGridLayout;
    settingsGrid->setHorizontalSpacing(10);
    settingsGrid->setVerticalSpacing(10);
    const QList<QPushButton *> settingsButtons = {
        m_manageKeysButton, m_viewModelsButton, m_imageGenerationButton,
        m_chatButton, m_skillsButton, m_mcpConfigButton,
        m_backupsButton, m_transferButton, m_doctorButton,
        m_desktopDownloadButton, m_checkUpdatesButton, helpButton,
    };
    int settingsIndex = 0;
    for (QPushButton *button : settingsButtons) {
        button->show();
        button->setMinimumHeight(52);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background:#ffffff; color:#344054; border:1px solid #e4e7ec;"
            "border-radius:8px; text-align:left; padding:0 16px; font-size:13px; font-weight:600; }"
            "QPushButton:hover { background:#f8fafc; border-color:#98a2b3; color:#101828; }"));
        settingsGrid->addWidget(button, settingsIndex / 3, settingsIndex % 3);
        ++settingsIndex;
    }
    settingsPage.second->addLayout(settingsGrid);

    toolsHeader->hide();
    dataHeader->hide();
    systemHeader->hide();

    m_workspaceStack->addWidget(dashboardPage.first);
    m_workspaceStack->addWidget(desktopPage.first);
    m_workspaceStack->addWidget(content);
    m_workspaceStack->addWidget(gatewayPage.first);
    m_workspaceStack->addWidget(settingsPage.first);
    m_agentWorkbench = new AgentWorkbenchWidget(
        m_workbenchEmergencyDisabled, m_workspaceStack);
    m_agentWorkbench->setEmergencyDisabled(
        m_workbenchEmergencyDisabled, m_workbenchEmergencyReasonCode,
        m_workbenchEmergencyPolicyVerified);
    m_workspaceStack->addWidget(m_agentWorkbench);
    bodyLayout->addWidget(m_workspaceStack, 1);
    root->addWidget(body, 1);

    connect(dashboardRefresh, &QPushButton::clicked,
            this, &MainWindow::refreshToolVersions);
    connect(desktopRefresh, &QPushButton::clicked,
            this, &MainWindow::refreshDesktopPage);
    connect(m_gatewayStartButton, &QPushButton::clicked, this, [this]() {
        QSettings().setValue(QStringLiteral("gateway/enabled"), true);
        if (!m_gatewayManager->start()) {
            QSettings().setValue(QStringLiteral("gateway/enabled"), false);
            m_gatewayMessageLabel->setText(
                QStringLiteral("启动失败：%1").arg(m_gatewayManager->lastError()));
        }
        refreshGatewayPage();
    });
    connect(m_gatewayStopButton, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(
                this, QStringLiteral("停止本地网关"),
                QStringLiteral("停止后会恢复当前档案的直接连接配置。确定继续吗？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        QSettings().setValue(QStringLiteral("gateway/enabled"), false);
        m_gatewayManager->stop();
        refreshGatewayPage();
    });
    connect(m_gatewayRestartButton, &QPushButton::clicked, this, [this]() {
        QSettings().setValue(QStringLiteral("gateway/enabled"), true);
        if (m_gatewayManager->isRunning()) m_gatewayManager->stop();
        if (!m_gatewayManager->start()) {
            QSettings().setValue(QStringLiteral("gateway/enabled"), false);
            m_gatewayMessageLabel->setText(
                QStringLiteral("重启失败：%1").arg(m_gatewayManager->lastError()));
        }
        refreshGatewayPage();
    });
    connect(clearGatewayLogs, &QPushButton::clicked, this, [this]() {
        m_gatewayManager->clearRequestLogs();
        refreshGatewayLogs();
    });

    connect(m_logoutButton, &QPushButton::clicked,
            this, &MainWindow::onLogoutClicked);
    connect(m_newConnectButton, &QPushButton::clicked,
            this, &MainWindow::onNewConnectClicked);
    connect(m_bulkSwitchButton, &QPushButton::clicked,
            this, &MainWindow::onBulkSwitchClicked);
    connect(m_manageKeysButton, &QPushButton::clicked,
            this, &MainWindow::onManageKeysClicked);
    connect(m_viewModelsButton, &QPushButton::clicked,
            this, &MainWindow::onViewModelsClicked);
    connect(m_imageGenerationButton, &QPushButton::clicked,
            this, &MainWindow::onImageGenerationClicked);
    connect(m_backupsButton, &QPushButton::clicked,
            this, &MainWindow::onBackupsClicked);
    connect(m_transferButton, &QPushButton::clicked,
            this, &MainWindow::onTransferClicked);
    if (m_updateManager && m_updateManager->isSupported()) {
        m_autoUpdateChecksAction->setChecked(
            m_updateManager->automaticallyChecksForUpdates());
        connect(m_checkUpdatesAction, &QAction::triggered,
                m_updateManager, &UpdateManager::checkForUpdates);
        connect(m_autoUpdateChecksAction, &QAction::toggled,
                this, [this](bool enabled) {
            m_updateManager->setAutomaticallyChecksForUpdates(enabled);
            logMessage(
                enabled ? QStringLiteral("已开启自动检查更新")
                        : QStringLiteral("已关闭自动检查更新"),
                enabled ? kLogSuccess : kLogMuted);
        });
        connect(m_updateManager, &UpdateManager::automaticChecksChanged,
                m_autoUpdateChecksAction, &QAction::setChecked);
    } else {
        m_checkUpdatesButton->setEnabled(false);
        m_checkUpdatesButton->setToolTip(QStringLiteral("当前平台暂不支持应用内更新"));
    }
    connect(m_refreshToolVersionsButton, &QPushButton::clicked,
            this, &MainWindow::refreshToolVersions);
    connect(m_doctorButton, &QPushButton::clicked,
            this, &MainWindow::onSystemDoctorClicked);
    connect(m_gatewayButton, &QPushButton::clicked,
            this, &MainWindow::onGatewayClicked);
    connect(m_desktopEnhancementsButton, &QPushButton::clicked,
            this, &MainWindow::onDesktopEnhancementsClicked);
    connect(m_desktopDownloadButton, &QPushButton::clicked,
            this, &MainWindow::onDesktopDownloadClicked);
    connect(m_chatButton, &QPushButton::clicked,
            this, &MainWindow::onChatClicked);
    connect(m_skillsButton, &QPushButton::clicked,
            this, &MainWindow::onSkillsClicked);
    connect(m_mcpConfigButton, &QPushButton::clicked,
            this, &MainWindow::onMcpConfigClicked);
    connect(m_balanceButton, &QPushButton::clicked,
            this, &MainWindow::onUsageClicked);
    connect(m_userLabel, &QPushButton::clicked,
            this, &MainWindow::onAccountClicked);
    m_balanceRefreshTimer = new QTimer(this);
    m_balanceRefreshTimer->setInterval(60 * 1000);
    connect(m_balanceRefreshTimer, &QTimer::timeout,
            this, &MainWindow::refreshBalance);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_navGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &MainWindow::switchWorkspacePage);
    connect(m_filterGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &MainWindow::onFilterChanged);
#else
    connect(m_navGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, &MainWindow::switchWorkspacePage);
    connect(m_filterGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, &MainWindow::onFilterChanged);
#endif
    connect(clearLogButton, &QPushButton::clicked, m_logOutput, &QTextEdit::clear);
    for (int pageIndex = 0; pageIndex < 6; ++pageIndex) {
        auto *shortcut = new QShortcut(
            QKeySequence(QStringLiteral("Alt+%1").arg(pageIndex + 1)), this);
        connect(shortcut, &QShortcut::activated, this,
                [this, pageIndex]() { switchWorkspacePage(pageIndex); });
    }
    refreshDesktopPage();
    refreshGatewayPage();
    refreshGatewayLogs();
    if (QAbstractButton *launchpadButton = m_navGroup->button(0)) {
        launchpadButton->setFocus(Qt::OtherFocusReason);
    }
    QTimer::singleShot(0, dashboardPage.first, [scroll = dashboardPage.first]() {
        scroll->verticalScrollBar()->setValue(0);
    });
}

void MainWindow::switchWorkspacePage(int pageIndex)
{
    if (!m_workspaceStack || pageIndex < 0 || pageIndex >= m_workspaceStack->count()) {
        return;
    }
    m_workspaceStack->setCurrentIndex(pageIndex);
    if (m_navGroup) {
        if (QAbstractButton *button = m_navGroup->button(pageIndex)) {
            button->setChecked(true);
        }
    }
    if (pageIndex == 1) {
        refreshDesktopPage();
    } else if (pageIndex == 3) {
        refreshGatewayPage();
        refreshGatewayLogs();
    }
}

void MainWindow::refreshDesktopPage()
{
    const auto refreshProduct = [this](AiTool tool, QLabel *statusLabel,
                                       QPushButton *actionButton) {
        if (!statusLabel || !actionButton) return;
        const DesktopAppStatus state = m_toolManager->detectDesktop(tool);
        const bool proxyDownloadable =
            DesktopDownloader::proxiedProductForTool(tool).has_value();
        if (state.installed) {
            statusLabel->setText(QStringLiteral("已检测到本机安装"));
            statusLabel->setStyleSheet(QStringLiteral(
                "font-size:12px; color:#067647; font-weight:700;"));
            actionButton->setText(QStringLiteral("已安装"));
            actionButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
            actionButton->setEnabled(false);
        } else {
            statusLabel->setText(proxyDownloadable
                ? QStringLiteral("未安装 · 可通过 Aegisy 代理下载")
                : QStringLiteral("当前平台仅支持官方下载页"));
            statusLabel->setStyleSheet(QStringLiteral(
                "font-size:12px; color:#b54708; font-weight:700;"));
            actionButton->setText(proxyDownloadable
                ? QStringLiteral("下载并安装") : QStringLiteral("打开下载页"));
            actionButton->setIcon(style()->standardIcon(
                proxyDownloadable
                    ? QStyle::SP_ArrowDown : QStyle::SP_BrowserReload));
            actionButton->setEnabled(true);
        }
    };
    refreshProduct(AiTool::CodexCli, m_chatGptDesktopStatus, m_chatGptDesktopAction);
    refreshProduct(AiTool::ClaudeCode, m_claudeDesktopStatus, m_claudeDesktopAction);
}

void MainWindow::refreshGatewayPage()
{
    if (!m_gatewayStateLabel) return;
    const bool running = m_gatewayManager->isRunning();
    m_gatewayStateLabel->setText(running ? QStringLiteral("运行中") : QStringLiteral("已停止"));
    m_gatewayStateLabel->setStyleSheet(running
        ? QStringLiteral("font-size:13px; font-weight:700; color:#067647; border:none;")
        : QStringLiteral("font-size:13px; font-weight:700; color:#b54708; border:none;"));
    m_gatewayEndpointLabel->setText(m_gatewayManager->endpoint(AiTool::CodexCli));
    m_gatewayEndpointLabel->setToolTip(QStringLiteral(
        "Codex: %1\nClaude: %2\nGemini: %3\nOpenCode: %4")
        .arg(m_gatewayManager->endpoint(AiTool::CodexCli),
             m_gatewayManager->endpoint(AiTool::ClaudeCode),
             m_gatewayManager->endpoint(AiTool::GeminiCli),
             m_gatewayManager->endpoint(AiTool::OpenCode)));
    const bool configured = QSettings().value(
        QStringLiteral("gateway/enabled"), false).toBool();
    m_gatewayModeLabel->setText(configured
        ? QStringLiteral("网关配置") : QStringLiteral("直接连接"));
    m_gatewayMessageLabel->setText(running
        ? QStringLiteral("网关仅监听 127.0.0.1，真实 API Key 只驻留在本机进程内存中。")
        : QStringLiteral("当前未运行本地网关，已激活档案直接连接 Aegisy 服务。"));
    m_gatewayStartButton->setEnabled(!running);
    m_gatewayRestartButton->setEnabled(running);
    m_gatewayStopButton->setEnabled(running);
}

void MainWindow::refreshGatewayLogs()
{
    if (!m_gatewayLogTable) return;
    const QList<QJsonObject> logs = m_gatewayManager->requestLogs();
    m_gatewayLogTable->setRowCount(0);
    const int first = qMax(0, logs.size() - 50);
    for (int index = logs.size() - 1; index >= first; --index) {
        const QJsonObject entry = logs.at(index);
        const int row = m_gatewayLogTable->rowCount();
        m_gatewayLogTable->insertRow(row);
        const QString timestamp = entry.value(QStringLiteral("timestamp")).toString();
        const QString method = entry.value(QStringLiteral("method")).toString();
        const QString path = entry.value(QStringLiteral("path")).toString();
        m_gatewayLogTable->setItem(row, 0, new QTableWidgetItem(timestamp.mid(11, 8)));
        m_gatewayLogTable->setItem(row, 1, new QTableWidgetItem(
            entry.value(QStringLiteral("tool")).toString()));
        m_gatewayLogTable->setItem(row, 2, new QTableWidgetItem(
            QStringLiteral("%1 %2").arg(method, path)));
        m_gatewayLogTable->setItem(row, 3, new QTableWidgetItem(
            entry.value(QStringLiteral("model")).toString()));
        m_gatewayLogTable->setItem(row, 4, new QTableWidgetItem(QString::number(
            entry.value(QStringLiteral("status")).toInt())));
        m_gatewayLogTable->setItem(row, 5, new QTableWidgetItem(QStringLiteral("%1 ms").arg(
            entry.value(QStringLiteral("latency_ms")).toInt())));
    }
}

void MainWindow::rebuildCards()
{
    refreshConfigurationWatchers();
    QLayoutItem *item = nullptr;
    while ((item = m_cardsLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_cardTestWidgets.clear();

    const QList<Profile> profiles = m_profileManager->allProfiles();
    m_profileCountLabel->setState(
        QStringLiteral("%1 个配置").arg(profiles.size()),
        StatusBadge::Tone::Neutral,
        style()->standardIcon(QStyle::SP_FileDialogListView));

    QSet<int> readyActiveProfiles;
    QHash<int, QString> repairIssues;
    QStringList activeDescriptions;
    QStringList repairDescriptions;
    for (ProfileType type : allProfileTypes()) {
        const int activeIndex = m_profileManager->activeIndex(type);
        if (activeIndex >= 0 && activeIndex < profiles.size()
                && profiles[activeIndex].hasAnyKey()) {
            QString issue;
            if (isProfileConfigurationReady(profiles[activeIndex], &issue)) {
                readyActiveProfiles.insert(activeIndex);
                activeDescriptions.append(QStringLiteral("%1：%2")
                    .arg(profileTypeName(type), profiles[activeIndex].name));
            } else {
                repairIssues.insert(activeIndex, issue);
                repairDescriptions.append(QStringLiteral("%1：%2（%3）")
                    .arg(profileTypeName(type), profiles[activeIndex].name, issue));
            }
        }
    }
    if (activeDescriptions.isEmpty() && repairDescriptions.isEmpty()) {
        m_activeProfileLabel->setState(
            QStringLiteral("尚未激活"), StatusBadge::Tone::Neutral,
            style()->standardIcon(QStyle::SP_MessageBoxInformation));
        m_activeProfileLabel->setToolTip(QString());
    } else if (activeDescriptions.isEmpty()) {
        m_activeProfileLabel->setState(
            QStringLiteral("%1 个配置需修复").arg(repairDescriptions.size()),
            StatusBadge::Tone::Error,
            style()->standardIcon(QStyle::SP_MessageBoxCritical));
        m_activeProfileLabel->setToolTip(repairDescriptions.join(QLatin1Char('\n')));
    } else {
        const bool hasRepairs = !repairDescriptions.isEmpty();
        const QString summary = hasRepairs
            ? QStringLiteral("%1 个已激活 · %2 个需修复")
                .arg(activeDescriptions.size()).arg(repairDescriptions.size())
            : QStringLiteral("%1 个终端已激活").arg(activeDescriptions.size());
        m_activeProfileLabel->setState(
            summary,
            hasRepairs ? StatusBadge::Tone::Warning : StatusBadge::Tone::Success,
            style()->standardIcon(hasRepairs
                ? QStyle::SP_MessageBoxWarning : QStyle::SP_DialogApplyButton));
        QStringList details = activeDescriptions;
        details.append(repairDescriptions);
        m_activeProfileLabel->setToolTip(details.join(QLatin1Char('\n')));
    }

    int visibleCount = 0;
    for (const Profile &profile : profiles) {
        if (m_filterType != 0 && static_cast<int>(profile.type) != m_filterType) {
            continue;
        }
        const bool isActive = readyActiveProfiles.contains(profile.index);
        const bool needsRepair = repairIssues.contains(profile.index);
        m_cardsLayout->addWidget(createProfileCard(
            profile, isActive, needsRepair, repairIssues.value(profile.index)));
        ++visibleCount;
    }

    if (visibleCount == 0) {
        auto *emptyState = new QFrame(m_cardsContainer);
        emptyState->setMinimumHeight(116);
        emptyState->setStyleSheet(QStringLiteral(
            "QFrame { background: white; border: 1px dashed #cfd4dc; border-radius: 8px; }"));
        auto *emptyLayout = new QVBoxLayout(emptyState);
        emptyLayout->setAlignment(Qt::AlignCenter);
        auto *emptyTitle = new QLabel(QStringLiteral("当前筛选下没有配置"), emptyState);
        emptyTitle->setAlignment(Qt::AlignCenter);
        emptyTitle->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600; color: #475467;"));
        auto *emptyHint = new QLabel(QStringLiteral("新建配置后会显示在这里"), emptyState);
        emptyHint->setAlignment(Qt::AlignCenter);
        emptyHint->setStyleSheet(QStringLiteral("font-size: 11px; color: #98a2b3;"));
        emptyLayout->addWidget(emptyTitle);
        emptyLayout->addWidget(emptyHint);
        m_cardsLayout->addWidget(emptyState);
    }

    m_cardsLayout->addWidget(createAddCard());
    m_cardsLayout->addStretch();
}

QWidget *MainWindow::createProfileCard(const Profile &profile, bool isActive,
                                       bool needsRepair,
                                       const QString &repairReason)
{
    const AiTool tool = profile.tool();
    const QString accent = toolAccent(tool);
    const QString background = needsRepair
        ? QStringLiteral("#fff7ed")
        : (isActive ? toolSoftColor(tool) : QStringLiteral("#ffffff"));
    const QString cardAccent = needsRepair ? QStringLiteral("#d97706") : accent;

    auto *card = new QFrame(m_cardsContainer);
    card->setObjectName(QStringLiteral("profileCard"));
    card->setMinimumHeight(112);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    card->setStyleSheet(QStringLiteral(
        "QFrame#profileCard {"
        "  background: %1; border: 1px solid #e4e7ec; border-left: 4px solid %2;"
        "  border-radius: 8px;"
        "}").arg(background, cardAccent));

    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(14, 12, 12, 12);
    layout->setSpacing(12);

    // 徽标优先显示配置名称首字母，同类型多档案时可一眼区分
    const QString badgeText = profile.name.isEmpty()
        ? toolLetter(tool)
        : profile.name.left(1).toUpper();
    const int badgeFontSize = badgeText.length() > 1 ? 13 : 18;
    auto *badge = new QLabel(badgeText, card);
    badge->setFixedSize(42, 42);
    badge->setAlignment(Qt::AlignCenter);
    badge->setStyleSheet(QStringLiteral(
        "background: %1; color: white; border: none; border-radius: 8px;"
        "font-size: %2px; font-weight: 700;").arg(accent).arg(badgeFontSize));
    layout->addWidget(badge);

    auto *details = new QVBoxLayout;
    details->setSpacing(3);

    // ── 第一行：配置名称（独占一行，始终完整显示）──────────────────
    auto *nameLabel = new QLabel(profile.name, card);
    nameLabel->setToolTip(profile.name);
    nameLabel->setStyleSheet(QStringLiteral(
        "font-size: 15px; font-weight: 700; color: #101828; border: none;"));
    details->addWidget(nameLabel);

    // ── 第二行：工具类型 badge + 激活状态 badge ────────────────────
    auto *badgeRow = new QHBoxLayout;
    badgeRow->setSpacing(6);
    auto *typeBadge = new QLabel(profileTypeName(profile.type), card);
    typeBadge->setStyleSheet(QStringLiteral(
        "background: %1; color: %2; border: 1px solid %2;"
        "border-radius: 6px; padding: 2px 7px; font-size: 10px; font-weight: 700;")
        .arg(toolSoftColor(tool), accent));
    badgeRow->addWidget(typeBadge);
    if (isActive) {
        auto *activeBadge = new QLabel(QStringLiteral("当前使用"), card);
        activeBadge->setStyleSheet(QStringLiteral(
            "background: #dcfae6; color: #067647; border: 1px solid #abefc6;"
            "border-radius: 6px; padding: 2px 7px; font-size: 10px; font-weight: 700;"));
        badgeRow->addWidget(activeBadge);
    } else if (needsRepair) {
        auto *repairBadge = new QLabel(QStringLiteral("需修复"), card);
        repairBadge->setToolTip(repairReason);
        repairBadge->setStyleSheet(QStringLiteral(
            "background: #fffaeb; color: #b54708; border: 1px solid #fedf89;"
            "border-radius: 6px; padding: 2px 7px; font-size: 10px; font-weight: 700;"));
        badgeRow->addWidget(repairBadge);
    }
    badgeRow->addStretch();
    details->addLayout(badgeRow);

    auto *toolLine = new QLabel(
        QStringLiteral("%1  ·  %2  ·  %3")
            .arg(ToolManager::toolName(tool),
                 m_toolVersionTexts.value(static_cast<int>(tool), QStringLiteral("检测中...")),
                 toolConfigPath(tool)), card);
    toolLine->setStyleSheet(QStringLiteral(
        "font-size: 11px; color: #667085; border: none;"));
    details->addWidget(toolLine);

    auto *configRow = new QHBoxLayout;
    configRow->setSpacing(14);
    QString keyText;
    if (!profile.hasAnyKey()) {
        keyText = QStringLiteral("Key：未配置");
    } else if (!profile.keyHint.isEmpty()) {
        keyText = QStringLiteral("凭据：已安全保存 · ID %1").arg(profile.keyHint);
    } else {
        keyText = QStringLiteral("Key：已安全保存");
    }
    auto *keyLabel = new QLabel(keyText, card);
    keyLabel->setStyleSheet(QStringLiteral(
        "font-family: monospace; font-size: 11px; color: %1; border: none;")
        .arg(profile.hasAnyKey() ? QStringLiteral("#475467") : QStringLiteral("#b54708")));
    configRow->addWidget(keyLabel);
    auto *modelLabel = new QLabel(
        QStringLiteral("模型：%1")
            .arg(profile.model.isEmpty() ? QStringLiteral("工具默认") : profile.model), card);
    modelLabel->setToolTip(profile.model);
    modelLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    modelLabel->setStyleSheet(QStringLiteral(
        "font-size: 11px; color: #667085; border: none;"));
    configRow->addWidget(modelLabel, 1);
    details->addLayout(configRow);
    layout->addLayout(details, 1);

    auto *launchButton = new QPushButton(QStringLiteral("启动"), card);
    launchButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    launchButton->setFixedSize(78, 36);
    launchButton->setCursor(Qt::PointingHandCursor);
    launchButton->setEnabled(isActive && profile.hasAnyKey());
    launchButton->setToolTip(needsRepair
        ? repairReason
        : isActive
        ? QStringLiteral("自动检测系统终端并启动 %1")
            .arg(ToolManager::toolName(tool))
        : QStringLiteral("请先激活该配置"));
    launchButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    const int profileIndex = profile.index;
    connect(launchButton, &QPushButton::clicked,
            this, [this, profileIndex]() { launchProfile(profileIndex, false); });
    layout->addWidget(launchButton);

    auto *activateButton = new QPushButton(
        isActive ? QStringLiteral("已激活")
                 : (needsRepair ? QStringLiteral("修复") : QStringLiteral("激活")), card);
    activateButton->setIcon(style()->standardIcon(
        isActive ? QStyle::SP_DialogApplyButton
                 : (needsRepair ? QStyle::SP_BrowserReload : QStyle::SP_MediaPlay)));
    activateButton->setFixedSize(92, 36);
    activateButton->setCursor(Qt::PointingHandCursor);
    activateButton->setEnabled(!isActive && profile.hasAnyKey());
    if (needsRepair) activateButton->setToolTip(repairReason);
    activateButton->setStyleSheet(isActive
        ? QStringLiteral(
            "QPushButton { background: #dcfae6; color: #067647; border: 1px solid #abefc6;"
            "border-radius: 7px; font-size: 12px; font-weight: 600; }")
        : AppTheme::primaryButtonStyle());
    connect(activateButton, &QPushButton::clicked,
            this, [this, profileIndex]() { activateProfile(profileIndex); });
    layout->addWidget(activateButton);

    auto *moreButton = new QToolButton(card);
    moreButton->setText(QStringLiteral("⋯"));
    moreButton->setToolTip(QStringLiteral("更多配置操作"));
    moreButton->setFixedSize(36, 36);
    moreButton->setCursor(Qt::PointingHandCursor);
    moreButton->setPopupMode(QToolButton::InstantPopup);
    moreButton->setStyleSheet(QStringLiteral(
        "QToolButton { background: #ffffff; color: #475467; border: 1px solid #d0d5dd;"
        "border-radius: 7px; font-size: 18px; font-weight: 700; padding: 0; }"
        "QToolButton:hover { background: #F5F8FF; color: #165DFF; border-color: #98B3F6; }"
        "QToolButton::menu-indicator { image: none; width: 0; height: 0; }"));
    auto *moreMenu = new QMenu(moreButton);
    QAction *testAction = moreMenu->addAction(
        style()->standardIcon(QStyle::SP_BrowserReload), QStringLiteral("测试连接"));
    testAction->setEnabled(profile.hasAnyKey());
    QAction *editAction = moreMenu->addAction(
        style()->standardIcon(QStyle::SP_FileDialogDetailedView), QStringLiteral("编辑配置"));
    moreMenu->addSeparator();
    QAction *deleteAction = moreMenu->addAction(
        style()->standardIcon(QStyle::SP_DialogDiscardButton), QStringLiteral("删除配置"));
    deleteAction->setEnabled(m_profileManager->count() > 1);
    moreButton->setMenu(moreMenu);
    connect(editAction, &QAction::triggered,
            this, [this, profileIndex]() { editProfile(profileIndex); });
    connect(deleteAction, &QAction::triggered,
            this, [this, profileIndex]() { deleteProfile(profileIndex); });
    layout->addWidget(moreButton);

    auto *testResultLabel = new StatusBadge(card);
    testResultLabel->setState(QStringLiteral("等待测试"), StatusBadge::Tone::Neutral);
    testResultLabel->hide();
    // 把测试结果 label 放到 details 布局的最后
    details->addWidget(testResultLabel);

    const QString testRequestId = QStringLiteral("card-test-%1").arg(profile.index);
    m_cardTestWidgets.insert(testRequestId,
        qMakePair(testResultLabel, testAction));

    connect(testAction, &QAction::triggered, this,
        [this, testRequestId, testAction, testResultLabel, profileIndex]() {
            const QList<Profile> ps = m_profileManager->allProfiles();
            if (profileIndex < 0 || profileIndex >= ps.size()) return;
            const Profile p = m_profileManager->profileWithCredential(profileIndex);
            if (p.key.isEmpty()) return;

            testAction->setEnabled(false);
            testAction->setIcon(style()->standardIcon(QStyle::SP_BrowserStop));
            testResultLabel->setState(
                QStringLiteral("正在测试"), StatusBadge::Tone::Info,
                style()->standardIcon(QStyle::SP_BrowserReload));
            testResultLabel->show();
            m_apiClient->testConnection(testRequestId, p.key,
                                        ps[profileIndex].model);
        });

    return card;
}

void MainWindow::launchProfile(int index, bool embedded)
{
    Q_UNUSED(embedded);
    const QList<Profile> profiles = m_profileManager->allProfiles();
    if (index < 0 || index >= profiles.size()) {
        return;
    }
    const Profile &profile = profiles[index];
    if (!m_profileManager->isActive(index) || !profile.hasAnyKey()) {
        QMessageBox::information(this, QStringLiteral("尚未激活"),
                                 QStringLiteral("请先激活该配置，再启动对应终端。"));
        return;
    }
    QString configurationIssue;
    if (!isProfileConfigurationReady(profile, &configurationIssue)) {
        rebuildCards();
        rebuildTrayMenu();
        const auto reply = QMessageBox::question(
            this,
            QStringLiteral("配置需要修复"),
            QStringLiteral("%1\n\n是否立即重新写入「%2」的本地配置？")
                .arg(configurationIssue, profile.name),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes);
        if (reply == QMessageBox::Yes) activateProfile(index);
        return;
    }

    const QString directorySetting = QStringLiteral("launch/directories/%1")
        .arg(profile.id.isEmpty() ? QString::number(profile.index) : profile.id);
    QSettings settings;
    QString initialDirectory = settings.value(
        directorySetting, QDir::homePath()).toString();
    if (!QFileInfo(initialDirectory).isDir()) {
        initialDirectory = QDir::homePath();
    }
    const QString directory = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择 %1 的工作目录").arg(ToolManager::toolName(profile.tool())),
        initialDirectory,
        QFileDialog::ShowDirsOnly);
    if (directory.isEmpty()) {
        logMessage(QStringLiteral("已取消启动「%1」").arg(profile.name), kLogMuted);
        return;
    }
    settings.setValue(directorySetting, directory);

    if (!m_toolManager->launch(profile.tool(), directory)) {
        QMessageBox::warning(this, QStringLiteral("启动失败"),
                             m_toolManager->lastError());
        logMessage(QStringLiteral("%1 启动失败：%2")
            .arg(ToolManager::toolName(profile.tool()), m_toolManager->lastError()),
            kLogError);
        return;
    }
    logMessage(QStringLiteral("已在系统终端启动 %1，工作目录：%2")
        .arg(ToolManager::toolName(profile.tool()), directory), kLogSuccess);
}

QWidget *MainWindow::createAddCard()
{
    auto *button = new QPushButton(QStringLiteral("新建单终端配置"), m_cardsContainer);
    button->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    button->setFixedHeight(54);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: transparent; color: #667085; border: 1px dashed #b9c0ca;"
        "  border-radius: 8px; font-size: 12px;"
        "}"
        "QPushButton:hover { background: #F5F8FF; color: #174EA6; border-color: #165DFF; }"));
    connect(button, &QPushButton::clicked,
            this, &MainWindow::onNewConnectClicked);
    return button;
}

void MainWindow::onFilterChanged(int typeId)
{
    m_filterType = typeId;
    rebuildCards();
}

void MainWindow::onBulkSwitchClicked()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("全工具一键切换"));
    dialog.setMinimumWidth(520);
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dialog.setStyleSheet(QStringLiteral("QDialog { background: #f4f7f9; } QLabel { color: #182230; }"));

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(24, 22, 24, 18);
    root->setSpacing(14);

    auto *titleRow = new QHBoxLayout;
    auto *titleIcon = new QLabel(&dialog);
    titleIcon->setPixmap(style()->standardIcon(QStyle::SP_BrowserReload).pixmap(20, 20));
    titleRow->addWidget(titleIcon);
    auto *title = new QLabel(QStringLiteral("全工具一键切换"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 700; color: #101828;"));
    titleRow->addWidget(title);
    titleRow->addStretch();
    auto *toolCount = new StatusBadge(&dialog);
    toolCount->setState(
        QStringLiteral("4 个工具"), StatusBadge::Tone::Info,
        style()->standardIcon(QStyle::SP_ComputerIcon));
    titleRow->addWidget(toolCount);
    root->addLayout(titleRow);

    const struct { ProfileType type; const char *accent; const char *label; } rows[] = {
        { ProfileType::Claude, "#c15f3c", "Claude Code" },
        { ProfileType::Codex,  "#6366f1", "Codex CLI" },
        { ProfileType::Gemini, "#165DFF", "Gemini CLI" },
        { ProfileType::OpenCode, "#059669", "OpenCode" },
    };

    QHash<ProfileType, QComboBox *> combos;
    for (const auto &row : rows) {
        auto *rowWidget = new QFrame(&dialog);
        rowWidget->setStyleSheet(QStringLiteral(
            "QFrame { background: white; border: 1px solid #e4e7ec; border-radius: 8px; }"));
        auto *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(14, 10, 14, 10);
        rowLayout->setSpacing(12);

        auto *dot = new QLabel(QStringLiteral("●"), rowWidget);
        dot->setStyleSheet(QStringLiteral("color: %1; font-size: 14px;")
                           .arg(QString::fromLatin1(row.accent)));
        dot->setFixedWidth(16);
        rowLayout->addWidget(dot);

        auto *label = new QLabel(QString::fromLatin1(row.label), rowWidget);
        label->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600; color: #344054;"));
        label->setFixedWidth(110);
        rowLayout->addWidget(label);

        auto *combo = new QComboBox(rowWidget);
        combo->setFixedHeight(34);
        combo->addItem(QStringLiteral("— 不切换此工具 —"), -1);

        const int activeIndex = m_profileManager->activeIndex(row.type);
        const QList<Profile> profiles = m_profileManager->allProfiles();
        for (const Profile &profile : profiles) {
            if (profile.type != row.type || !profile.hasAnyKey()) continue;
            QString label = profile.name;
            if (!profile.model.isEmpty()) {
                label += QStringLiteral(" · %1").arg(profile.model);
            }
            if (!profile.keyHint.isEmpty()) {
                label += QStringLiteral(" · 凭据 ID %1").arg(profile.keyHint);
            }
            combo->addItem(label, profile.index);
            if (profile.index == activeIndex) {
                combo->setCurrentIndex(combo->count() - 1);
            }
        }
        if (combo->count() == 1) {
            combo->setItemText(0, QStringLiteral("— 暂无可用档案 —"));
            combo->setEnabled(false);
        }
        rowLayout->addWidget(combo, 1);
        combos.insert(row.type, combo);
        root->addWidget(rowWidget);
    }

    auto *buttonRow = new QHBoxLayout;
    auto *cancelButton = new QPushButton(QStringLiteral("取消"), &dialog);
    cancelButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    auto *applyButton = new QPushButton(QStringLiteral("全部激活"), &dialog);
    applyButton->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    applyButton->setStyleSheet(AppTheme::primaryButtonStyle());
    buttonRow->addStretch();
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(applyButton);
    root->addLayout(buttonRow);

    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(applyButton, &QPushButton::clicked, &dialog, [&]() {
        int scheduled = 0;
        for (const auto &row : rows) {
            QComboBox *combo = combos.value(row.type);
            if (!combo) continue;
            const int profileIndex = combo->currentData().toInt();
            if (profileIndex < 0) continue;
            const QList<Profile> profiles = m_profileManager->allProfiles();
            if (!ProfileManager::isActivationSelectionValid(
                    profiles, profileIndex, row.type)) {
                logMessage(
                    QStringLiteral("%1 选择的档案已失效").arg(profileTypeName(row.type)),
                    kLogError);
                continue;
            }
            ++scheduled;
            if (!configureFromProfile(profileIndex, toolForType(row.type))) {
                logMessage(
                    QStringLiteral("%1 批量激活失败：%2")
                        .arg(profileTypeName(row.type), m_toolManager->lastError()),
                    kLogError);
            } else {
                m_profileManager->setActiveIndex(profileIndex);
                logMessage(
                    QStringLiteral("✓ %1 已切换到「%2」")
                        .arg(profileTypeName(row.type), profiles[profileIndex].name),
                    kLogSuccess);
            }
        }
        if (scheduled == 0) {
            QMessageBox::information(&dialog, QStringLiteral("未做任何更改"),
                                     QStringLiteral("四个工具均选择了「不切换」，没有更改任何配置。"));
            return;
        }
        rebuildCards();
        dialog.accept();
    });

    dialog.exec();
}

void MainWindow::onNewConnectClicked()
{
    ConnectWizardDialog dialog(m_apiClient, m_profileManager, -1, this);
    const int result = dialog.exec();
    m_apiClient->getApiKeys();

    if (result != QDialog::Accepted) {
        rebuildCards();
        return;
    }

    const int newIndex = dialog.resultIndex();
    logMessage(QStringLiteral("新配置已保存，正在检查本地环境..."), kLogSuccess);
    rebuildCards();
    showEnvCheckDialog(newIndex);
    if (newIndex >= 0) {
        activateProfile(newIndex);
    }
}

void MainWindow::editProfile(int index)
{
    const bool wasActive = m_profileManager->isActive(index);
    ConnectWizardDialog dialog(m_apiClient, m_profileManager, index, this);
    const int result = dialog.exec();
    m_apiClient->getApiKeys();

    if (result == QDialog::Accepted) {
        logMessage(QStringLiteral("配置已更新，正在检查本地环境..."), kLogSuccess);
        rebuildCards();
        showEnvCheckDialog(index);
        if (wasActive) {
            activateProfile(index);
        }
    } else {
        rebuildCards();
    }
}

void MainWindow::deleteProfile(int index)
{
    const QList<Profile> profiles = m_profileManager->allProfiles();
    if (index < 0 || index >= profiles.size()) {
        return;
    }
    if (profiles.size() <= 1) {
        QMessageBox::information(this, QStringLiteral("保留一个配置"),
                                 QStringLiteral("至少需要保留一个连接配置。"));
        return;
    }

    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("删除配置"),
        QStringLiteral("确定删除「%1」吗？此操作不可恢复。").arg(profiles[index].name),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    if (m_activatingIndex == index) {
        ++m_activationGeneration;
        m_activationQueue.clear();
        m_activatingIndex = -1;
    } else if (m_activatingIndex > index) {
        --m_activatingIndex;
    }

    m_profileManager->removeProfile(index);
    logMessage(QStringLiteral("配置已删除"), kLogMuted);
    rebuildCards();
}

void MainWindow::activateProfile(int index)
{
    const QList<Profile> profiles = m_profileManager->allProfiles();
    if (index < 0 || index >= profiles.size()) {
        return;
    }

    const Profile profile = profiles[index];
    if (!profile.hasAnyKey()) {
        logMessage(QStringLiteral("该配置尚未绑定 API Key，无法激活"), kLogWarn);
        return;
    }

    // 若用户之前勾选了「不再提示」，且本次没有警告，则跳过确认弹窗直接激活
    const bool skipConfirm = QSettings().value(
        QStringLiteral("ui/skipActivateConfirm"), false).toBool();
    const ConfigurationPreview quickPreview = m_toolManager->previewConfiguration(
        profile.tool(), profile.model,
        QSettings().value(QStringLiteral("gateway/enabled"), false).toBool());
    const bool hasWarnings = !quickPreview.warnings.isEmpty();

    if (!skipConfirm || hasWarnings) {
        if (!confirmConfigurationPreview(profile)) {
            logMessage(QStringLiteral("已取消激活「%1」").arg(profile.name), kLogMuted);
            return;
        }
    }

    if (m_activatingIndex >= 0 && m_activatingIndex != index) {
        logMessage(QStringLiteral("已切换激活任务，之前的异步结果将被忽略"), kLogMuted);
    }

    m_activatingIndex = index;
    ++m_activationGeneration;
    m_activationQueue.clear();
    m_activationQueue.append(profile.tool());

    logMessage(
        QStringLiteral("正在激活「%1」并更新 %2...")
            .arg(profile.name, toolConfigPath(profile.tool())),
        kLogInfo);
    rebuildCards();
    processActivationQueue();
}

bool MainWindow::confirmConfigurationPreview(const Profile &profile)
{
    const ConfigurationPreview preview = m_toolManager->previewConfiguration(
        profile.tool(), profile.model,
        QSettings().value(QStringLiteral("gateway/enabled"), false).toBool());
    QString text = QStringLiteral(
        "<b>将激活「%1」并更新 %2 配置</b><br><br>")
        .arg(profile.name.toHtmlEscaped(),
             ToolManager::toolName(profile.tool()).toHtmlEscaped());
    text += QStringLiteral("<b>目标文件</b><br>");
    for (const QString &file : preview.files) {
        text += QStringLiteral("• %1<br>").arg(file.toHtmlEscaped());
    }
    text += QStringLiteral("<br><b>计划变更</b><br>");
    for (const QString &change : preview.changes) {
        text += QStringLiteral("• %1<br>").arg(change.toHtmlEscaped());
    }
    if (!preview.warnings.isEmpty()) {
        text += QStringLiteral("<br><b style='color:#b54708'>需要注意</b><br>");
        for (const QString &warning : preview.warnings) {
            text += QStringLiteral("• %1<br>").arg(warning.toHtmlEscaped());
        }
    }

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("确认配置变更"));
    box.setIcon(preview.warnings.isEmpty()
        ? QMessageBox::Information : QMessageBox::Warning);
    box.setTextFormat(Qt::RichText);
    box.setText(text);
    box.setStandardButtons(QMessageBox::Cancel | QMessageBox::Ok);
    box.setDefaultButton(QMessageBox::Ok);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("备份并激活"));
    box.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));

    // 「不再提示」复选框（仅无警告时才出现，有警告时必须显式确认）
    auto *noPromptCheck = new QCheckBox(
        QStringLiteral("以后直接切换，不再弹出此对话框"), &box);
    noPromptCheck->setStyleSheet(QStringLiteral(
        "QCheckBox { font-size: 12px; color: #667085; margin-top: 6px; }"));
    box.setCheckBox(noPromptCheck);
    if (!preview.warnings.isEmpty()) {
        noPromptCheck->setVisible(false);   // 有警告时不允许跳过
    }

    const bool confirmed = box.exec() == QMessageBox::Ok;
    if (confirmed && noPromptCheck->isChecked()) {
        QSettings().setValue(QStringLiteral("ui/skipActivateConfirm"), true);
        logMessage(QStringLiteral("已开启快速切换模式，以后激活不再弹出确认框（可在首选项中重置）"),
                   kLogMuted);
    }
    return confirmed;
}

void MainWindow::warnIfCliRunning(const Profile &profile)
{
    const AiTool tool = profile.tool();
    if (!m_toolManager->isCliRunning(tool)) {
        return;
    }

    const QString name = ToolManager::toolName(tool);
    logMessage(
        QStringLiteral("检测到 %1 仍在运行，需重启后才会使用新 Key").arg(name),
        kLogWarn);

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("需重启终端"));
    box.setIcon(QMessageBox::Warning);
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral(
        "<b>「%1」已激活，但检测到 %2 仍在运行。</b><br><br>"
        "运行中的进程已把旧 Key 读入内存，不会自动重新读取 %3。<br>"
        "请<b>关闭并重新启动</b>该终端 / 桌面端，新 Key 才会生效。")
        .arg(profile.name.toHtmlEscaped(),
             name.toHtmlEscaped(),
             toolConfigPath(tool).toHtmlEscaped()));
    box.setStandardButtons(QMessageBox::Ok);
    box.button(QMessageBox::Ok)->setText(QStringLiteral("我知道了"));
    box.exec();
}

void MainWindow::processActivationQueue()
{
    if (m_activationQueue.isEmpty()) {
        const QList<Profile> profiles = m_profileManager->allProfiles();
        if (m_activatingIndex >= 0 && m_activatingIndex < profiles.size()) {
            const Profile &profile = profiles[m_activatingIndex];
            logMessage(
                QStringLiteral("「%1」已激活，本地认证配置已更新；已运行的终端需重启后使用新 Key")
                    .arg(profile.name),
                kLogSuccess);
            warnIfCliRunning(profile);
        }
        m_activatingIndex = -1;
        rebuildCards();
        return;
    }

    const AiTool tool = m_activationQueue.first();
    const bool configured = configureFromProfile(m_activatingIndex, tool);
    if (!configured) {
        logMessage(
            QStringLiteral("激活失败，已保留之前的当前档案和本地配置"),
            kLogError);
        m_activationQueue.clear();
        m_activatingIndex = -1;
        rebuildCards();
        return;
    }

    m_profileManager->setActiveIndex(m_activatingIndex);
    rebuildCards();
    const ToolStatus status = m_toolManager->detect(tool);

    if (!status.conflictWarning.isEmpty()) {
        logMessage(status.conflictWarning, kLogWarn);
    }

    if (!status.installed && !status.nodeOk) {
        logMessage(
            QStringLiteral("%1 认证已更新；安装 Node.js 后即可运行 CLI")
                .arg(ToolManager::toolName(tool)),
            kLogWarn);
        m_activationQueue.removeFirst();
        processActivationQueue();
        return;
    }

    if (!status.installed) {
        if (m_toolManager->isCliRunning(tool)) {
            logMessage(
                QStringLiteral("认证已更新；%1 正在运行，已跳过修复。关闭所有相关窗口和终端后，"
                               "可在“系统体检”中重新更新。")
                    .arg(ToolManager::toolName(tool)),
                kLogWarn);
            m_activationQueue.removeFirst();
            processActivationQueue();
            return;
        }
        logMessage(
            QStringLiteral("认证已更新，正在安装 %1...").arg(ToolManager::toolName(tool)),
            kLogInfo);
        m_toolManager->install(tool, m_activationGeneration);
        return;
    }

    m_activationQueue.removeFirst();
    processActivationQueue();
}

bool MainWindow::configureFromProfile(int profileIndex, AiTool tool)
{
    const Profile profile = m_profileManager->profileWithCredential(profileIndex);
    if (profile.tool() != tool || profile.key.isEmpty()) {
        if (!m_profileManager->lastError().isEmpty()) {
            logMessage(m_profileManager->lastError(), kLogError);
        }
        return false;
    }

    const bool gatewayEnabled = QSettings().value(
        QStringLiteral("gateway/enabled"), false).toBool();
    bool success = false;
    if (gatewayEnabled) {
        if (!m_gatewayManager->isRunning() && !m_gatewayManager->start()) {
            logMessage(QStringLiteral("本地网关启动失败：%1")
                .arg(m_gatewayManager->lastError()), kLogError);
            return false;
        }
        if (!m_gatewayManager->configureProfile(tool, profile.key)) {
            logMessage(QStringLiteral("无法把档案凭据加载到本地网关"), kLogError);
            return false;
        }
        success = m_toolManager->configureGateway(
            tool, m_gatewayManager->localToken(), profile.model);
    } else {
        success = m_toolManager->configure(tool, profile.key, profile.model);
    }
    if (success) {
        refreshConfigurationWatchers();
        logMessage(
            QStringLiteral("%1 已写入 %2")
                .arg(ToolManager::toolName(tool), toolConfigPath(tool)),
            kLogSuccess);
        if (!m_toolManager->lastWarning().isEmpty()) {
            logMessage(m_toolManager->lastWarning(), kLogWarn);
        }
    } else {
        logMessage(
            QStringLiteral("%1 配置写入失败：%2")
                .arg(ToolManager::toolName(tool), m_toolManager->lastError()),
            kLogError);
    }
    return success;
}

void MainWindow::onCompanionConfigurationReceived(const QJsonObject &projection)
{
    if (m_companionAccountIdentity.isEmpty()
            || projection.value(QStringLiteral("account_identity")).toString()
                != m_companionAccountIdentity) {
        onCompanionConfigurationFailed(QStringLiteral("projection-account-mismatch"));
        return;
    }
    QSettings settings;
    QString errorCode;
    if (!CompanionConfigProjection::saveLastValid(&settings, projection, &errorCode)) {
        onCompanionConfigurationFailed(
            errorCode.isEmpty() ? QStringLiteral("projection-cache-write-failed") : errorCode);
        return;
    }
    updateCompanionProjectionStatus(projection, true);
    logMessage(
        QStringLiteral("已同步网站配置元数据：%1 项（不含凭据值）")
            .arg(projection.value(QStringLiteral("key_count")).toInt()),
        kLogSuccess);
}

void MainWindow::onCompanionConfigurationFailed(const QString &errorCode)
{
    QSettings settings;
    const QJsonObject cached = CompanionConfigProjection::loadLastValid(
        &settings, m_companionAccountIdentity);
    if (!cached.isEmpty()) {
        updateCompanionProjectionStatus(cached, false);
    } else if (m_websiteProjectionLabel) {
        m_websiteProjectionLabel->setState(
            QStringLiteral("网站配置 不可用"), StatusBadge::Tone::Error,
            style()->standardIcon(QStyle::SP_MessageBoxWarning));
        m_websiteProjectionLabel->setToolTip(
            QStringLiteral("未获得可验证的网站配置元数据；不会自动修改本地配置"));
    }
    logMessage(
        QStringLiteral("网站配置元数据同步失败（%1），已保留本地状态")
            .arg(errorCode.isEmpty() ? QStringLiteral("projection-failed") : errorCode),
        kLogWarn);
}

void MainWindow::updateCompanionProjectionStatus(
    const QJsonObject &projection, bool online)
{
    if (!m_websiteProjectionLabel) return;
    const int count = projection.value(QStringLiteral("key_count")).toInt();
    m_websiteProjectionLabel->setState(
        online ? QStringLiteral("网站配置 %1 项").arg(count)
               : QStringLiteral("网站配置 离线 %1 项").arg(count),
        online ? StatusBadge::Tone::Success : StatusBadge::Tone::Warning,
        style()->standardIcon(online ? QStyle::SP_DialogApplyButton
                                     : QStyle::SP_MessageBoxWarning));
    m_websiteProjectionLabel->setToolTip(
        online
            ? QStringLiteral("来自已认证 Aegisy 网站响应的元数据投影；不含凭据值，不会自动写配置")
            : QStringLiteral("显示最后一次有效的网站元数据投影；离线状态不会写入本地配置"));
}

void MainWindow::onUserInfoReceived(const QJsonObject &userInfo)
{
    m_userInfo = userInfo;
    QJsonValue accountId = userInfo.value(QStringLiteral("id"));
    if (accountId.isUndefined() || accountId.isNull()) {
        accountId = userInfo.value(QStringLiteral("user_id"));
    }
    const QString verifiedAccountIdentity =
        CompanionConfigProjection::accountIdentityForWebsiteId(accountId);
    const bool accountChanged = !m_companionAccountIdentity.isEmpty()
        && verifiedAccountIdentity != m_companionAccountIdentity;
    const bool shouldSyncCompanion = m_waitingForCompanionAccount || accountChanged;
    m_companionAccountIdentity = verifiedAccountIdentity;
    m_waitingForCompanionAccount = false;
    if (shouldSyncCompanion) {
        if (m_companionAccountIdentity.isEmpty()) {
            onCompanionConfigurationFailed(QStringLiteral("projection-account-invalid"));
        } else {
            if (accountChanged && m_websiteProjectionLabel) {
                m_websiteProjectionLabel->setState(
                    QStringLiteral("网站配置 账号已切换"), StatusBadge::Tone::Neutral,
                    style()->standardIcon(QStyle::SP_DriveNetIcon));
            }
            QSettings projectionSettings;
            const QJsonObject cachedProjection =
                CompanionConfigProjection::loadLastValid(
                    &projectionSettings, m_companionAccountIdentity);
            if (!cachedProjection.isEmpty()) {
                updateCompanionProjectionStatus(cachedProjection, false);
            }
            m_apiClient->getApiKeys();
        }
    }
    const double balance = userInfo.value(QStringLiteral("balance")).toDouble();
    const QString formatted = QLocale(QLocale::English).toString(balance, 'f', 2);
    m_balanceButton->setText(QStringLiteral("余额  $%1").arg(formatted));
    m_balance = balance;
    m_balanceKnown = userInfo.contains(QStringLiteral("balance"));
    m_runtimeStatusStore->setBalance(m_balance, m_balanceKnown);

    QString displayName = userInfo.value(QStringLiteral("username")).toString().trimmed();
    if (displayName.isEmpty()) {
        displayName = userInfo.value(QStringLiteral("email")).toString().trimmed();
    }
    m_userLabel->setText(displayName.isEmpty()
        ? QStringLiteral("U") : displayName.left(1).toUpper());
    m_userLabel->setToolTip(displayName.isEmpty()
        ? QStringLiteral("账号中心：修改密码与密卡充值")
        : QStringLiteral("%1\n点击进入账号中心").arg(displayName));
    m_balanceButton->setToolTip(QStringLiteral("查看账号与 API Key 用量"));
}

void MainWindow::onUsageClicked()
{
    auto *dialog = new UsageDialog(m_apiClient, this);
    dialog->exec();
    dialog->deleteLater();
    refreshBalance();
}

void MainWindow::onAccountClicked()
{
    auto *dialog = new AccountDialog(m_apiClient, m_userInfo, this);
    connect(dialog, &AccountDialog::accountBalanceChanged,
            this, [this]() {
        refreshBalance();
        m_apiClient->getUserInfo();
    });
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onRequestFailed(const QString &error)
{
    if (m_authExpiredHandled) {
        return;
    }
    logMessage(QStringLiteral("请求失败：%1").arg(error), kLogError);
}

void MainWindow::onAuthenticationExpired()
{
    if (m_authExpiredHandled) {
        return;
    }
    m_authExpiredHandled = true;
    m_authToken.clear();
    if (m_balanceRefreshTimer) {
        m_balanceRefreshTimer->stop();
    }
    if (m_gatewayManager && m_gatewayManager->isRunning()) {
        QSettings().setValue(QStringLiteral("gateway/enabled"), false);
        m_gatewayManager->stop();
    }
    SecureStorage::clearToken();
    QMessageBox::information(
        this,
        QStringLiteral("登录已过期"),
        QStringLiteral("登录状态已过期，请重新登录 Aegisy 账号。"));
    m_quitting = true;
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
    emit loggedOut();
    close();
}

void MainWindow::onInstallOutput(AiTool tool, const QString &line)
{
    Q_UNUSED(tool);
    logMessage(line, kLogMuted);
}

void MainWindow::onInstallFinished(AiTool tool, int requestId, bool success)
{
    const int toolId = static_cast<int>(tool);
    m_installingTools.remove(toolId);
    if (QPushButton *button = m_toolInstallButtons.value(toolId, nullptr)) {
        button->setEnabled(true);
        button->setText(QStringLiteral("安装"));
    }
    if (m_pendingToolVersionChecks == 0) {
        QTimer::singleShot(0, this, &MainWindow::refreshToolVersions);
    }
    if (m_activatingIndex < 0) {
        logMessage(
            success
                ? QStringLiteral("%1 安装完成").arg(ToolManager::toolName(tool))
                : QStringLiteral("%1 安装失败").arg(ToolManager::toolName(tool)),
            success ? kLogSuccess : kLogError);
        return;
    }
    if (requestId != m_activationGeneration) {
        logMessage(
            QStringLiteral("已忽略过期的 %1 安装结果").arg(ToolManager::toolName(tool)),
            kLogMuted);
        return;
    }
    if (m_activationQueue.isEmpty() || m_activationQueue.first() != tool) {
        return;
    }

    if (!success) {
        logMessage(
            QStringLiteral("%1 安装失败，可手动运行 npm install -g %2")
                .arg(ToolManager::toolName(tool), ToolManager::npmPackage(tool)),
            kLogError);
    } else {
        logMessage(
            QStringLiteral("%1 安装完成").arg(ToolManager::toolName(tool)),
            kLogSuccess);
    }

    m_activationQueue.removeFirst();
    processActivationQueue();
}

void MainWindow::installToolEnvironment(AiTool tool)
{
    const int toolId = static_cast<int>(tool);
    if (m_installingTools.contains(toolId)) {
        return;
    }

    m_installingTools.insert(toolId);
    if (QPushButton *button = m_toolInstallButtons.value(toolId, nullptr)) {
        button->setEnabled(false);
        button->setText(QStringLiteral("..."));
    }
    logMessage(
        QStringLiteral("正在检查并安装 %1 运行环境...").arg(ToolManager::toolName(tool)),
        kLogInfo);
    m_toolManager->install(tool, -1);
}

void MainWindow::showEnvCheckDialog(int profileIndex)
{
    const QList<Profile> profiles = m_profileManager->allProfiles();
    if (profileIndex < 0 || profileIndex >= profiles.size()) {
        return;
    }

    const Profile &profile = profiles[profileIndex];
    if (!profile.hasAnyKey()) {
        return;
    }

    const AiTool tool = profile.tool();
    const ToolStatus status = m_toolManager->detectFast(tool);
    const DesktopAppStatus desktop = m_toolManager->detectDesktop(tool);
    if (status.installed && status.conflictWarning.isEmpty()) {
        logMessage(
            status.version.isEmpty()
                ? QStringLiteral("%1 本地环境已就绪").arg(ToolManager::toolName(tool))
                : QStringLiteral("%1 %2 本地环境已就绪")
                      .arg(ToolManager::toolName(tool), status.version),
            kLogSuccess);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("本地环境检查"));
    dialog.setMinimumWidth(520);
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #f6f7f9; }"
        "QLabel { color: #182230; }"));

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(24, 22, 24, 18);
    root->setSpacing(12);

    auto *title = new QLabel(
        QStringLiteral("%1 环境检查").arg(ToolManager::toolName(tool)), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 700; color: #101828;"));
    root->addWidget(title);

    auto addIssueRow = [&dialog, root](QStyle::StandardPixmap iconType,
                                      const QString &titleText,
                                      const QString &detailText,
                                      QPushButton *actionButton) {
        auto *row = new QFrame(&dialog);
        row->setStyleSheet(QStringLiteral(
            "QFrame { background: white; border: 1px solid #e4e7ec; border-radius: 8px; }"));
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(14, 12, 14, 12);
        rowLayout->setSpacing(12);
        auto *icon = new QLabel(row);
        icon->setPixmap(dialog.style()->standardIcon(iconType).pixmap(22, 22));
        rowLayout->addWidget(icon);
        auto *textColumn = new QVBoxLayout;
        textColumn->setSpacing(1);
        auto *rowTitle = new QLabel(titleText, row);
        rowTitle->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 700; color: #344054;"));
        auto *detail = new QLabel(detailText, row);
        detail->setWordWrap(true);
        detail->setStyleSheet(QStringLiteral("font-size: 11px; color: #667085;"));
        textColumn->addWidget(rowTitle);
        textColumn->addWidget(detail);
        rowLayout->addLayout(textColumn, 1);
        if (actionButton) {
            rowLayout->addWidget(actionButton);
        }
        root->addWidget(row);
    };

    if (!status.installed && !status.nodeOk) {
        auto *button = new QPushButton(QStringLiteral("一键安装环境"), &dialog);
        button->setFixedHeight(34);
        button->setStyleSheet(AppTheme::primaryButtonStyle());
        connect(button, &QPushButton::clicked, this, [this, tool, button]() {
            button->setEnabled(false);
            button->setText(QStringLiteral("安装中..."));
            installToolEnvironment(tool);
        });
        addIssueRow(QStyle::SP_MessageBoxWarning,
                    QStringLiteral("Node.js 未安装"),
                    QStringLiteral("将先安装 Node.js LTS，再安装对应 CLI。"),
                    button);
    } else if (!status.installed) {
        auto *button = new QPushButton(
            status.repairRequired ? QStringLiteral("修复 CLI")
                                  : QStringLiteral("安装 CLI"), &dialog);
        button->setFixedHeight(34);
        button->setStyleSheet(AppTheme::primaryButtonStyle());
        connect(button, &QPushButton::clicked, this, [this, tool, button]() {
            button->setEnabled(false);
            button->setText(QStringLiteral("安装中..."));
            installToolEnvironment(tool);
        });
        addIssueRow(QStyle::SP_MessageBoxWarning,
                    status.repairRequired ? QStringLiteral("CLI 安装损坏")
                                          : QStringLiteral("CLI 未安装"),
                    status.repairRequired
                        ? status.installationIssue
                        : QStringLiteral(
                            "认证文件仍会正常更新，安装后即可直接使用。"),
                    button);
    }

    if (!desktop.installed) {
        // Gemini 无官方桌面版：保持打开网页；Claude/ChatGPT 走服务器代理下载。
        const auto product = DesktopDownloader::proxiedProductForTool(tool);
        if (product) {
            auto *button = new QPushButton(QStringLiteral("下载并安装"), &dialog);
            button->setFixedHeight(34);
            button->setStyleSheet(AppTheme::primaryButtonStyle());
            const QString appName = desktop.appName;
            const QString token = m_authToken;
            connect(button, &QPushButton::clicked, &dialog,
                    [this, product = *product, appName, token]() {
                auto *dl = new DesktopDownloader(
                    product, appName, token, m_apiClient->baseUrl(), this);
                dl->exec();
                dl->deleteLater();
            });
            addIssueRow(QStyle::SP_DesktopIcon,
                        QStringLiteral("%1 未安装").arg(desktop.appName),
                        QStringLiteral("Aegisy 美国节点 · 受认证流式代理"),
                        button);
        } else {
            auto *button = new QPushButton(QStringLiteral("打开下载页"), &dialog);
            button->setFixedHeight(34);
            button->setStyleSheet(AppTheme::secondaryButtonStyle());
            const QString url = desktop.downloadUrl;
            connect(button, &QPushButton::clicked, [url]() {
                QDesktopServices::openUrl(QUrl(url));
            });
            addIssueRow(QStyle::SP_DesktopIcon,
                        QStringLiteral("%1 未安装").arg(desktop.appName),
                        QStringLiteral("桌面客户端为可选项，不影响 CLI 配置。"),
                        button);
        }
    }

    if (!status.conflictWarning.isEmpty()) {
        addIssueRow(QStyle::SP_MessageBoxWarning,
                    QStringLiteral("检测到环境变量冲突"),
                    status.conflictWarning,
                    nullptr);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("继续"));
    buttons->button(QDialogButtonBox::Ok)->setStyleSheet(AppTheme::primaryButtonStyle());
    buttons->button(QDialogButtonBox::Ok)->setFixedHeight(36);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    root->addWidget(buttons);
    dialog.exec();
}

void MainWindow::onManageKeysClicked()
{
    auto *dialog = new ApiKeysDialog(m_apiClient, this);
    dialog->exec();
    dialog->deleteLater();
    m_apiClient->getApiKeys();
}

void MainWindow::onCardConnectionTested(const QString &requestId, bool success,
                                        const QString &detail, int latencyMs)
{
    auto it = m_cardTestWidgets.find(requestId);
    if (it == m_cardTestWidgets.end()) return;

    StatusBadge *label  = it.value().first;
    QAction *button = it.value().second;

    if (label) {
        label->setState(
            success ? QStringLiteral("%1 · %2 ms").arg(detail).arg(latencyMs) : detail,
            success ? StatusBadge::Tone::Success : StatusBadge::Tone::Error,
            style()->standardIcon(success
                ? QStyle::SP_DialogApplyButton : QStyle::SP_MessageBoxCritical));
        label->show();
    }
    if (button) {
        button->setEnabled(true);
        button->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    }
}

void MainWindow::onViewModelsClicked()
{
    auto *dialog = new ModelsDialog(m_apiClient, this);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onImageGenerationClicked()
{
    auto *dialog = new ImageGenerationDialog(m_apiClient, this);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onChatClicked()
{
    auto *dialog = new ChatDialog(m_apiClient, m_skillManager, m_profileManager,
                                  m_runtimeStatusStore, this);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onSkillsClicked()
{
    auto *dialog = new SkillsDialog(m_skillManager, this);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onMcpConfigClicked()
{
    auto *dialog = new McpConfigDialog(this);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onHelpClicked()
{
    auto *dialog = new HelpDialog(this);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onSystemDoctorClicked()
{
    auto *dialog = new SystemDoctorDialog(m_toolManager, this);
    dialog->exec();
    dialog->deleteLater();
    refreshToolVersions();
}

void MainWindow::onGatewayClicked()
{
    auto *dialog = new GatewayDialog(m_gatewayManager, this);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onDesktopEnhancementsClicked()
{
    auto *dialog = new DesktopEnhancementDialog(m_desktopEnhancementManager, this);
    connect(dialog, &DesktopEnhancementDialog::openModelsRequested,
            this, &MainWindow::onViewModelsClicked);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onDesktopDownloadClicked()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("下载桌面端"));
    dialog.setMinimumWidth(520);
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background: #f6f7f9; }"
        "QLabel { color: #182230; }"));

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(24, 22, 24, 18);
    root->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("下载桌面客户端"), &dialog);
    title->setStyleSheet(
        QStringLiteral("font-size: 17px; font-weight: 700; color: #101828;"));
    root->addWidget(title);

    auto addRow = [&dialog, root](const QString &titleText,
                                  const QString &detailText,
                                  QWidget *actionWidget) {
        auto *row = new QFrame(&dialog);
        row->setStyleSheet(QStringLiteral(
            "QFrame { background: white; border: 1px solid #e4e7ec; border-radius: 8px; }"));
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(14, 12, 14, 12);
        rowLayout->setSpacing(12);
        auto *icon = new QLabel(row);
        icon->setPixmap(dialog.style()->standardIcon(QStyle::SP_DesktopIcon).pixmap(22, 22));
        rowLayout->addWidget(icon);
        auto *textColumn = new QVBoxLayout;
        textColumn->setSpacing(1);
        auto *rowTitle = new QLabel(titleText, row);
        rowTitle->setStyleSheet(
            QStringLiteral("font-size: 12px; font-weight: 700; color: #344054;"));
        auto *detail = new QLabel(detailText, row);
        detail->setWordWrap(true);
        detail->setStyleSheet(QStringLiteral("font-size: 11px; color: #667085;"));
        textColumn->addWidget(rowTitle);
        textColumn->addWidget(detail);
        rowLayout->addLayout(textColumn, 1);
        if (actionWidget) {
            rowLayout->addWidget(actionWidget);
        }
        root->addWidget(row);
    };

    for (const AiTool tool : { AiTool::ClaudeCode, AiTool::CodexCli }) {
        const DesktopAppStatus desktop = m_toolManager->detectDesktop(tool);
        if (desktop.installed) {
            auto *badge = new StatusBadge(&dialog);
            badge->setState(
                QStringLiteral("已安装"), StatusBadge::Tone::Success,
                style()->standardIcon(QStyle::SP_DialogApplyButton));
            addRow(desktop.appName,
                   QStringLiteral("已检测到本机安装"), badge);
            continue;
        }

        const auto product = DesktopDownloader::proxiedProductForTool(tool);
        if (product) {
            auto *button = new QPushButton(QStringLiteral("下载并安装"), &dialog);
            button->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
            button->setFixedHeight(34);
            button->setStyleSheet(AppTheme::primaryButtonStyle());
            const QString appName = desktop.appName;
            const QString token = m_authToken;
            connect(button, &QPushButton::clicked, &dialog,
                    [this, product = *product, appName, token]() {
                auto *dl = new DesktopDownloader(
                    product, appName, token, m_apiClient->baseUrl(), this);
                dl->exec();
                dl->deleteLater();
            });
            addRow(desktop.appName,
                   QStringLiteral("Aegisy 美国节点 · 受认证流式代理"),
                   button);
        } else {
            auto *button = new QPushButton(QStringLiteral("打开下载页"), &dialog);
            button->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
            button->setFixedHeight(34);
            button->setStyleSheet(AppTheme::secondaryButtonStyle());
            const QString url = desktop.downloadUrl;
            connect(button, &QPushButton::clicked, [url]() {
                QDesktopServices::openUrl(QUrl(url));
            });
            addRow(desktop.appName,
                   QStringLiteral("当前平台仅提供官方下载页"), button);
        }
    }

    auto *closeButton = new QPushButton(QStringLiteral("关闭"), &dialog);
    closeButton->setFixedHeight(34);
    closeButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    auto *footer = new QHBoxLayout;
    footer->addStretch();
    footer->addWidget(closeButton);
    root->addLayout(footer);

    dialog.exec();
}

void MainWindow::onGatewayRunningChanged(bool running)
{
    if (m_gatewayButton) {
        m_gatewayButton->setText(running ? QStringLiteral("网关运行中")
                                         : QStringLiteral("本地网关"));
    }
    const bool enabled = QSettings().value(
        QStringLiteral("gateway/enabled"), false).toBool();
    const QList<Profile> profiles = m_profileManager->allProfiles();
    if (running && enabled) {
        for (ProfileType type : allProfileTypes()) {
            const int index = m_profileManager->activeIndex(type);
            if (index < 0 || index >= profiles.size()) continue;
            const Profile profile = m_profileManager->profileWithCredential(index);
            if (profile.key.isEmpty()) continue;
            if (!m_gatewayManager->configureProfile(profile.tool(), profile.key)
                    || !m_toolManager->configureGateway(
                        profile.tool(), m_gatewayManager->localToken(), profile.model)) {
                logMessage(QStringLiteral("%1 切换到本地网关失败：%2")
                    .arg(ToolManager::toolName(profile.tool()), m_toolManager->lastError()),
                    kLogError);
            }
        }
        logMessage(QStringLiteral("本地网关已启动，仅监听 127.0.0.1:43112"), kLogSuccess);
    } else if (!running) {
        if (enabled) {
            QSettings().setValue(QStringLiteral("gateway/enabled"), false);
            logMessage(QStringLiteral("本地网关意外停止，正在恢复直接连接"), kLogWarn);
        }
        for (ProfileType type : allProfileTypes()) {
            const int index = m_profileManager->activeIndex(type);
            if (index < 0 || index >= profiles.size()) continue;
            const Profile profile = m_profileManager->profileWithCredential(index);
            if (profile.key.isEmpty()) continue;
            if (!m_toolManager->configure(profile.tool(), profile.key, profile.model)) {
                logMessage(QStringLiteral("%1 恢复直接连接失败：%2")
                    .arg(ToolManager::toolName(profile.tool()), m_toolManager->lastError()),
                    kLogError);
            } else if (!m_toolManager->lastWarning().isEmpty()) {
                logMessage(m_toolManager->lastWarning(), kLogWarn);
            }
        }
        logMessage(QStringLiteral("已关闭本地网关并恢复直接连接配置"), kLogInfo);
    }
    updateRuntimeProfileStatus();
    refreshGatewayPage();
    refreshGatewayLogs();
    rebuildCards();
    rebuildTrayMenu();
}

void MainWindow::onBackupsClicked()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("配置备份与恢复"));
    dialog.resize(580, 430);
    dialog.setMinimumSize(520, 380);
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(24, 22, 24, 18);
    root->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("配置备份历史"), &dialog);
    title->setStyleSheet(QStringLiteral(
        "font-size: 18px; font-weight: 700; color: #101828;"));
    root->addWidget(title);
    auto *hint = new QLabel(
        QStringLiteral("每次激活前自动备份，按终端保留最近 10 次。恢复前也会先保存当前配置。"),
        &dialog);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    root->addWidget(hint);

    auto *toolCombo = new QComboBox(&dialog);
    toolCombo->addItem(QStringLiteral("Claude Code"), static_cast<int>(AiTool::ClaudeCode));
    toolCombo->addItem(QStringLiteral("Codex CLI"), static_cast<int>(AiTool::CodexCli));
    toolCombo->addItem(QStringLiteral("Gemini CLI"), static_cast<int>(AiTool::GeminiCli));
    toolCombo->addItem(QStringLiteral("OpenCode"), static_cast<int>(AiTool::OpenCode));
    Profile active;
    if (m_filterType != 0) {
        active = m_profileManager->activeProfile(static_cast<ProfileType>(m_filterType));
    } else {
        for (ProfileType type : allProfileTypes()) {
            active = m_profileManager->activeProfile(type);
            if (active.index >= 0) {
                break;
            }
        }
    }
    if (active.index >= 0) {
        toolCombo->setCurrentIndex(
            toolCombo->findData(static_cast<int>(active.tool())));
    }
    root->addWidget(toolCombo);

    auto *list = new QListWidget(&dialog);
    list->setAlternatingRowColors(true);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(list, 1);

    auto *buttonRow = new QHBoxLayout;
    auto *status = new QLabel(&dialog);
    status->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    buttonRow->addWidget(status);
    buttonRow->addStretch();
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), &dialog);
    closeButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    auto *restoreButton = new QPushButton(QStringLiteral("恢复所选备份"), &dialog);
    restoreButton->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    restoreButton->setStyleSheet(AppTheme::primaryButtonStyle());
    restoreButton->setEnabled(false);
    buttonRow->addWidget(closeButton);
    buttonRow->addWidget(restoreButton);
    root->addLayout(buttonRow);

    const auto reload = [this, toolCombo, list, status, restoreButton]() {
        list->clear();
        const AiTool tool = static_cast<AiTool>(toolCombo->currentData().toInt());
        const ConfigBackupInventory inventory = m_toolManager->backupInventory(tool);
        for (const ConfigBackup &backup : inventory.backups) {
            auto *item = new QListWidgetItem(
                QStringLiteral("%1    %2 个配置文件")
                    .arg(backup.createdAt.toLocalTime().toString(
                             QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                    .arg(backup.fileCount),
                list);
            item->setData(Qt::UserRole, backup.id);
        }
        bool subsystemReady = false;
        switch (inventory.state) {
        case ConfigBackupSubsystemState::Ready:
            subsystemReady = true;
            status->setText(QStringLiteral("共 %1 个备份").arg(inventory.backups.size()));
            status->setStyleSheet(QStringLiteral("font-size: 12px; color: #067647;"));
            break;
        case ConfigBackupSubsystemState::Empty:
            status->setText(QStringLiteral("暂无备份"));
            status->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
            break;
        case ConfigBackupSubsystemState::Unavailable:
            status->setText(QStringLiteral("备份功能暂不可用（%1）")
                .arg(inventory.errorCode.isEmpty()
                    ? QStringLiteral("configuration-backup-unavailable")
                    : inventory.errorCode));
            status->setStyleSheet(QStringLiteral("font-size: 12px; color: #b54708;"));
            break;
        case ConfigBackupSubsystemState::Invalid:
            status->setText(QStringLiteral("备份存储校验失败（%1）")
                .arg(inventory.errorCode.isEmpty()
                    ? QStringLiteral("configuration-backup-invalid")
                    : inventory.errorCode));
            status->setStyleSheet(QStringLiteral("font-size: 12px; color: #b42318;"));
            break;
        }
        restoreButton->setProperty("backupSubsystemReady", subsystemReady);
        restoreButton->setEnabled(false);
    };

    connect(toolCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            &dialog, [reload](int) { reload(); });
    connect(list, &QListWidget::itemSelectionChanged, &dialog,
            [list, restoreButton]() {
        restoreButton->setEnabled(
            restoreButton->property("backupSubsystemReady").toBool()
            && list->currentItem() != nullptr);
    });
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(restoreButton, &QPushButton::clicked, &dialog,
            [this, toolCombo, list, &dialog]() {
        QListWidgetItem *item = list->currentItem();
        if (!item) {
            return;
        }
        const AiTool tool = static_cast<AiTool>(toolCombo->currentData().toInt());
        const auto answer = QMessageBox::question(
            &dialog,
            QStringLiteral("恢复配置"),
            QStringLiteral("确定恢复所选的 %1 配置吗？当前配置会先自动备份。")
                .arg(ToolManager::toolName(tool)),
            QMessageBox::Yes | QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
        if (!m_toolManager->restoreBackup(item->data(Qt::UserRole).toString(), tool)) {
            QMessageBox::critical(
                &dialog, QStringLiteral("恢复失败"), m_toolManager->lastError());
            return;
        }
        m_profileManager->clearActiveProfile(profileTypeForTool(tool));
        refreshConfigurationWatchers();
        rebuildCards();
        logMessage(
            QStringLiteral("%1 配置已从备份恢复").arg(ToolManager::toolName(tool)),
            kLogSuccess);
        if (m_toolManager->lastWarning().isEmpty()) {
            QMessageBox::information(
                &dialog, QStringLiteral("恢复完成"), QStringLiteral("本地配置已恢复。"));
        } else {
            logMessage(m_toolManager->lastWarning(), kLogWarn);
            QMessageBox::warning(
                &dialog, QStringLiteral("恢复完成"),
                QStringLiteral("本地配置已恢复，但备份保留清理未完成：%1")
                    .arg(m_toolManager->lastWarning()));
        }
        dialog.accept();
    });

    reload();
    dialog.exec();
}

void MainWindow::onTransferClicked()
{
    QMenu menu(this);
    QAction *exportAction = menu.addAction(
        style()->standardIcon(QStyle::SP_DialogSaveButton), QStringLiteral("导出加密档案"));
    QAction *importAction = menu.addAction(
        style()->standardIcon(QStyle::SP_DialogOpenButton), QStringLiteral("导入加密档案"));
    QAction *selected = menu.exec(
        m_transferButton->mapToGlobal(QPoint(0, m_transferButton->height())));
    if (!selected) {
        return;
    }

    if (selected == exportAction) {
        QString filePath = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("导出加密档案"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                + QStringLiteral("/aegisy-profiles.aegisy"),
            QStringLiteral("Aegisy 加密档案 (*.aegisy)"));
        if (filePath.isEmpty()) {
            return;
        }
        if (!filePath.endsWith(QStringLiteral(".aegisy"), Qt::CaseInsensitive)) {
            filePath += QStringLiteral(".aegisy");
        }

        bool ok = false;
        const QString password = QInputDialog::getText(
            this, QStringLiteral("设置导出密码"),
            QStringLiteral("密码用于 AES-256-GCM 加密，至少 8 个字符："),
            QLineEdit::Password, QString(), &ok);
        if (!ok) {
            return;
        }
        if (password.size() < 8) {
            QMessageBox::information(
                this, QStringLiteral("密码过短"), QStringLiteral("导出密码至少需要 8 个字符。"));
            return;
        }
        const QString confirmation = QInputDialog::getText(
            this, QStringLiteral("确认导出密码"),
            QStringLiteral("请再次输入导出密码："),
            QLineEdit::Password, QString(), &ok);
        if (!ok) {
            return;
        }
        if (password != confirmation) {
            QMessageBox::warning(
                this, QStringLiteral("密码不一致"), QStringLiteral("两次输入的密码不一致。"));
            return;
        }
        if (!m_profileManager->exportProfiles(filePath, password)) {
            QMessageBox::critical(
                this, QStringLiteral("导出失败"), m_profileManager->lastError());
            return;
        }
        logMessage(QStringLiteral("档案已加密导出到 %1").arg(filePath), kLogSuccess);
        QMessageBox::information(
            this, QStringLiteral("导出完成"),
            QStringLiteral("档案已使用 AES-256-GCM 加密导出。请妥善保存密码。"));
        return;
    }

    if (selected == importAction) {
        const QString filePath = QFileDialog::getOpenFileName(
            this,
            QStringLiteral("导入加密档案"),
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
            QStringLiteral("Aegisy 加密档案 (*.aegisy)"));
        if (filePath.isEmpty()) {
            return;
        }
        bool ok = false;
        const QString password = QInputDialog::getText(
            this, QStringLiteral("输入导入密码"),
            QStringLiteral("请输入该档案的导出密码："),
            QLineEdit::Password, QString(), &ok);
        if (!ok) {
            return;
        }
        int importedCount = 0;
        if (!m_profileManager->importProfiles(
                filePath, password, &importedCount)) {
            QMessageBox::critical(
                this, QStringLiteral("导入失败"), m_profileManager->lastError());
            return;
        }
        rebuildCards();
        logMessage(
            QStringLiteral("已导入 %1 个档案").arg(importedCount), kLogSuccess);
        QMessageBox::information(
            this, QStringLiteral("导入完成"),
            QStringLiteral("已安全导入 %1 个档案。").arg(importedCount));
    }
}

void MainWindow::onLogoutClicked()
{
    const auto reply = QMessageBox::question(
        this,
        QStringLiteral("退出登录"),
        QStringLiteral("确定要退出当前账号吗？"),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    SecureStorage::clearToken();
    m_quitting = true;
    if (m_trayIcon) {
        m_trayIcon->hide();
    }
    emit loggedOut();
    close();
}
