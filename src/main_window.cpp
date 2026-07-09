#include "main_window.h"
#include "api_keys_dialog.h"
#include "models_dialog.h"
#include "connect_wizard.h"
#include "secure_storage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QJsonObject>
#include <QFont>
#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QProgressDialog>

static const QString kSiteUrl = "https://www.aegisy.cc";

// ── 按工具返回主题色 / 图标 ─────────────────────────────────────
static QString toolAccent(AiTool tool) {
    switch (tool) {
        case AiTool::ClaudeCode: return "#6366f1";
        case AiTool::CodexCli:  return "#3b82f6";
        case AiTool::GeminiCli: return "#0d9488";
        default:                return "#6366f1";
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

// ── 日志颜色（深色终端背景）────────────────────────────────────
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
    resize(860, 660);

    connect(m_apiClient, &ApiClient::apiKeysReceived, this, &MainWindow::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::requestFailed, this, &MainWindow::onRequestFailed);
    connect(m_toolManager, &ToolManager::installOutput, this, &MainWindow::onInstallOutput);
    connect(m_toolManager, &ToolManager::installFinished, this, &MainWindow::onInstallFinished);

    rebuildCards();
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
}

QString MainWindow::maskKey(const QString &key)
{
    if (key.length() > 12) {
        return key.left(8) + "..." + key.right(4);
    }
    return key;
}

void MainWindow::logMessage(const QString &message, const QString &color)
{
    m_logOutput->append(QString("<span style='color:%1'>%2</span>").arg(color, message));
}

void MainWindow::setupUi()
{
    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    central->setStyleSheet("background-color: #f1f5f9;");

    QVBoxLayout *mainLayout = new QVBoxLayout(central);
    mainLayout->setSpacing(14);
    mainLayout->setContentsMargins(24, 20, 24, 20);

    // ===== 顶部导航栏 =====
    QFrame *topBar = new QFrame(this);
    topBar->setStyleSheet(
        "QFrame { background: white; border: 1.5px solid #e2e8f0; border-radius: 12px; }");
    QHBoxLayout *topBarLayout = new QHBoxLayout(topBar);
    topBarLayout->setContentsMargins(18, 12, 18, 12);
    topBarLayout->setSpacing(10);

    QLabel *logoBadge = new QLabel("A", this);
    logoBadge->setFixedSize(34, 34);
    logoBadge->setAlignment(Qt::AlignCenter);
    logoBadge->setStyleSheet(
        "QLabel {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
        "    stop:0 #6366f1, stop:1 #8b5cf6);"
        "  color: white; border-radius: 9px;"
        "  font-size: 17px; font-weight: bold; }");
    topBarLayout->addWidget(logoBadge);

    QLabel *appTitle = new QLabel("Aegisy 客户端", this);
    QFont appTitleFont = appTitle->font();
    appTitleFont.setPointSize(15);
    appTitleFont.setBold(true);
    appTitle->setFont(appTitleFont);
    appTitle->setStyleSheet("color: #1e293b;");
    topBarLayout->addWidget(appTitle);

    topBarLayout->addStretch();

    m_userLabel = new QLabel(this);
    m_userLabel->setStyleSheet("color: #64748b; font-size: 13px;");
    topBarLayout->addWidget(m_userLabel);

    m_logoutButton = new QPushButton("退出登录", this);
    m_logoutButton->setCursor(Qt::PointingHandCursor);
    m_logoutButton->setMinimumHeight(32);
    m_logoutButton->setStyleSheet(
        "QPushButton {"
        "  background: transparent; color: #ef4444;"
        "  border: 1.5px solid #fca5a5; border-radius: 7px;"
        "  padding: 4px 14px; font-size: 13px; }"
        "QPushButton:hover { background: #fef2f2; border-color: #ef4444; }");
    topBarLayout->addWidget(m_logoutButton);

    mainLayout->addWidget(topBar);

    // ===== 档案区标题栏 =====
    QHBoxLayout *sectionBar = new QHBoxLayout();
    QLabel *sectionTitle = new QLabel("我的配置档案", this);
    QFont secFont = sectionTitle->font();
    secFont.setPointSize(13);
    secFont.setBold(true);
    sectionTitle->setFont(secFont);
    sectionTitle->setStyleSheet("color: #334155;");
    sectionBar->addWidget(sectionTitle);
    sectionBar->addStretch();

    m_newConnectButton = new QPushButton("＋ 新建接入", this);
    m_newConnectButton->setCursor(Qt::PointingHandCursor);
    m_newConnectButton->setMinimumHeight(36);
    m_newConnectButton->setStyleSheet(
        "QPushButton {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 #6366f1, stop:1 #8b5cf6);"
        "  color: white; border: none; border-radius: 8px;"
        "  font-size: 13px; font-weight: bold; padding: 6px 18px; }"
        "QPushButton:hover {"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        "    stop:0 #4f46e5, stop:1 #7c3aed); }");
    sectionBar->addWidget(m_newConnectButton);
    mainLayout->addLayout(sectionBar);

    // ===== 类型筛选栏 =====
    QHBoxLayout *filterBar = new QHBoxLayout();
    filterBar->setSpacing(6);
    filterBar->setContentsMargins(0, 0, 0, 0);

    m_filterGroup = new QButtonGroup(this);
    m_filterGroup->setExclusive(true);

    const struct { int id; QString label; } kFilters[] = {
        { 0, QString::fromUtf8("全部") },
        { 1, QStringLiteral("Claude") },
        { 2, QStringLiteral("Codex") },
        { 3, QStringLiteral("Gemini") },
    };
    const QString filterBtnStyle =
        "QPushButton {"
        "  background: #f8fafc; color: #64748b; border: 1.5px solid #e2e8f0;"
        "  border-radius: 6px; font-size: 12px; padding: 3px 12px; }"
        "QPushButton:checked {"
        "  background: #eff6ff; color: #6366f1; border-color: #6366f1; font-weight: bold; }"
        "QPushButton:hover:!checked { background: #f1f5f9; }";
    for (const auto &f : kFilters) {
        auto *btn = new QPushButton(f.label, this);
        btn->setCheckable(true);
        btn->setChecked(f.id == 0);
        btn->setFixedHeight(28);
        btn->setStyleSheet(filterBtnStyle);
        m_filterGroup->addButton(btn, f.id);
        filterBar->addWidget(btn);
    }
    filterBar->addStretch();
    mainLayout->addLayout(filterBar);

    // ===== 档案卡片横向滚动区 =====
    m_cardsScroll = new QScrollArea(this);
    m_cardsScroll->setWidgetResizable(true);
    m_cardsScroll->setFrameShape(QFrame::NoFrame);
    m_cardsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_cardsScroll->setMinimumHeight(300);
    m_cardsScroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");

    m_cardsContainer = new QWidget();
    m_cardsContainer->setStyleSheet("background: transparent;");
    m_cardsLayout = new QHBoxLayout(m_cardsContainer);
    m_cardsLayout->setContentsMargins(2, 2, 2, 8);
    m_cardsLayout->setSpacing(14);
    m_cardsLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_cardsScroll->setWidget(m_cardsContainer);
    mainLayout->addWidget(m_cardsScroll, 1);

    // ===== 高级功能栏 =====
    QFrame *advFrame = new QFrame(this);
    advFrame->setStyleSheet(
        "QFrame { background: white; border: 1.5px solid #e2e8f0; border-radius: 8px; }");
    QHBoxLayout *advBar = new QHBoxLayout(advFrame);
    advBar->setContentsMargins(16, 8, 16, 8);
    advBar->setSpacing(10);

    QLabel *advLabel = new QLabel("高级功能", this);
    advLabel->setStyleSheet("color: #94a3b8; font-size: 12px;");
    advBar->addWidget(advLabel);

    QFrame *sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setFixedHeight(16);
    sep->setStyleSheet("color: #e2e8f0;");
    advBar->addWidget(sep);

    m_manageKeysButton = new QPushButton("🔑  我的 API Keys", this);
    m_viewModelsButton = new QPushButton("📋  查看模型", this);
    const QString advBtnStyle =
        "QPushButton {"
        "  background: transparent; color: #6366f1;"
        "  border: 1.5px solid #e0e7ff; border-radius: 6px;"
        "  padding: 4px 14px; font-size: 13px; }"
        "QPushButton:hover { background: #eef2ff; border-color: #6366f1; }";
    for (QPushButton *b : {m_manageKeysButton, m_viewModelsButton}) {
        b->setStyleSheet(advBtnStyle);
        b->setCursor(Qt::PointingHandCursor);
        advBar->addWidget(b);
    }
    advBar->addStretch();
    mainLayout->addWidget(advFrame);

    // ===== 日志（终端风格）=====
    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setMaximumHeight(100);
    m_logOutput->setStyleSheet(
        "QTextEdit {"
        "  background-color: #0f172a; border: 1px solid #1e293b;"
        "  border-radius: 8px; font-family: 'Courier New', monospace;"
        "  font-size: 11px; color: #94a3b8; padding: 6px; }");
    mainLayout->addWidget(m_logOutput);

    connect(m_logoutButton, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);
    connect(m_newConnectButton, &QPushButton::clicked, this, &MainWindow::onNewConnectClicked);
    connect(m_manageKeysButton, &QPushButton::clicked, this, &MainWindow::onManageKeysClicked);
    connect(m_viewModelsButton, &QPushButton::clicked, this, &MainWindow::onViewModelsClicked);
    connect(m_filterGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &MainWindow::onFilterChanged);
}

// ── 重建全部档案卡片 ────────────────────────────────────────────
void MainWindow::rebuildCards()
{
    QLayoutItem *item;
    while ((item = m_cardsLayout->takeAt(0)) != nullptr) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    const QList<Profile> profiles = m_profileManager->allProfiles();
    const int activeIdx = m_profileManager->activeIndex();

    for (const Profile &p : profiles) {
        // 筛选：Mixed 表示显示全部；否则只显示类型匹配的（Mixed 档案始终显示）
        if (m_filterType != ProfileType::Mixed
                && p.type != ProfileType::Mixed
                && p.type != m_filterType) {
            continue;
        }
        m_cardsLayout->addWidget(createProfileCard(p, p.index == activeIdx));
    }
    m_cardsLayout->addWidget(createAddCard());
    m_cardsLayout->addStretch();
}

// ── 单张档案卡片 ────────────────────────────────────────────────
QWidget* MainWindow::createProfileCard(const Profile &profile, bool isActive)
{
    QFrame *card = new QFrame(this);
    card->setFixedWidth(260);
    card->setStyleSheet(QString(
        "QFrame {"
        "  background: white;"
        "  border: %1;"
        "  border-radius: 12px;"
        "}").arg(isActive ? "2px solid #6366f1" : "1.5px solid #e2e8f0"));

    QVBoxLayout *v = new QVBoxLayout(card);
    v->setContentsMargins(16, 14, 16, 14);
    v->setSpacing(10);

    // 标题行：档案名 + 活跃徽章
    QHBoxLayout *titleRow = new QHBoxLayout();
    QLabel *nameLabel = new QLabel(profile.name, card);
    QFont nameFont = nameLabel->font();
    nameFont.setPointSize(13);
    nameFont.setBold(true);
    nameLabel->setFont(nameFont);
    nameLabel->setStyleSheet("color: #1e293b; border: none;");
    titleRow->addWidget(nameLabel);
    titleRow->addStretch();

    if (isActive) {
        QLabel *badge = new QLabel("● 活跃", card);
        badge->setStyleSheet(
            "color: #16a34a; background: #dcfce7; border: none;"
            "border-radius: 6px; padding: 2px 8px; font-size: 11px; font-weight: bold;");
        titleRow->addWidget(badge);
    }
    v->addLayout(titleRow);

    // 分隔线
    QFrame *hr = new QFrame(card);
    hr->setFrameShape(QFrame::HLine);
    hr->setStyleSheet("color: #f1f5f9; border: none; background: #f1f5f9; max-height: 1px;");
    v->addWidget(hr);

    // 三个工具行
    for (AiTool tool : {AiTool::ClaudeCode, AiTool::CodexCli, AiTool::GeminiCli}) {
        const QString key = profile.keyFor(tool);
        const QString model = profile.modelFor(tool);
        const QString accent = toolAccent(tool);

        QVBoxLayout *toolBox = new QVBoxLayout();
        toolBox->setSpacing(1);

        QLabel *head = new QLabel(
            QString("%1  %2").arg(toolEmoji(tool), ToolManager::toolName(tool)), card);
        head->setStyleSheet(QString("color: %1; border: none; font-size: 12px; font-weight: bold;").arg(accent));
        toolBox->addWidget(head);

        if (key.isEmpty()) {
            QLabel *none = new QLabel("　未配置", card);
            none->setStyleSheet("color: #cbd5e1; border: none; font-size: 11px;");
            toolBox->addWidget(none);
        } else {
            QLabel *keyLabel = new QLabel("　" + maskKey(key), card);
            keyLabel->setStyleSheet("color: #64748b; border: none; font-size: 11px; font-family: monospace;");
            toolBox->addWidget(keyLabel);

            QLabel *modelLabel = new QLabel("　" + (model.isEmpty() ? QString("(默认模型)") : model), card);
            modelLabel->setStyleSheet("color: #94a3b8; border: none; font-size: 11px;");
            toolBox->addWidget(modelLabel);
        }
        v->addLayout(toolBox);
    }

    v->addStretch();

    // 底部按钮行
    QHBoxLayout *btnRow = new QHBoxLayout();
    btnRow->setSpacing(6);

    QPushButton *activateBtn = new QPushButton(isActive ? "已激活" : "激活", card);
    activateBtn->setCursor(Qt::PointingHandCursor);
    activateBtn->setMinimumHeight(32);
    activateBtn->setEnabled(!isActive && profile.hasAnyKey());
    activateBtn->setStyleSheet(
        isActive
        ? QString("QPushButton { background: #dcfce7; color: #16a34a; border: none;"
                  "  border-radius: 7px; font-size: 12px; font-weight: bold; }")
        : QString("QPushButton { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,"
                  "    stop:0 #6366f1, stop:1 #8b5cf6);"
                  "  color: white; border: none; border-radius: 7px;"
                  "  font-size: 12px; font-weight: bold; }"
                  "QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,"
                  "    stop:0 #4f46e5, stop:1 #7c3aed); }"
                  "QPushButton:disabled { background: #e2e8f0; color: #94a3b8; }"));
    const int pidx = profile.index;
    connect(activateBtn, &QPushButton::clicked, this, [this, pidx]() { activateProfile(pidx); });
    btnRow->addWidget(activateBtn, 1);

    QPushButton *editBtn = new QPushButton("✏", card);
    editBtn->setCursor(Qt::PointingHandCursor);
    editBtn->setFixedSize(32, 32);
    editBtn->setToolTip("编辑档案");
    editBtn->setStyleSheet(
        "QPushButton { background: transparent; color: #64748b;"
        "  border: 1.5px solid #e2e8f0; border-radius: 7px; font-size: 13px; }"
        "QPushButton:hover { background: #f1f5f9; }");
    connect(editBtn, &QPushButton::clicked, this, [this, pidx]() { editProfile(pidx); });
    btnRow->addWidget(editBtn);

    QPushButton *delBtn = new QPushButton("🗑", card);
    delBtn->setCursor(Qt::PointingHandCursor);
    delBtn->setFixedSize(32, 32);
    delBtn->setToolTip("删除档案");
    delBtn->setStyleSheet(
        "QPushButton { background: transparent; color: #ef4444;"
        "  border: 1.5px solid #fecaca; border-radius: 7px; font-size: 13px; }"
        "QPushButton:hover { background: #fef2f2; }");
    connect(delBtn, &QPushButton::clicked, this, [this, pidx]() { deleteProfile(pidx); });
    btnRow->addWidget(delBtn);

    v->addLayout(btnRow);
    return card;
}

// ── “新建接入”占位卡片 ──────────────────────────────────────────
QWidget* MainWindow::createAddCard()
{
    QPushButton *addCard = new QPushButton("＋\n\n新建接入", this);
    addCard->setCursor(Qt::PointingHandCursor);
    addCard->setFixedWidth(180);
    addCard->setMinimumHeight(240);
    addCard->setStyleSheet(
        "QPushButton {"
        "  background: transparent; color: #94a3b8;"
        "  border: 2px dashed #cbd5e1; border-radius: 12px;"
        "  font-size: 14px; }"
        "QPushButton:hover { background: #eef2ff; color: #6366f1; border-color: #6366f1; }");
    connect(addCard, &QPushButton::clicked, this, &MainWindow::onNewConnectClicked);
    return addCard;
}

// ── 筛选类型切换 ─────────────────────────────────────────────────
void MainWindow::onFilterChanged(int typeId)
{
    m_filterType = static_cast<ProfileType>(typeId);
    rebuildCards();
}

// ── 新建接入：打开向导 ──────────────────────────────────────────
void MainWindow::onNewConnectClicked()
{
    ConnectWizardDialog dlg(m_apiClient, m_profileManager, -1, this);
    const int result = dlg.exec();
    m_apiClient->getApiKeys();

    if (result == QDialog::Accepted) {
        const int newIdx = dlg.resultIndex();
        logMessage("✓ 新档案创建完成，正在检测环境...", kLogSuccess);
        rebuildCards();
        showEnvCheckDialog(newIdx);   // 先检测再激活
        if (newIdx >= 0) {
            activateProfile(newIdx);
        }
    } else {
        rebuildCards();
    }
}

// ── 编辑档案 ────────────────────────────────────────────────────
void MainWindow::editProfile(int index)
{
    ConnectWizardDialog dlg(m_apiClient, m_profileManager, index, this);
    const int result = dlg.exec();
    m_apiClient->getApiKeys();

    if (result == QDialog::Accepted) {
        logMessage("✓ 档案已更新，正在检测环境...", kLogSuccess);
        showEnvCheckDialog(index);
    }
    rebuildCards();
}

// ── 删除档案 ────────────────────────────────────────────────────
void MainWindow::deleteProfile(int index)
{
    const QList<Profile> profiles = m_profileManager->allProfiles();
    if (index < 0 || index >= profiles.size()) return;
    if (profiles.size() <= 1) {
        QMessageBox::information(this, "无法删除", "至少需要保留一个档案。");
        return;
    }
    const auto reply = QMessageBox::question(
        this, "删除档案",
        QString("确定删除档案「%1」吗？此操作不可恢复。").arg(profiles[index].name),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) return;

    m_profileManager->removeProfile(index);
    logMessage("已删除档案", kLogMuted);
    rebuildCards();
}

// ── 激活档案（含自动安装缺失工具）──────────────────────────────
void MainWindow::activateProfile(int index)
{
    const QList<Profile> profiles = m_profileManager->allProfiles();
    if (index < 0 || index >= profiles.size()) return;
    const Profile profile = profiles[index];
    if (!profile.hasAnyKey()) {
        logMessage("该档案未配置任何 Key，无法激活", kLogWarn);
        return;
    }
    // 允许在上一次激活尚未跑完时直接切换：重置队列后重新开始。
    // 旧的异步安装若还在进行，其回调会被 onInstallFinished 里的“陈旧回调”判断忽略。
    if (m_activatingIndex >= 0 && m_activatingIndex != index) {
        logMessage("切换到新档案，中断上一个激活任务", kLogMuted);
    }

    m_profileManager->setActiveIndex(index);
    m_activatingIndex = index;
    m_activationQueue.clear();
    for (AiTool tool : {AiTool::ClaudeCode, AiTool::CodexCli, AiTool::GeminiCli}) {
        if (!profile.keyFor(tool).isEmpty()) {
            m_activationQueue.append(tool);
        }
    }
    logMessage(QString("正在激活档案「%1」...").arg(profile.name), kLogInfo);
    rebuildCards();
    processActivationQueue();
}

// ── 逐个处理激活队列（安装 → 配置）────────────────────────────
void MainWindow::processActivationQueue()
{
    if (m_activationQueue.isEmpty()) {
        logMessage("🎉 档案激活完成！重新打开终端即可使用。", kLogSuccess);
        m_activatingIndex = -1;
        rebuildCards();
        return;
    }

    const AiTool tool = m_activationQueue.first();
    const ToolStatus status = m_toolManager->detect(tool);

    // 如果检测到环境变量冲突，先警告（激活照常继续，configure() 会强制覆写配置文件）
    if (!status.conflictWarning.isEmpty()) {
        logMessage(QString("⚠ %1").arg(status.conflictWarning), kLogWarn);
    }

    if (!status.nodeOk) {
        logMessage(QString("✗ %1 跳过：未检测到 Node.js").arg(ToolManager::toolName(tool)), kLogError);
        m_activationQueue.removeFirst();
        processActivationQueue();
        return;
    }

    if (!status.installed) {
        logMessage(QString("%1 未安装，开始自动安装...").arg(ToolManager::toolName(tool)), kLogInfo);
        m_toolManager->install(tool);  // 异步 → onInstallFinished 继续
        return;
    }

    // 已安装 → 直接写配置，然后推进队列
    configureFromProfile(m_activatingIndex, tool);
    m_activationQueue.removeFirst();
    processActivationQueue();
}

// ── 用档案里的 Key + Model 写入某个工具的配置 ──────────────────
void MainWindow::configureFromProfile(int profileIndex, AiTool tool)
{
    const QList<Profile> profiles = m_profileManager->allProfiles();
    if (profileIndex < 0 || profileIndex >= profiles.size()) return;
    const Profile profile = profiles[profileIndex];
    const QString key = profile.keyFor(tool);
    const QString model = profile.modelFor(tool);
    if (key.isEmpty()) return;

    logMessage(QString("正在写入 %1 配置...").arg(ToolManager::toolName(tool)), kLogInfo);
    const bool ok = m_toolManager->configure(tool, key, model);
    if (ok) {
        logMessage(QString("✓ %1 已接入%2")
                       .arg(ToolManager::toolName(tool),
                            model.isEmpty() ? QString() : QString("（模型 %1）").arg(model)),
                   kLogSuccess);
    } else {
        logMessage(QString("✗ %1 配置写入失败：%2")
                       .arg(ToolManager::toolName(tool), m_toolManager->lastError()),
                   kLogError);
    }
}

// ── API / 安装回调 ──────────────────────────────────────────────
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

void MainWindow::onInstallOutput(AiTool tool, const QString &line)
{
    Q_UNUSED(tool);
    logMessage(line, kLogMuted);
}

void MainWindow::onInstallFinished(AiTool tool, bool success)
{
    if (m_activatingIndex < 0) return;  // 不在激活流程中

    // 陈旧回调：若完成的工具已不是当前队列的队首，说明用户已切换到别的档案，
    // 这个安装结果作废，直接忽略，避免破坏新队列。
    if (m_activationQueue.isEmpty() || m_activationQueue.first() != tool) {
        logMessage(QString("忽略过期的安装回调：%1").arg(ToolManager::toolName(tool)), kLogMuted);
        return;
    }

    if (!success) {
        logMessage(QString("✗ %1 安装失败，请检查网络或手动运行：npm install -g %2")
                       .arg(ToolManager::toolName(tool), ToolManager::npmPackage(tool)),
                   kLogError);
        m_activationQueue.removeFirst();
        processActivationQueue();
        return;
    }

    logMessage(QString("✓ %1 安装完成").arg(ToolManager::toolName(tool)), kLogSuccess);
    configureFromProfile(m_activatingIndex, tool);
    m_activationQueue.removeFirst();
    processActivationQueue();
}

// ── 保存后环境检测弹窗 ──────────────────────────────────────────
void MainWindow::showEnvCheckDialog(int profileIndex)
{
    if (profileIndex < 0) return;
    const QList<Profile> profiles = m_profileManager->allProfiles();
    if (profileIndex >= profiles.size()) return;
    const Profile &profile = profiles[profileIndex];

    // 只检测该档案实际配置了 Key 的工具
    QList<AiTool> tools;
    for (AiTool t : toolsForType(profile.type)) {
        if (!profile.keyFor(t).isEmpty()) tools.append(t);
    }
    if (tools.isEmpty()) return;

    // 构建检测结果（同步，最多 2s/工具）
    struct CheckRow {
        QString toolName;
        bool    nodeOk;
        bool    cliOk;
        bool    desktopOk;
        QString desktopName;
        QString desktopUrl;
        QString conflict;
    };
    QList<CheckRow> rows;
    bool anyIssue = false;

    for (AiTool t : tools) {
        const ToolStatus     ts = m_toolManager->detectFast(t);
        const DesktopAppStatus ds = m_toolManager->detectDesktop(t);
        CheckRow row;
        row.toolName    = ToolManager::toolName(t);
        row.nodeOk      = ts.nodeOk;
        row.cliOk       = ts.installed;
        row.desktopOk   = ds.installed;
        row.desktopName = ds.appName;
        row.desktopUrl  = ds.downloadUrl;
        row.conflict    = ts.conflictWarning;
        rows.append(row);
        if (!ts.nodeOk || !ts.installed || !ts.conflictWarning.isEmpty())
            anyIssue = true;
    }

    if (!anyIssue) {
        // 全部就绪时不打扰用户，只在日志里输出一条简报
        for (const CheckRow &r : rows) {
            logMessage(QString("✓ %1 环境就绪").arg(r.toolName), kLogSuccess);
        }
        return;
    }

    // ── 有问题时弹出检测报告 ──────────────────────────────────────
    QDialog dlg(this);
    dlg.setWindowTitle(QString::fromUtf8("环境检测报告"));
    dlg.setFixedWidth(480);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setStyleSheet("QDialog { background: #ffffff; }");

    auto *root = new QVBoxLayout(&dlg);
    root->setContentsMargins(24, 20, 24, 16);
    root->setSpacing(12);

    auto *title = new QLabel(QString::fromUtf8("🔍 保存档案后的环境检测"), &dlg);
    title->setStyleSheet("font-size: 15px; font-weight: bold; color: #111827;");
    root->addWidget(title);

    auto *sub = new QLabel(QString::fromUtf8("以下工具需要你完成额外的安装步骤后才能正常使用："), &dlg);
    sub->setWordWrap(true);
    sub->setStyleSheet("font-size: 12px; color: #6b7280;");
    root->addWidget(sub);

    for (const CheckRow &r : rows) {
        auto *card = new QFrame(&dlg);
        card->setStyleSheet(
            "QFrame { background: #f9fafb; border: 1px solid #e5e7eb; border-radius: 8px; }");
        auto *cv = new QVBoxLayout(card);
        cv->setContentsMargins(14, 12, 14, 12);
        cv->setSpacing(6);

        auto *hdr = new QLabel(r.toolName, card);
        hdr->setStyleSheet("font-size: 13px; font-weight: bold; color: #111827;");
        cv->addWidget(hdr);

        // Node.js
        if (!r.nodeOk) {
            auto *row2 = new QHBoxLayout;
            auto *icon = new QLabel("❌", card);
            auto *txt  = new QLabel(QString::fromUtf8("Node.js 未安装"), card);
            txt->setStyleSheet("font-size: 12px; color: #ef4444;");
            auto *btn  = new QPushButton(QString::fromUtf8("下载 Node.js"), card);
            btn->setFixedHeight(26);
            btn->setStyleSheet(
                "QPushButton { background: #3b82f6; color: white; border: none;"
                "  border-radius: 5px; font-size: 11px; padding: 0 10px; }"
                "QPushButton:hover { background: #2563eb; }");
            connect(btn, &QPushButton::clicked, [](){ QDesktopServices::openUrl(QUrl("https://nodejs.org/")); });
            row2->addWidget(icon); row2->addWidget(txt, 1); row2->addWidget(btn);
            cv->addLayout(row2);
        }

        // CLI
        if (!r.cliOk) {
            auto *row2 = new QHBoxLayout;
            auto *icon = new QLabel("❌", card);
            auto *txt  = new QLabel(QString::fromUtf8("CLI 未安装"), card);
            txt->setStyleSheet("font-size: 12px; color: #ef4444;");
            auto *btn  = new QPushButton(QString::fromUtf8("一键安装"), card);
            btn->setEnabled(r.nodeOk);
            btn->setFixedHeight(26);
            btn->setStyleSheet(
                "QPushButton { background: #6366f1; color: white; border: none;"
                "  border-radius: 5px; font-size: 11px; padding: 0 10px; }"
                "QPushButton:hover { background: #4f46e5; }"
                "QPushButton:disabled { background: #d1d5db; color: #9ca3af; }");
            // 找到对应工具
            for (AiTool t : tools) {
                if (ToolManager::toolName(t) == r.toolName) {
                    connect(btn, &QPushButton::clicked, this, [this, t, btn](){
                        btn->setEnabled(false);
                        btn->setText(QString::fromUtf8("安装中..."));
                        m_toolManager->install(t);
                    });
                    break;
                }
            }
            row2->addWidget(icon); row2->addWidget(txt, 1); row2->addWidget(btn);
            cv->addLayout(row2);
        }

        // Desktop
        if (!r.desktopOk) {
            auto *row2 = new QHBoxLayout;
            auto *icon = new QLabel("⬇️", card);
            auto *txt  = new QLabel(r.desktopName + QString::fromUtf8(" 未安装"), card);
            txt->setStyleSheet("font-size: 12px; color: #f59e0b;");
            auto *btn  = new QPushButton(QString::fromUtf8("下载安装"), card);
            btn->setFixedHeight(26);
            const QString url = r.desktopUrl;
            btn->setStyleSheet(
                "QPushButton { background: #f59e0b; color: white; border: none;"
                "  border-radius: 5px; font-size: 11px; padding: 0 10px; }"
                "QPushButton:hover { background: #d97706; }");
            connect(btn, &QPushButton::clicked, [url](){ QDesktopServices::openUrl(QUrl(url)); });
            row2->addWidget(icon); row2->addWidget(txt, 1); row2->addWidget(btn);
            cv->addLayout(row2);
        }

        // 冲突警告
        if (!r.conflict.isEmpty()) {
            auto *row2 = new QHBoxLayout;
            auto *icon = new QLabel("⚠️", card);
            auto *txt  = new QLabel(r.conflict, card);
            txt->setWordWrap(true);
            txt->setStyleSheet("font-size: 11px; color: #b45309;");
            row2->addWidget(icon); row2->addWidget(txt, 1);
            cv->addLayout(row2);
        }

        root->addWidget(card);
    }

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok, &dlg);
    btns->button(QDialogButtonBox::Ok)->setText(QString::fromUtf8("知道了"));
    connect(btns, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    root->addWidget(btns);

    dlg.exec();
}

// ── 高级功能 / 退出 ─────────────────────────────────────────────
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
    if (reply != QMessageBox::Yes) return;

    SecureStorage::clearToken();
    emit loggedOut();
    close();
}
