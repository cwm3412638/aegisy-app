#include "main_window.h"
#include "api_keys_dialog.h"
#include "models_dialog.h"
#include "secure_storage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QInputDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QJsonObject>
#include <QFont>

static const QString kSiteUrl = "https://www.aegisy.cc";

// ── 按工具返回主题色 ──────────────────────────────────────────
static QString toolAccent(AiTool tool) {
    switch (tool) {
        case AiTool::ClaudeCode: return "#6366f1";
        case AiTool::CodexCli:  return "#3b82f6";
        case AiTool::GeminiCli: return "#0d9488";
        default:                return "#6366f1";
    }
}
static QString toolAccentHover(AiTool tool) {
    switch (tool) {
        case AiTool::ClaudeCode: return "#4f46e5";
        case AiTool::CodexCli:  return "#2563eb";
        case AiTool::GeminiCli: return "#0f766e";
        default:                return "#4f46e5";
    }
}
static QString toolEmoji(AiTool tool) {
    switch (tool) {
        case AiTool::ClaudeCode: return "🤖";
        case AiTool::CodexCli:  return "⚡";
        case AiTool::GeminiCli: return "💎";
        default:                return "🔧";
    }
}

// ── 日志颜色（适配深色终端背景）────────────────────────────────
static const QString kLogSuccess = "#4ade80";
static const QString kLogError   = "#f87171";
static const QString kLogInfo    = "#818cf8";
static const QString kLogWarn    = "#fb923c";
static const QString kLogMuted   = "#94a3b8";

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_apiClient(new ApiClient(this))
    , m_toolManager(new ToolManager(this))
    , m_profileManager(new ProfileManager(this))
{
    setupUi();
    setWindowTitle("Aegisy 客户端 — AI 工具一键接入");
    resize(700, 660);

    connect(m_apiClient, &ApiClient::apiKeysReceived, this, &MainWindow::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::requestFailed, this, &MainWindow::onRequestFailed);
    connect(m_toolManager, &ToolManager::installOutput, this, &MainWindow::onInstallOutput);
    connect(m_toolManager, &ToolManager::installFinished, this, &MainWindow::onInstallFinished);
}

MainWindow::~MainWindow()
{
}

void MainWindow::setAuthToken(const QString &token)
{
    m_authToken = token;
    m_apiClient->setAuthToken(token);

    logMessage("正在获取账号 API Keys...", kLogInfo);
    m_apiClient->getApiKeys();

    refreshAllCards();
}

