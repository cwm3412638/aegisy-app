#include "main_window.h"

#include "api_keys_dialog.h"
#include "account_dialog.h"
#include "connect_wizard.h"
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

#include <QButtonGroup>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QApplication>
#include <QCloseEvent>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>

namespace {

QString toolAccent(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("#c15f3c");
    case AiTool::CodexCli:   return QStringLiteral("#111827");
    case AiTool::GeminiCli:  return QStringLiteral("#1a73e8");
    }
    return QStringLiteral("#0f766e");
}

QString toolSoftColor(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("#fff4ef");
    case AiTool::CodexCli:   return QStringLiteral("#f3f4f6");
    case AiTool::GeminiCli:  return QStringLiteral("#eef5ff");
    }
    return QStringLiteral("#ecfdf5");
}

QString toolLetter(AiTool tool)
{
    switch (tool) {
    case AiTool::ClaudeCode: return QStringLiteral("C");
    case AiTool::CodexCli:   return QStringLiteral("O");
    case AiTool::GeminiCli:  return QStringLiteral("G");
    }
    return QStringLiteral("A");
}

QString toolConfigPath(AiTool tool)
{
    return ToolManager::configFilePath(tool);
}

const QString kLogSuccess = QStringLiteral("#067647");
const QString kLogError   = QStringLiteral("#b42318");
const QString kLogInfo    = QStringLiteral("#175cd3");
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
{
    setupUi();
    setWindowTitle(QStringLiteral("Aegisy - AI 工具连接管理"));
    resize(1080, 720);
    setMinimumSize(1024, 620);

    connect(m_apiClient, &ApiClient::apiKeysReceived,
            this, &MainWindow::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::userInfoReceived,
            this, &MainWindow::onUserInfoReceived);
    connect(m_apiClient, &ApiClient::requestFailed,
            this, &MainWindow::onRequestFailed);
    connect(m_apiClient, &ApiClient::authenticationExpired,
            this, &MainWindow::onAuthenticationExpired);
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
    connect(m_gatewayManager, &GatewayManager::gatewayError,
            this, [this](const QString &error) {
        logMessage(QStringLiteral("本地网关错误：%1").arg(error), kLogError);
    });
    connect(m_profileManager, &ProfileManager::profilesChanged,
            this, &MainWindow::rebuildTrayMenu);
    connect(m_profileManager, &ProfileManager::activeProfileChanged,
            this, [this](int, int) { rebuildTrayMenu(); });

    refreshToolVersions();
    rebuildCards();
    setupTray();
    if (!m_profileManager->lastError().isEmpty()) {
        logMessage(m_profileManager->lastError(), kLogError);
    }
}