void MainWindow::setupUi()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    central->setStyleSheet("background-color: #f1f5f9;");

    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // ===== 顶部导航栏 =====
    QFrame *topBar = new QFrame(this);
    topBar->setStyleSheet(
        "QFrame {"
        "  background: white;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 10px;"
        "}"
    );
    QHBoxLayout *topBarLayout = new QHBoxLayout(topBar);
    topBarLayout->setContentsMargins(16, 10, 16, 10);
    topBarLayout->setSpacing(10);

    // Logo 徽标
    QLabel *logoBadge = new QLabel("A", this);
    logoBadge->setFixedSize(32, 32);
    logoBadge->setAlignment(Qt::AlignCenter);
    logoBadge->setStyleSheet(
        "QLabel {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
        "    stop:0 #6366f1, stop:1 #8b5cf6);"
        "  color: white;"
        "  border-radius: 8px;"
        "  font-size: 16px;"
        "  font-weight: bold;"
        "}"
    );
    topBarLayout->addWidget(logoBadge);

    QLabel *appTitle = new QLabel("Aegisy", this);
    QFont appTitleFont = appTitle->font();
    appTitleFont.setPointSize(14);
    appTitleFont.setBold(true);
    appTitle->setFont(appTitleFont);
    appTitle->setStyleSheet("color: #1e293b;");
    topBarLayout->addWidget(appTitle);

    // 竖分隔线
    QFrame *profileDiv = new QFrame(this);
    profileDiv->setFrameShape(QFrame::VLine);
    profileDiv->setFixedHeight(18);
    profileDiv->setStyleSheet("color: #e2e8f0;");
    topBarLayout->addWidget(profileDiv);

    QLabel *profileLabel = new QLabel("档案", this);
    profileLabel->setStyleSheet("color: #94a3b8; font-size: 12px;");
    topBarLayout->addWidget(profileLabel);

    // 档案下拉框
    m_profileCombo = new QComboBox(this);
    m_profileCombo->setMinimumWidth(120);
    m_profileCombo->setMaximumWidth(160);
    m_profileCombo->setMinimumHeight(28);
    m_profileCombo->setCursor(Qt::PointingHandCursor);
    m_profileCombo->setStyleSheet(
        "QComboBox {"
        "  background: #f8fafc;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 6px;"
        "  padding: 2px 8px;"
        "  font-size: 13px;"
        "  color: #1e293b;"
        "}"
        "QComboBox:focus { border-color: #6366f1; }"
        "QComboBox::drop-down { border: none; padding-right: 6px; }"
        "QComboBox QAbstractItemView {"
        "  border: 1.5px solid #e2e8f0;"
        "  background: white;"
        "  selection-background-color: #eef2ff;"
        "  selection-color: #3730a3;"
        "}"
    );
    topBarLayout->addWidget(m_profileCombo);

    const QString iconBtnStyle =
        "QPushButton {"
        "  background: transparent;"
        "  color: #64748b;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 6px;"
        "  font-size: 14px;"
        "  padding: 2px 7px;"
        "}"
        "QPushButton:hover {"
        "  background: #f1f5f9;"
        "  color: #1e293b;"
        "}";

    m_addProfileButton = new QPushButton("+", this);
    m_addProfileButton->setFixedSize(28, 28);
    m_addProfileButton->setCursor(Qt::PointingHandCursor);
    m_addProfileButton->setToolTip("新建档案");
    m_addProfileButton->setStyleSheet(iconBtnStyle);
    topBarLayout->addWidget(m_addProfileButton);

    m_manageProfileButton = new QPushButton("⋯", this);
    m_manageProfileButton->setFixedSize(28, 28);
    m_manageProfileButton->setCursor(Qt::PointingHandCursor);
    m_manageProfileButton->setToolTip("管理档案（重命名 / 删除）");
    m_manageProfileButton->setStyleSheet(iconBtnStyle);
    topBarLayout->addWidget(m_manageProfileButton);

    topBarLayout->addStretch();

    m_userLabel = new QLabel(this);
    m_userLabel->setStyleSheet("color: #64748b; font-size: 13px;");
    topBarLayout->addWidget(m_userLabel);

    m_logoutButton = new QPushButton("退出登录", this);
    m_logoutButton->setCursor(Qt::PointingHandCursor);
    m_logoutButton->setMinimumHeight(30);
    m_logoutButton->setStyleSheet(
        "QPushButton {"
        "  background: transparent;"
        "  color: #ef4444;"
        "  border: 1.5px solid #fca5a5;"
        "  border-radius: 6px;"
        "  padding: 3px 12px;"
        "  font-size: 13px;"
        "}"
        "QPushButton:hover {"
        "  background: #fef2f2;"
        "  border-color: #ef4444;"
        "}"
    );
    topBarLayout->addWidget(m_logoutButton);

    mainLayout->addWidget(topBar);

    // 副标题
    QLabel *subtitle = new QLabel("选择你要使用的 AI 工具，点击「一键接入」即可完成全部配置", this);
    subtitle->setStyleSheet("color: #64748b; font-size: 13px; padding: 0 4px;");
    mainLayout->addWidget(subtitle);

    // ===== 三张工具卡片 =====
    mainLayout->addWidget(createToolCard(AiTool::ClaudeCode));
    mainLayout->addWidget(createToolCard(AiTool::CodexCli));
    mainLayout->addWidget(createToolCard(AiTool::GeminiCli));

    // ===== 高级功能栏 =====
    QFrame *advFrame = new QFrame(this);
    advFrame->setStyleSheet(
        "QFrame {"
        "  background: white;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 8px;"
        "}"
    );
    QHBoxLayout *advBar = new QHBoxLayout(advFrame);
    advBar->setContentsMargins(16, 8, 16, 8);
    advBar->setSpacing(10);

    QLabel *advLabel = new QLabel("高级功能", this);
    advLabel->setStyleSheet("color: #94a3b8; font-size: 12px;");
    advBar->addWidget(advLabel);

    // 竖分隔线
    QFrame *sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setFixedHeight(16);
    sep->setStyleSheet("color: #e2e8f0;");
    advBar->addWidget(sep);

    m_manageKeysButton = new QPushButton("🔑  我的 API Keys", this);
    m_viewModelsButton = new QPushButton("📋  查看模型", this);
    const QString advBtnStyle =
        "QPushButton {"
        "  background: transparent;"
        "  color: #6366f1;"
        "  border: 1.5px solid #e0e7ff;"
        "  border-radius: 6px;"
        "  padding: 4px 14px;"
        "  font-size: 13px;"
        "}"
        "QPushButton:hover {"
        "  background: #eef2ff;"
        "  border-color: #6366f1;"
        "}";
    for (QPushButton *b : {m_manageKeysButton, m_viewModelsButton}) {
        b->setStyleSheet(advBtnStyle);
        b->setCursor(Qt::PointingHandCursor);
        advBar->addWidget(b);
    }
    advBar->addStretch();
    mainLayout->addWidget(advFrame);

    // ===== 日志输出（深色终端风格）=====
    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumHeight(106);
    m_logOutput->setStyleSheet(
        "QTextEdit {"
        "  background-color: #0f172a;"
        "  border: 1.5px solid #1e293b;"
        "  border-radius: 8px;"
        "  font-family: 'Courier New', Consolas, monospace;"
        "  font-size: 11px;"
        "  color: #94a3b8;"
        "  padding: 6px 10px;"
        "}"
    );
    mainLayout->addWidget(m_logOutput);

    connect(m_logoutButton, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);
    connect(m_manageKeysButton, &QPushButton::clicked, this, &MainWindow::onManageKeysClicked);
    connect(m_viewModelsButton, &QPushButton::clicked, this, &MainWindow::onViewModelsClicked);

    // 档案信号
    refreshProfileCombo();
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onProfileComboChanged);
    connect(m_addProfileButton,    &QPushButton::clicked, this, &MainWindow::onAddProfileClicked);
    connect(m_manageProfileButton, &QPushButton::clicked, this, &MainWindow::onManageProfileClicked);
}

QWidget* MainWindow::createToolCard(AiTool tool)
{
    const QString accent      = toolAccent(tool);
    const QString accentHover = toolAccentHover(tool);
    const QString emoji       = toolEmoji(tool);

    // 白色卡片容器
    QFrame *box = new QFrame(this);
    box->setStyleSheet(
        "QFrame {"
        "  background: white;"
        "  border: 1.5px solid #e2e8f0;"
        "  border-radius: 10px;"
        "}"
    );

    QVBoxLayout *outerV = new QVBoxLayout(box);
    outerV->setSpacing(0);
    outerV->setContentsMargins(0, 0, 0, 0);

    // 顶部主题色细条
    QFrame *accentBar = new QFrame(box);
    accentBar->setFixedHeight(4);
    accentBar->setStyleSheet(QString(
        "QFrame {"
        "  background: %1;"
        "  border: none;"
        "  border-top-left-radius: 9px;"
        "  border-top-right-radius: 9px;"
        "}"
    ).arg(accent));
    outerV->addWidget(accentBar);

    // 内容区
    QVBoxLayout *v = new QVBoxLayout();
    v->setSpacing(8);
    v->setContentsMargins(16, 12, 16, 14);
    outerV->addLayout(v);

    // 工具名行
    QLabel *nameLabel = new QLabel(emoji + "  " + ToolManager::toolName(tool), box);
    QFont nameFont = nameLabel->font();
    nameFont.setPointSize(13);
    nameFont.setBold(true);
    nameLabel->setFont(nameFont);
    nameLabel->setStyleSheet(QString("color: %1;").arg(accent));
    v->addWidget(nameLabel);

    ToolCard card;

    // 状态文字
    card.statusLabel = new QLabel("检测中...", box);
    card.statusLabel->setStyleSheet("color: #64748b; font-size: 12px;");
    card.statusLabel->setWordWrap(true);
    v->addWidget(card.statusLabel);

    // 冲突警告（默认隐藏）
    card.warnLabel = new QLabel(box);
    card.warnLabel->setStyleSheet(
        "QLabel {"
        "  color: #92400e;"
        "  font-size: 12px;"
        "  background: #fffbeb;"
        "  border: 1px solid #fde68a;"
        "  border-radius: 5px;"
        "  padding: 5px 10px;"
        "}"
    );
    card.warnLabel->setWordWrap(true);
    card.warnLabel->setVisible(false);
    v->addWidget(card.warnLabel);

    // 按钮行
    QHBoxLayout *btnRow = new QHBoxLayout();
    btnRow->addStretch();

    QPushButton *guideButton = new QPushButton("使用说明 →", box);
    guideButton->setCursor(Qt::PointingHandCursor);
    guideButton->setStyleSheet(
        "QPushButton {"
        "  background: transparent;"
        "  color: #94a3b8;"
        "  border: none;"
        "  font-size: 12px;"
        "}"
        "QPushButton:hover { color: #6366f1; }"
    );
    connect(guideButton, &QPushButton::clicked, this, []() {
        QDesktopServices::openUrl(QUrl(kSiteUrl));
    });
    btnRow->addWidget(guideButton);

    btnRow->addSpacing(10);

    card.actionButton = new QPushButton("🚀  一键接入", box);
    card.actionButton->setMinimumHeight(34);
    card.actionButton->setMinimumWidth(130);
    card.actionButton->setCursor(Qt::PointingHandCursor);
    card.actionButton->setStyleSheet(
        QString(
            "QPushButton {"
            "  background: %1;"
            "  color: white;"
            "  border: none;"
            "  border-radius: 7px;"
            "  font-size: 13px;"
            "  font-weight: bold;"
            "  padding: 6px 18px;"
            "}"
            "QPushButton:hover { background: %2; }"
            "QPushButton:pressed { background: %2; }"
            "QPushButton:disabled { background: #e2e8f0; color: #94a3b8; }"
        ).arg(accent, accentHover)
    );
    connect(card.actionButton, &QPushButton::clicked, this, [this, tool]() {
        onConnectToolClicked(tool);
    });
    btnRow->addWidget(card.actionButton);

    v->addLayout(btnRow);

    m_cards.insert(tool, card);
    return box;
}