MainWindow::~MainWindow()
{
    if (m_gatewayManager && m_gatewayManager->isRunning()) {
        m_gatewayManager->stop();
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
        QAction *section = m_trayMenu->addAction(profileTypeName(type));
        section->setEnabled(false);
        bool hasProfile = false;
        for (const Profile &profile : profiles) {
            if (profile.type != type) {
                continue;
            }
            hasProfile = true;
            QAction *profileAction = m_trayMenu->addAction(profile.name);
            profileAction->setCheckable(true);
            profileAction->setChecked(
                profile.index == activeIndex && profile.hasAnyKey());
            profileAction->setEnabled(profile.hasAnyKey());
            const int profileIndex = profile.index;
            connect(profileAction, &QAction::triggered, this,
                    [this, profileIndex]() { activateProfile(profileIndex); });
        }
        if (!hasProfile) {
            QAction *empty = m_trayMenu->addAction(QStringLiteral("  暂无档案"));
            empty->setEnabled(false);
        } else if (activeIndex >= 0 && activeIndex < profiles.size()
                   && profiles[activeIndex].type == type
                   && profiles[activeIndex].hasAnyKey()) {
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
    QMainWindow::closeEvent(event);
}

void MainWindow::setAuthToken(const QString &token)
{
    m_authToken = token;
    m_apiClient->setAuthToken(token);
    logMessage(QStringLiteral("正在同步账号 API Keys..."), kLogInfo);
    m_apiClient->getApiKeys();
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
            }
        });
    }
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
        displayText = QStringLiteral("未安装");
        tooltip = QStringLiteral("未检测到 %1").arg(ToolManager::toolName(tool));
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
            ? QStringLiteral("...") : QStringLiteral("安装"));
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
    topBar->setFixedHeight(68);
    topBar->setStyleSheet(QStringLiteral(
        "QFrame#topBar { background: white; border-bottom: 1px solid #e4e7ec; }"));
    auto *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(22, 12, 22, 12);
    topLayout->setSpacing(10);

    auto *brandMark = new QLabel(QStringLiteral("A"), topBar);
    brandMark->setFixedSize(38, 38);
    brandMark->setAlignment(Qt::AlignCenter);
    brandMark->setStyleSheet(QStringLiteral(
        "background: #0f766e; color: white; border-radius: 8px;"
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
        "QPushButton { color: white; background: #0f766e; border: none;"
        "border-radius: 19px; font-size: 14px; font-weight: 700; }"
        "QPushButton:hover { background: #115e59; }"));
    topLayout->addWidget(m_userLabel);

    m_balanceButton = new QPushButton(QStringLiteral("余额  --"), topBar);
    m_balanceButton->setToolTip(QStringLiteral("查看账号与 API Key 用量"));
    m_balanceButton->setFixedHeight(36);
    m_balanceButton->setMinimumWidth(112);
    m_balanceButton->setMaximumWidth(150);
    m_balanceButton->setCursor(Qt::PointingHandCursor);
    m_balanceButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: #eff8ff; color: #175cd3; border: 1px solid #b2ddff;"
        "border-radius: 7px; padding: 0 10px; font-size: 11px; font-weight: 700; }"
        "QPushButton:hover { background: #dff0ff; border-color: #84caff; }"));
    topLayout->addWidget(m_balanceButton);

    m_manageKeysButton = new QPushButton(QStringLiteral("API Keys"), topBar);
    m_manageKeysButton->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    m_viewModelsButton = new QPushButton(QStringLiteral("模型"), topBar);
    m_viewModelsButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogListView));
    m_imageGenerationButton = new QPushButton(QStringLiteral("生图"), topBar);
    m_imageGenerationButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    m_backupsButton = new QPushButton(QStringLiteral("备份"), topBar);
    m_backupsButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_transferButton = new QPushButton(QStringLiteral("迁移"), topBar);
    m_transferButton->setIcon(style()->standardIcon(QStyle::SP_DirLinkIcon));
    m_checkUpdatesButton = new QPushButton(QStringLiteral("更新"), topBar);
    m_checkUpdatesButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));

    m_manageKeysButton->setFixedWidth(92);
    m_viewModelsButton->setFixedWidth(70);
    m_imageGenerationButton->setFixedWidth(70);
    m_backupsButton->setFixedWidth(70);
    m_transferButton->setFixedWidth(70);
    m_checkUpdatesButton->setFixedWidth(70);

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
             m_manageKeysButton, m_viewModelsButton, m_imageGenerationButton, m_backupsButton,
             m_transferButton, m_checkUpdatesButton }) {
        button->setFixedHeight(36);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(AppTheme::secondaryButtonStyle());
        topLayout->addWidget(button);
    }

    m_logoutButton = new QPushButton(topBar);
    m_logoutButton->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    m_logoutButton->setToolTip(QStringLiteral("退出登录"));
    m_logoutButton->setFixedSize(36, 36);
    m_logoutButton->setCursor(Qt::PointingHandCursor);
    m_logoutButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: white; border: 1px solid #fecdca; border-radius: 7px; }"
        "QPushButton:hover { background: #fef3f2; border-color: #f04438; }"));
    topLayout->addWidget(m_logoutButton);
    root->addWidget(topBar);

    auto *body = new QWidget(central);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    auto *sidebar = new QFrame(body);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(210);
    sidebar->setStyleSheet(QStringLiteral(
        "QFrame#sidebar { background: #fbfcfd; border-right: 1px solid #e4e7ec; }"));
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(16, 22, 16, 16);
    sideLayout->setSpacing(6);

    auto *filterLabel = new QLabel(QStringLiteral("工具筛选"), sidebar);
    filterLabel->setStyleSheet(QStringLiteral(
        "font-size: 11px; font-weight: 700; color: #667085; padding: 0 8px 6px 8px;"));
    sideLayout->addWidget(filterLabel);

    m_filterGroup = new QButtonGroup(this);
    m_filterGroup->setExclusive(true);
    const struct {
        int id;
        const char *label;
        QStyle::StandardPixmap icon;
    } filters[] = {
        { 0, "全部配置", QStyle::SP_DirHomeIcon },
        { static_cast<int>(ProfileType::Claude), "Claude Code", QStyle::SP_FileDialogDetailedView },
        { static_cast<int>(ProfileType::Codex),  "Codex CLI", QStyle::SP_CommandLink },
        { static_cast<int>(ProfileType::Gemini), "Gemini CLI", QStyle::SP_FileDialogInfoView },
    };

    const QString filterStyle = QStringLiteral(
        "QPushButton {"
        "  background: transparent; color: #475467; border: none; border-radius: 7px;"
        "  text-align: left; padding-left: 10px; padding-right: 10px; font-size: 13px;"
        "}"
        "QPushButton:hover { background: #f0f2f5; color: #182230; }"
        "QPushButton:checked { background: #e7f5f2; color: #0f5f59; font-weight: 700; }");
    for (const auto &filter : filters) {
        auto *button = new QPushButton(QString::fromUtf8(filter.label), sidebar);
        button->setCheckable(true);
        button->setChecked(filter.id == 0);
        button->setFixedHeight(40);
        button->setIcon(style()->standardIcon(filter.icon));
        button->setIconSize(QSize(16, 16));
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(filterStyle);
        m_filterGroup->addButton(button, filter.id);
        sideLayout->addWidget(button);
    }
    sideLayout->addSpacing(14);

    auto *terminalHeader = new QHBoxLayout;
    terminalHeader->setContentsMargins(8, 0, 2, 0);
    auto *terminalTitle = new QLabel(QStringLiteral("本地终端"), sidebar);
    terminalTitle->setStyleSheet(QStringLiteral(
        "font-size: 11px; font-weight: 700; color: #667085;"));
    terminalHeader->addWidget(terminalTitle);
    terminalHeader->addStretch();
    m_refreshToolVersionsButton = new QPushButton(sidebar);
    m_refreshToolVersionsButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_refreshToolVersionsButton->setToolTip(QStringLiteral("刷新本地终端版本"));
    m_refreshToolVersionsButton->setFixedSize(26, 26);
    m_refreshToolVersionsButton->setCursor(Qt::PointingHandCursor);
    m_refreshToolVersionsButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; border-radius: 6px; }"
        "QPushButton:hover { background: #eaecf0; }"
        "QPushButton:disabled { background: transparent; }"));
    terminalHeader->addWidget(m_refreshToolVersionsButton);
    sideLayout->addLayout(terminalHeader);

    const struct {
        AiTool tool;
        const char *name;
    } terminals[] = {
        { AiTool::ClaudeCode, "Claude" },
        { AiTool::CodexCli, "Codex" },
        { AiTool::GeminiCli, "Gemini" },
    };
    for (const auto &terminal : terminals) {
        auto *row = new QWidget(sidebar);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(8, 2, 4, 2);
        rowLayout->setSpacing(6);
        auto *name = new QLabel(QString::fromUtf8(terminal.name), row);
        name->setStyleSheet(QStringLiteral("font-size: 11px; color: #475467;"));
        rowLayout->addWidget(name);
        rowLayout->addStretch();
        auto *version = new QLabel(QStringLiteral("检测中..."), row);
        version->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        version->setStyleSheet(QStringLiteral(
            "font-family: monospace; font-size: 10px; color: #98a2b3;"));
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
            "QPushButton:disabled { color: #98a2b3; background: #f2f4f7; border-color: #e4e7ec; }"));
        installButton->hide();
        rowLayout->addWidget(installButton);
        m_toolInstallButtons.insert(static_cast<int>(terminal.tool), installButton);
        connect(installButton, &QPushButton::clicked, this,
                [this, tool = terminal.tool]() { installToolEnvironment(tool); });
        sideLayout->addWidget(row);
    }

    m_doctorButton = new QPushButton(QStringLiteral("系统体检"), sidebar);
    m_doctorButton->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    m_doctorButton->setToolTip(QStringLiteral("检查系统依赖、CLI、配置与安全存储"));
    m_doctorButton->setFixedHeight(34);
    m_doctorButton->setCursor(Qt::PointingHandCursor);
    m_doctorButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    sideLayout->addWidget(m_doctorButton);

    m_gatewayButton = new QPushButton(QStringLiteral("本地网关"), sidebar);
    m_gatewayButton->setIcon(style()->standardIcon(QStyle::SP_DriveNetIcon));
    m_gatewayButton->setToolTip(QStringLiteral("本地转发、快速档案切换与请求监控"));
    m_gatewayButton->setFixedHeight(34);
    m_gatewayButton->setCursor(Qt::PointingHandCursor);
    m_gatewayButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    sideLayout->addWidget(m_gatewayButton);

    m_desktopEnhancementsButton = new QPushButton(QStringLiteral("桌面增强"), sidebar);
    m_desktopEnhancementsButton->setIcon(style()->standardIcon(QStyle::SP_DesktopIcon));
    m_desktopEnhancementsButton->setToolTip(
        QStringLiteral("全量模型、Codex 插件、历史会话、Computer Use 与 Claude 汉化"));
    m_desktopEnhancementsButton->setFixedHeight(34);
    m_desktopEnhancementsButton->setCursor(Qt::PointingHandCursor);
    m_desktopEnhancementsButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    sideLayout->addWidget(m_desktopEnhancementsButton);

    m_chatButton = new QPushButton(QStringLiteral("AI 对话"), sidebar);
    m_chatButton->setIcon(style()->standardIcon(QStyle::SP_MessageBoxInformation));
    m_chatButton->setToolTip(QStringLiteral("选择 API Key 和模型进行流式对话"));
    m_chatButton->setFixedHeight(34);
    m_chatButton->setCursor(Qt::PointingHandCursor);
    m_chatButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    sideLayout->addWidget(m_chatButton);

    m_skillsButton = new QPushButton(QStringLiteral("Skills"), sidebar);
    m_skillsButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    m_skillsButton->setToolTip(QStringLiteral("安装、启用和管理对话自动调用的 Skills"));
    m_skillsButton->setFixedHeight(34);
    m_skillsButton->setCursor(Qt::PointingHandCursor);
    m_skillsButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    sideLayout->addWidget(m_skillsButton);

    sideLayout->addStretch();

    auto *localTitle = new QLabel(QStringLiteral("认证文件"), sidebar);
    localTitle->setStyleSheet(QStringLiteral(
        "font-size: 11px; font-weight: 700; color: #667085; padding: 0 8px;"));
    sideLayout->addWidget(localTitle);
    auto *localPaths = new QLabel(
        QStringLiteral("Claude  settings.json\nCodex    auth.json\nGemini   .env"), sidebar);
    localPaths->setStyleSheet(QStringLiteral(
        "font-family: monospace; font-size: 10px; color: #98a2b3;"
        "background: #f2f4f7; border-radius: 7px; padding: 10px;"));
    sideLayout->addWidget(localPaths);
    bodyLayout->addWidget(sidebar);

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

    m_newConnectButton = new QPushButton(QStringLiteral("新建配置"), content);
    m_newConnectButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    m_newConnectButton->setFixedHeight(40);
    m_newConnectButton->setCursor(Qt::PointingHandCursor);
    m_newConnectButton->setStyleSheet(AppTheme::primaryButtonStyle());
    headingRow->addWidget(m_newConnectButton);
    contentLayout->addLayout(headingRow);

    auto *summaryRow = new QHBoxLayout;
    summaryRow->setSpacing(8);
    m_profileCountLabel = new QLabel(content);
    m_profileCountLabel->setStyleSheet(QStringLiteral(
        "background: white; color: #475467; border: 1px solid #e4e7ec;"
        "border-radius: 7px; padding: 6px 10px; font-size: 11px;"));
    summaryRow->addWidget(m_profileCountLabel);
    m_activeProfileLabel = new QLabel(content);
    m_activeProfileLabel->setStyleSheet(QStringLiteral(
        "background: #ecfdf3; color: #067647; border: 1px solid #abefc6;"
        "border-radius: 7px; padding: 6px 10px; font-size: 11px;"));
    summaryRow->addWidget(m_activeProfileLabel);
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
    contentLayout->addLayout(activityHeader);

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
    contentLayout->addWidget(m_logOutput);

    bodyLayout->addWidget(content, 1);
    root->addWidget(body, 1);

    connect(m_logoutButton, &QPushButton::clicked,
            this, &MainWindow::onLogoutClicked);
    connect(m_newConnectButton, &QPushButton::clicked,
            this, &MainWindow::onNewConnectClicked);
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
    connect(m_chatButton, &QPushButton::clicked,
            this, &MainWindow::onChatClicked);
    connect(m_skillsButton, &QPushButton::clicked,
            this, &MainWindow::onSkillsClicked);
    connect(m_balanceButton, &QPushButton::clicked,
            this, &MainWindow::onUsageClicked);
    connect(m_userLabel, &QPushButton::clicked,
            this, &MainWindow::onAccountClicked);
    m_balanceRefreshTimer = new QTimer(this);
    m_balanceRefreshTimer->setInterval(60 * 1000);
    connect(m_balanceRefreshTimer, &QTimer::timeout,
            this, &MainWindow::refreshBalance);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_filterGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &MainWindow::onFilterChanged);