void MainWindow::refreshAllCards()
{
    refreshToolCard(AiTool::ClaudeCode);
    refreshToolCard(AiTool::CodexCli);
    refreshToolCard(AiTool::GeminiCli);
}

void MainWindow::refreshToolCard(AiTool tool)
{
    const ToolCard &card = m_cards.value(tool);
    if (!card.statusLabel) return;

    const ToolStatus status = m_toolManager->detect(tool);

    QStringList parts;
    if (!status.nodeOk) {
        parts << "✗ 未检测到 Node.js";
    } else if (status.installed) {
        parts << "✓ 已安装";
    } else {
        parts << "✗ 未安装（可一键安装）";
    }

    if (status.configured) {
        parts << QString("✓ 已接入 (Key: %1)").arg(maskKey(status.configuredKey));
        card.statusLabel->setStyleSheet("color: #16a34a; font-size: 12px;");
        card.actionButton->setText("重新接入");
    } else {
        parts << "○ 未接入";
        card.statusLabel->setStyleSheet("color: #64748b; font-size: 12px;");
        card.actionButton->setText("🚀  一键接入");
    }
    card.statusLabel->setText(parts.join("  ·  "));

    card.warnLabel->setVisible(!status.conflictWarning.isEmpty());
    card.warnLabel->setText(status.conflictWarning.isEmpty()
                            ? QString()
                            : "⚠️  " + status.conflictWarning);
}

QString MainWindow::maskKey(const QString &key)
{
    if (key.length() > 12) {
        return key.left(8) + "..." + key.right(4);
    }
    return key;
}

void MainWindow::onApiKeysReceived(const QJsonArray &keys)
{
    m_keys = keys;
    m_keysLoaded = true;
    logMessage(QString("✓ 已获取 %1 个 API Key").arg(keys.size()), kLogSuccess);
}

void MainWindow::onRequestFailed(const QString &error)
{
    logMessage(QString("✗ 请求失败：%1").arg(error), kLogError);
}

QList<MainWindow::KeyChoice> MainWindow::keysForTool(AiTool tool) const
{
    const QString platform = ToolManager::toolPlatform(tool);
    QList<KeyChoice> choices;

    for (const QJsonValue &val : m_keys) {
        const QJsonObject obj = val.toObject();
        if (obj["status"].toString() != "active") continue;
        const QString key = obj["key"].toString();
        if (key.isEmpty()) continue;

        const QJsonObject group = obj["group"].toObject();
        if (group["platform"].toString() != platform) continue;

        KeyChoice c;
        c.key = key;
        const QString name = obj["name"].toString();
        c.label = QString("%1 — %2 (%3)")
                      .arg(name.isEmpty() ? QStringLiteral("(未命名)") : name,
                           group["name"].toString(),
                           maskKey(key));
        choices.append(c);
    }
    return choices;
}

void MainWindow::onConnectToolClicked(AiTool tool)
{
    const ToolCard &card = m_cards.value(tool);
    const ToolStatus status = m_toolManager->detect(tool);

    // 1) Node.js 前置检查
    if (!status.nodeOk) {
        QMessageBox msg(this);
        msg.setWindowTitle("需要先安装 Node.js");
        msg.setIcon(QMessageBox::Information);
        msg.setText(QString("%1 依赖 Node.js（建议 LTS v20+），检测到你的电脑尚未安装。\n\n"
                            "Windows 用户可在 PowerShell 中运行：\n"
                            "    winget install OpenJS.NodeJS.LTS\n\n"
                            "或点击下方按钮到官网下载安装包。安装完成后重启本程序。")
                        .arg(ToolManager::toolName(tool)));
        QPushButton *openBtn = msg.addButton("打开 Node.js 官网", QMessageBox::AcceptRole);
        msg.addButton("我知道了", QMessageBox::RejectRole);
        msg.exec();
        if (msg.clickedButton() == openBtn) {
            QDesktopServices::openUrl(QUrl("https://nodejs.org/"));
        }
        return;
    }

    // 2) Key 前置：按分组筛选，用户选择
    if (!m_keysLoaded) {
        logMessage("API Keys 尚未加载完成，请稍候再试...", kLogWarn);
        m_apiClient->getApiKeys();
        return;
    }
    const QList<KeyChoice> choices = keysForTool(tool);
    if (choices.isEmpty()) {
        const QString platform = ToolManager::toolPlatform(tool);
        QMessageBox msg(this);
        msg.setWindowTitle("没有可用的 API Key");
        msg.setIcon(QMessageBox::Information);
        msg.setText(QString("你的账号还没有「%1」分组的可用 API Key。\n\n"
                            "请到 aegisy.cc 官网 → 控制台 → API Keys，"
                            "创建一个 %1 分组的 Key 后回来重试。")
                        .arg(platform));
        QPushButton *openBtn = msg.addButton("打开官网创建", QMessageBox::AcceptRole);
        msg.addButton("取消", QMessageBox::RejectRole);
        msg.exec();
        if (msg.clickedButton() == openBtn) {
            QDesktopServices::openUrl(QUrl(kSiteUrl));
        }
        return;
    }

    QStringList labels;
    for (const KeyChoice &c : choices) {
        labels << c.label;
    }
    bool ok = false;
    const QString picked = QInputDialog::getItem(
        this,
        QString("选择 API Key — %1").arg(ToolManager::toolName(tool)),
        "将使用以下 API Key 接入（可下拉更换）：",
        labels, 0, false, &ok);
    if (!ok) {
        return;
    }
    const int idx = labels.indexOf(picked);
    m_pendingChoice.insert(tool, choices.value(idx < 0 ? 0 : idx));

    card.actionButton->setEnabled(false);

    // 3) 未安装 → 先一键安装
    if (!status.installed) {
        logMessage(QString("开始安装 %1 ...").arg(ToolManager::toolName(tool)), kLogInfo);
        m_toolManager->install(tool);
        return;
    }

    // 4) 已安装 → 直接配置
    configureTool(tool);
}