#else
    connect(m_filterGroup, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, &MainWindow::onFilterChanged);
#endif
    connect(clearLogButton, &QPushButton::clicked, m_logOutput, &QTextEdit::clear);
}

void MainWindow::rebuildCards()
{
    QLayoutItem *item = nullptr;
    while ((item = m_cardsLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }

    const QList<Profile> profiles = m_profileManager->allProfiles();
    m_profileCountLabel->setText(QStringLiteral("配置总数  %1").arg(profiles.size()));

    QStringList activeDescriptions;
    for (ProfileType type : allProfileTypes()) {
        const int activeIndex = m_profileManager->activeIndex(type);
        if (activeIndex >= 0 && activeIndex < profiles.size()
                && profiles[activeIndex].hasAnyKey()) {
            activeDescriptions.append(
                QStringLiteral("%1：%2").arg(profileTypeName(type), profiles[activeIndex].name));
        }
    }
    if (activeDescriptions.isEmpty()) {
        m_activeProfileLabel->setText(QStringLiteral("尚未激活有效配置"));
        m_activeProfileLabel->setToolTip(QString());
    } else {
        m_activeProfileLabel->setText(
            QStringLiteral("已激活  %1 / 3 个终端").arg(activeDescriptions.size()));
        m_activeProfileLabel->setToolTip(activeDescriptions.join(QLatin1Char('\n')));
    }

    int visibleCount = 0;
    for (const Profile &profile : profiles) {
        if (m_filterType != 0 && static_cast<int>(profile.type) != m_filterType) {
            continue;
        }
        const bool isActive = m_profileManager->isActive(profile.index)
            && profile.hasAnyKey();
        m_cardsLayout->addWidget(createProfileCard(profile, isActive));
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

QWidget *MainWindow::createProfileCard(const Profile &profile, bool isActive)
{
    const AiTool tool = profile.tool();
    const QString accent = toolAccent(tool);
    const QString background = isActive ? toolSoftColor(tool) : QStringLiteral("#ffffff");

    auto *card = new QFrame(m_cardsContainer);
    card->setObjectName(QStringLiteral("profileCard"));
    card->setMinimumHeight(124);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    card->setStyleSheet(QStringLiteral(
        "QFrame#profileCard {"
        "  background: %1; border: 1px solid #e4e7ec; border-left: 4px solid %2;"
        "  border-radius: 8px;"
        "}").arg(background, accent));

    auto *layout = new QHBoxLayout(card);
    layout->setContentsMargins(16, 14, 14, 14);
    layout->setSpacing(14);

    auto *badge = new QLabel(toolLetter(tool), card);
    badge->setFixedSize(46, 46);
    badge->setAlignment(Qt::AlignCenter);
    badge->setStyleSheet(QStringLiteral(
        "background: %1; color: white; border: none; border-radius: 8px;"
        "font-size: 18px; font-weight: 700;").arg(accent));
    layout->addWidget(badge);

    auto *details = new QVBoxLayout;
    details->setSpacing(4);
    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    auto *nameLabel = new QLabel(profile.name, card);
    nameLabel->setToolTip(profile.name);
    nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    nameLabel->setStyleSheet(QStringLiteral(
        "font-size: 14px; font-weight: 700; color: #101828; border: none;"));
    titleRow->addWidget(nameLabel);
    auto *typeBadge = new QLabel(profileTypeName(profile.type), card);
    typeBadge->setStyleSheet(QStringLiteral(
        "background: %1; color: %2; border: 1px solid %2;"
        "border-radius: 6px; padding: 2px 7px; font-size: 10px; font-weight: 700;")
        .arg(toolSoftColor(tool), accent));
    titleRow->addWidget(typeBadge);
    if (isActive) {
        auto *activeBadge = new QLabel(QStringLiteral("当前使用"), card);
        activeBadge->setStyleSheet(QStringLiteral(
            "background: #dcfae6; color: #067647; border: 1px solid #abefc6;"
            "border-radius: 6px; padding: 2px 7px; font-size: 10px; font-weight: 700;"));
        titleRow->addWidget(activeBadge);
    }
    titleRow->addStretch();
    details->addLayout(titleRow);

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
    auto *keyLabel = new QLabel(
        profile.hasAnyKey() ? QStringLiteral("Key：已安全保存")
                            : QStringLiteral("Key：未配置"), card);
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
    launchButton->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    launchButton->setFixedSize(82, 38);
    launchButton->setCursor(Qt::PointingHandCursor);
    launchButton->setEnabled(isActive && profile.hasAnyKey());
    launchButton->setToolTip(isActive
        ? QStringLiteral("自动检测系统终端并启动 %1")
            .arg(ToolManager::toolName(tool))
        : QStringLiteral("请先激活该配置"));
    launchButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    const int profileIndex = profile.index;
    connect(launchButton, &QPushButton::clicked,
            this, [this, profileIndex]() { launchProfile(profileIndex, false); });
    layout->addWidget(launchButton);

    auto *activateButton = new QPushButton(
        isActive ? QStringLiteral("已激活") : QStringLiteral("激活"), card);
    activateButton->setIcon(style()->standardIcon(
        isActive ? QStyle::SP_DialogApplyButton : QStyle::SP_MediaPlay));
    activateButton->setFixedSize(98, 38);
    activateButton->setCursor(Qt::PointingHandCursor);
    activateButton->setEnabled(!isActive && profile.hasAnyKey());
    activateButton->setStyleSheet(isActive
        ? QStringLiteral(
            "QPushButton { background: #dcfae6; color: #067647; border: 1px solid #abefc6;"
            "border-radius: 7px; font-size: 12px; font-weight: 600; }")
        : AppTheme::primaryButtonStyle());
    connect(activateButton, &QPushButton::clicked,
            this, [this, profileIndex]() { activateProfile(profileIndex); });
    layout->addWidget(activateButton);

    auto *editButton = new QPushButton(card);
    editButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    editButton->setToolTip(QStringLiteral("编辑配置"));
    editButton->setFixedSize(38, 38);
    editButton->setCursor(Qt::PointingHandCursor);
    editButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    connect(editButton, &QPushButton::clicked,
            this, [this, profileIndex]() { editProfile(profileIndex); });
    layout->addWidget(editButton);

    auto *deleteButton = new QPushButton(card);
    deleteButton->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
    deleteButton->setToolTip(QStringLiteral("删除配置"));
    deleteButton->setFixedSize(38, 38);
    deleteButton->setCursor(Qt::PointingHandCursor);
    deleteButton->setEnabled(m_profileManager->count() > 1);
    deleteButton->setStyleSheet(AppTheme::dangerButtonStyle());
    connect(deleteButton, &QPushButton::clicked,
            this, [this, profileIndex]() { deleteProfile(profileIndex); });
    layout->addWidget(deleteButton);

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

    const QString directory = QDir::homePath();

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
        "QPushButton:hover { background: #f0fdf9; color: #0f5f59; border-color: #0f766e; }"));
    connect(button, &QPushButton::clicked,
            this, &MainWindow::onNewConnectClicked);
    return button;
}

void MainWindow::onFilterChanged(int typeId)
{
    m_filterType = typeId;
    rebuildCards();
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

    if (!confirmConfigurationPreview(profile)) {
        logMessage(QStringLiteral("已取消激活「%1」").arg(profile.name), kLogMuted);
        return;
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
    return box.exec() == QMessageBox::Ok;
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
        logMessage(
            QStringLiteral("%1 已写入 %2")
                .arg(ToolManager::toolName(tool), toolConfigPath(tool)),
            kLogSuccess);
    } else {
        logMessage(
            QStringLiteral("%1 配置写入失败：%2")
                .arg(ToolManager::toolName(tool), m_toolManager->lastError()),
            kLogError);
    }
    return success;
}

void MainWindow::onApiKeysReceived(const QJsonArray &keys)
{
    m_keys = keys;
    m_keysLoaded = true;
    logMessage(QStringLiteral("已同步 %1 个 API Key").arg(keys.size()), kLogSuccess);
}

void MainWindow::onUserInfoReceived(const QJsonObject &userInfo)
{
    m_userInfo = userInfo;
    const double balance = userInfo.value(QStringLiteral("balance")).toDouble();
    const QString formatted = QLocale(QLocale::English).toString(balance, 'f', 2);
    m_balanceButton->setText(QStringLiteral("余额  $%1").arg(formatted));

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
        auto *button = new QPushButton(QStringLiteral("安装 CLI"), &dialog);
        button->setFixedHeight(34);
        button->setStyleSheet(AppTheme::primaryButtonStyle());
        connect(button, &QPushButton::clicked, this, [this, tool, button]() {
            button->setEnabled(false);
            button->setText(QStringLiteral("安装中..."));
            installToolEnvironment(tool);
        });
        addIssueRow(QStyle::SP_MessageBoxWarning,
                    QStringLiteral("CLI 未安装"),
                    QStringLiteral("认证文件仍会正常更新，安装后即可直接使用。"),
                    button);
    }

    if (!desktop.installed) {
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
    auto *dialog = new ChatDialog(m_apiClient, m_skillManager, this);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onSkillsClicked()
{
    auto *dialog = new SkillsDialog(m_skillManager, this);
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
            }
        }
        logMessage(QStringLiteral("已关闭本地网关并恢复直接连接配置"), kLogInfo);
    }
    rebuildCards();
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
        const QList<ConfigBackup> history = m_toolManager->backupHistory(tool);
        for (const ConfigBackup &backup : history) {
            auto *item = new QListWidgetItem(
                QStringLiteral("%1    %2 个配置文件")
                    .arg(backup.createdAt.toLocalTime().toString(
                             QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                    .arg(backup.fileCount),
                list);
            item->setData(Qt::UserRole, backup.id);
        }
        status->setText(history.isEmpty()
            ? QStringLiteral("暂无备份")
            : QStringLiteral("共 %1 个备份").arg(history.size()));
        restoreButton->setEnabled(false);
    };

    connect(toolCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            &dialog, [reload](int) { reload(); });
    connect(list, &QListWidget::itemSelectionChanged, &dialog,
            [list, restoreButton]() {
        restoreButton->setEnabled(list->currentItem() != nullptr);
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
        rebuildCards();
        logMessage(
            QStringLiteral("%1 配置已从备份恢复").arg(ToolManager::toolName(tool)),
            kLogSuccess);
        QMessageBox::information(
            &dialog, QStringLiteral("恢复完成"), QStringLiteral("本地配置已恢复。"));
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