void MainWindow::onInstallOutput(AiTool tool, const QString &line)
{
    Q_UNUSED(tool);
    logMessage(line, kLogMuted);
}

void MainWindow::onInstallFinished(AiTool tool, bool success)
{
    if (!success) {
        logMessage(QString("✗ %1 安装失败，请检查网络或手动运行：npm install -g %2")
                       .arg(ToolManager::toolName(tool), ToolManager::npmPackage(tool)),
                   kLogError);
        m_pendingChoice.remove(tool);
        const ToolCard &card = m_cards.value(tool);
        if (card.actionButton) card.actionButton->setEnabled(true);
        return;
    }
    logMessage(QString("✓ %1 安装完成").arg(ToolManager::toolName(tool)), kLogSuccess);
    configureTool(tool);
}

void MainWindow::configureTool(AiTool tool)
{
    const ToolCard &card = m_cards.value(tool);

    const KeyChoice choice = m_pendingChoice.take(tool);
    if (choice.key.isEmpty()) {
        if (card.actionButton) card.actionButton->setEnabled(true);
        return;
    }
    const QString apiKey   = choice.key;
    const QString keyLabel = choice.label;

    logMessage(QString("正在写入 %1 配置（%2）...").arg(ToolManager::toolName(tool), keyLabel), kLogInfo);

    const bool ok = m_toolManager->configure(tool, apiKey);
    if (ok) {
        // 将 Key 保存到当前档案，下次切换回来可自动恢复
        m_profileManager->saveKey(m_profileManager->activeIndex(), tool, apiKey);

        logMessage(QString("🎉 %1 接入完成！重新打开终端后运行 `%2` 即可使用。")
                       .arg(ToolManager::toolName(tool), ToolManager::cliCommand(tool)),
                   kLogSuccess);
        QMessageBox::information(this, "接入成功",
            QString("%1 已成功接入 Aegisy！\n\n"
                    "使用的 Key：%2\n\n"
                    "请重新打开终端，运行 `%3` 开始使用。")
                .arg(ToolManager::toolName(tool), keyLabel, ToolManager::cliCommand(tool)));
    } else {
        logMessage(QString("✗ %1 配置写入失败：%2")
                       .arg(ToolManager::toolName(tool), m_toolManager->lastError()),
                   kLogError);
        QMessageBox::warning(this, "接入失败",
            QString("%1 配置写入失败：\n%2")
                .arg(ToolManager::toolName(tool), m_toolManager->lastError()));
    }

    if (card.actionButton) card.actionButton->setEnabled(true);
    refreshToolCard(tool);
}

void MainWindow::onManageKeysClicked()
{
    ApiKeysDialog *dialog = new ApiKeysDialog(m_apiClient, this);
    dialog->exec();
    dialog->deleteLater();
    m_apiClient->getApiKeys();
}

void MainWindow::onViewModelsClicked()
{
    ModelsDialog *dialog = new ModelsDialog(m_apiClient, this);
    dialog->exec();
    dialog->deleteLater();
}

void MainWindow::onLogoutClicked()
{
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "退出登录", "确定要退出登录吗？",
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    SecureStorage::clearToken();
    emit loggedOut();
    close();
}

void MainWindow::logMessage(const QString &message, const QString &color)
{
    m_logOutput->append(QString("<span style='color:%1'>%2</span>").arg(color, message));
}

// ── 档案管理 ──────────────────────────────────────────────────

void MainWindow::refreshProfileCombo()
{
    m_profileCombo->blockSignals(true);
    m_profileCombo->clear();
    const auto profiles = m_profileManager->allProfiles();
    for (const Profile &p : profiles) {
        m_profileCombo->addItem(p.name);
    }
    m_profileCombo->setCurrentIndex(m_profileManager->activeIndex());
    m_profileCombo->blockSignals(false);
}

void MainWindow::applyProfile(const Profile &profile)
{
    bool anyApplied = false;
    for (AiTool tool : {AiTool::ClaudeCode, AiTool::CodexCli, AiTool::GeminiCli}) {
        const QString key = profile.keyFor(tool);
        if (key.isEmpty()) continue;

        const ToolStatus status = m_toolManager->detect(tool);
        if (!status.installed) {
            logMessage(QString("跳过 %1（未安装）").arg(ToolManager::toolName(tool)), kLogMuted);
            continue;
        }
        if (m_toolManager->configure(tool, key)) {
            logMessage(QString("✓ %1 已切换至档案「%2」")
                       .arg(ToolManager::toolName(tool), profile.name), kLogSuccess);
            anyApplied = true;
        } else {
            logMessage(QString("✗ %1 切换失败：%2")
                       .arg(ToolManager::toolName(tool), m_toolManager->lastError()), kLogError);
        }
    }
    if (!anyApplied && profile.hasAnyKey()) {
        logMessage("档案中的工具均未安装，无法自动切换配置", kLogMuted);
    }
    refreshAllCards();
}

void MainWindow::onProfileComboChanged(int index)
{
    if (index < 0) return;
    m_profileManager->setActiveIndex(index);
    const Profile profile = m_profileManager->activeProfile();
    logMessage(QString("已切换到档案「%1」").arg(profile.name), kLogInfo);
    applyProfile(profile);
}

void MainWindow::onAddProfileClicked()
{
    const int nextNum = m_profileManager->count() + 1;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, "新建档案", "档案名称：",
        QLineEdit::Normal, QString("档案 %1").arg(nextNum), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    const int newIdx = m_profileManager->addProfile(name.trimmed());
    refreshProfileCombo();

    // 切换到新建档案（blockSignals 避免触发 applyProfile，新档案没有 Key）
    m_profileCombo->blockSignals(true);
    m_profileCombo->setCurrentIndex(newIdx);
    m_profileCombo->blockSignals(false);
    m_profileManager->setActiveIndex(newIdx);
    logMessage(QString("已创建档案「%1」，请为各工具点击「一键接入」选择 Key").arg(name.trimmed()), kLogInfo);
}

void MainWindow::onManageProfileClicked()
{
    const int idx = m_profileManager->activeIndex();
    if (idx < 0) return;
    const auto profiles = m_profileManager->allProfiles();
    const QString currentName = profiles.value(idx).name;

    QMenu menu(this);
    QAction *renameAct = menu.addAction(QString("✏️  重命名「%1」").arg(currentName));
    menu.addSeparator();
    QAction *deleteAct = menu.addAction(QString("🗑  删除「%1」").arg(currentName));
    deleteAct->setEnabled(profiles.size() > 1);

    QAction *chosen = menu.exec(
        m_manageProfileButton->mapToGlobal(m_manageProfileButton->rect().bottomLeft()));
    if (!chosen) return;

    if (chosen == renameAct) {
        bool ok = false;
        const QString newName = QInputDialog::getText(
            this, "重命名档案", "新名称：",
            QLineEdit::Normal, currentName, &ok);
        if (ok && !newName.trimmed().isEmpty()) {
            m_profileManager->renameProfile(idx, newName.trimmed());
            refreshProfileCombo();
        }
    } else if (chosen == deleteAct) {
        const auto reply = QMessageBox::question(
            this, "删除档案",
            QString("确定删除档案「%1」吗？\n该档案保存的 Key 将一并删除。").arg(currentName));
        if (reply == QMessageBox::Yes) {
            m_profileManager->removeProfile(idx);
            refreshProfileCombo();
            applyProfile(m_profileManager->activeProfile());
        }
    }
}
