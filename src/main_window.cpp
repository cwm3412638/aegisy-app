#include "main_window.h"

#include "api_keys_dialog.h"
#include "connect_wizard.h"
#include "models_dialog.h"
#include "secure_storage.h"

#include <QButtonGroup>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QMessageBox>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

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

QString primaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: #0f766e; color: white; border: none; border-radius: 7px;"
        "  padding: 0 16px; font-size: 13px; font-weight: 600;"
        "}"
        "QPushButton:hover { background: #0b625c; }"
        "QPushButton:pressed { background: #094f4a; }"
        "QPushButton:disabled { background: #d7dde3; color: #8a96a3; }");
}

QString secondaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton {"
        "  background: white; color: #344054; border: 1px solid #d0d5dd;"
        "  border-radius: 7px; padding: 0 13px; font-size: 12px;"
        "}"
        "QPushButton:hover { background: #f8fafb; border-color: #98a2b3; }"
        "QPushButton:disabled { color: #98a2b3; background: #f8fafb; }");
}

const QString kLogSuccess = QStringLiteral("#067647");
const QString kLogError   = QStringLiteral("#b42318");
const QString kLogInfo    = QStringLiteral("#175cd3");
const QString kLogWarn    = QStringLiteral("#b54708");
const QString kLogMuted   = QStringLiteral("#667085");

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_apiClient(new ApiClient(this))
    , m_toolManager(new ToolManager(this))
    , m_profileManager(new ProfileManager(this))
{
    setupUi();
    setWindowTitle(QStringLiteral("Aegisy - AI 工具连接管理"));
    resize(1080, 720);
    setMinimumSize(900, 620);

    connect(m_apiClient, &ApiClient::apiKeysReceived,
            this, &MainWindow::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::requestFailed,
            this, &MainWindow::onRequestFailed);
    connect(m_toolManager, &ToolManager::installOutput,
            this, &MainWindow::onInstallOutput);
    connect(m_toolManager, &ToolManager::installFinished,
            this, &MainWindow::onInstallFinished);

    rebuildCards();
}

MainWindow::~MainWindow() = default;

void MainWindow::setAuthToken(const QString &token)
{
    m_authToken = token;
    m_apiClient->setAuthToken(token);
    logMessage(QStringLiteral("正在同步账号 API Keys..."), kLogInfo);
    m_apiClient->getApiKeys();
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
        "QWidget#appRoot { background: #f6f7f9; }"
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

    m_userLabel = new QLabel(QStringLiteral("● 账号已连接"), topBar);
    m_userLabel->setStyleSheet(QStringLiteral(
        "color: #067647; background: #ecfdf3; border: 1px solid #abefc6;"
        "border-radius: 7px; padding: 6px 10px; font-size: 11px; font-weight: 600;"));
    topLayout->addWidget(m_userLabel);

    m_manageKeysButton = new QPushButton(QStringLiteral("API Keys"), topBar);
    m_manageKeysButton->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    m_viewModelsButton = new QPushButton(QStringLiteral("模型"), topBar);
    m_viewModelsButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogListView));
    for (QPushButton *button : { m_manageKeysButton, m_viewModelsButton }) {
        button->setFixedHeight(36);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(secondaryButtonStyle());
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
    } filters[] = {
        { 0, "全部配置" },
        { static_cast<int>(ProfileType::Claude), "Claude Code" },
        { static_cast<int>(ProfileType::Codex),  "Codex CLI" },
        { static_cast<int>(ProfileType::Gemini), "Gemini CLI" },
    };

    const QString filterStyle = QStringLiteral(
        "QPushButton {"
        "  background: transparent; color: #475467; border: none; border-radius: 7px;"
        "  text-align: left; padding-left: 14px; font-size: 13px;"
        "}"
        "QPushButton:hover { background: #f0f2f5; color: #182230; }"
        "QPushButton:checked { background: #e7f5f2; color: #0f5f59; font-weight: 700; }");
    for (const auto &filter : filters) {
        auto *button = new QPushButton(QString::fromUtf8(filter.label), sidebar);
        button->setCheckable(true);
        button->setChecked(filter.id == 0);
        button->setFixedHeight(40);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(filterStyle);
        m_filterGroup->addButton(button, filter.id);
        sideLayout->addWidget(button);
    }
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
        QStringLiteral("独立管理 Claude、Codex 和 Gemini 的本地认证"), content);
    pageSubtitle->setStyleSheet(QStringLiteral("font-size: 12px; color: #667085;"));
    headingColumn->addWidget(pageTitle);
    headingColumn->addWidget(pageSubtitle);
    headingRow->addLayout(headingColumn);
    headingRow->addStretch();

    m_newConnectButton = new QPushButton(QStringLiteral("新建配置"), content);
    m_newConnectButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    m_newConnectButton->setFixedHeight(40);
    m_newConnectButton->setCursor(Qt::PointingHandCursor);
    m_newConnectButton->setStyleSheet(primaryButtonStyle());
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
    connect(m_filterGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &MainWindow::onFilterChanged);
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
    const int activeIndex = m_profileManager->activeIndex();
    m_profileCountLabel->setText(QStringLiteral("%1 个配置").arg(profiles.size()));

    if (activeIndex >= 0 && activeIndex < profiles.size()
            && profiles[activeIndex].hasAnyKey()) {
        const Profile &active = profiles[activeIndex];
        m_activeProfileLabel->setText(
            QStringLiteral("当前：%1 · %2")
                .arg(active.name, ToolManager::toolName(active.tool())));
    } else {
        m_activeProfileLabel->setText(QStringLiteral("尚未激活有效配置"));
    }

    int visibleCount = 0;
    for (const Profile &profile : profiles) {
        if (m_filterType != 0 && static_cast<int>(profile.type) != m_filterType) {
            continue;
        }
        const bool isActive = profile.index == activeIndex && profile.hasAnyKey();
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
    card->setMinimumHeight(112);
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
        QStringLiteral("%1  ·  %2")
            .arg(ToolManager::toolName(tool), toolConfigPath(tool)), card);
    toolLine->setStyleSheet(QStringLiteral(
        "font-size: 11px; color: #667085; border: none;"));
    details->addWidget(toolLine);

    auto *configRow = new QHBoxLayout;
    configRow->setSpacing(14);
    auto *keyLabel = new QLabel(
        profile.key.isEmpty() ? QStringLiteral("Key：未配置")
                              : QStringLiteral("Key：%1").arg(maskKey(profile.key)), card);
    keyLabel->setStyleSheet(QStringLiteral(
        "font-family: monospace; font-size: 11px; color: %1; border: none;")
        .arg(profile.key.isEmpty() ? QStringLiteral("#b54708") : QStringLiteral("#475467")));
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
        : primaryButtonStyle());
    const int profileIndex = profile.index;
    connect(activateButton, &QPushButton::clicked,
            this, [this, profileIndex]() { activateProfile(profileIndex); });
    layout->addWidget(activateButton);

    auto *editButton = new QPushButton(card);
    editButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    editButton->setToolTip(QStringLiteral("编辑配置"));
    editButton->setFixedSize(38, 38);
    editButton->setCursor(Qt::PointingHandCursor);
    editButton->setStyleSheet(secondaryButtonStyle());
    connect(editButton, &QPushButton::clicked,
            this, [this, profileIndex]() { editProfile(profileIndex); });
    layout->addWidget(editButton);

    auto *deleteButton = new QPushButton(card);
    deleteButton->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
    deleteButton->setToolTip(QStringLiteral("删除配置"));
    deleteButton->setFixedSize(38, 38);
    deleteButton->setCursor(Qt::PointingHandCursor);
    deleteButton->setEnabled(m_profileManager->count() > 1);
    deleteButton->setStyleSheet(QStringLiteral(
        "QPushButton { background: white; border: 1px solid #fecdca; border-radius: 7px; }"
        "QPushButton:hover { background: #fef3f2; border-color: #f04438; }"
        "QPushButton:disabled { background: #f8fafb; border-color: #eaecf0; }"));
    connect(deleteButton, &QPushButton::clicked,
            this, [this, profileIndex]() { deleteProfile(profileIndex); });
    layout->addWidget(deleteButton);

    return card;
}

QWidget *MainWindow::createAddCard()
{
    auto *button = new QPushButton(QStringLiteral("创建另一个连接配置"), m_cardsContainer);
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
    const bool wasActive = m_profileManager->activeIndex() == index;
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

    if (m_activatingIndex >= 0 && m_activatingIndex != index) {
        logMessage(QStringLiteral("已切换激活任务，之前的异步结果将被忽略"), kLogMuted);
    }

    m_profileManager->setActiveIndex(index);
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

void MainWindow::processActivationQueue()
{
    if (m_activationQueue.isEmpty()) {
        const QList<Profile> profiles = m_profileManager->allProfiles();
        if (m_activatingIndex >= 0 && m_activatingIndex < profiles.size()) {
            const Profile &profile = profiles[m_activatingIndex];
            logMessage(
                QStringLiteral("「%1」已激活，本地认证配置已更新").arg(profile.name),
                kLogSuccess);
        }
        m_activatingIndex = -1;
        rebuildCards();
        return;
    }

    const AiTool tool = m_activationQueue.first();
    const bool configured = configureFromProfile(m_activatingIndex, tool);
    const ToolStatus status = m_toolManager->detect(tool);

    if (!status.conflictWarning.isEmpty()) {
        logMessage(status.conflictWarning, kLogWarn);
    }

    if (!status.nodeOk) {
        logMessage(
            configured
                ? QStringLiteral("%1 认证已更新；安装 Node.js 后即可运行 CLI")
                      .arg(ToolManager::toolName(tool))
                : QStringLiteral("%1 配置写入失败，且未检测到 Node.js")
                      .arg(ToolManager::toolName(tool)),
            configured ? kLogWarn : kLogError);
        m_activationQueue.removeFirst();
        processActivationQueue();
        return;
    }

    if (!status.installed) {
        logMessage(
            configured
                ? QStringLiteral("认证已更新，正在安装 %1...").arg(ToolManager::toolName(tool))
                : QStringLiteral("配置写入失败，仍尝试安装 %1...").arg(ToolManager::toolName(tool)),
            configured ? kLogInfo : kLogWarn);
        m_toolManager->install(tool, m_activationGeneration);
        return;
    }

    m_activationQueue.removeFirst();
    processActivationQueue();
}

bool MainWindow::configureFromProfile(int profileIndex, AiTool tool)
{
    const QList<Profile> profiles = m_profileManager->allProfiles();
    if (profileIndex < 0 || profileIndex >= profiles.size()) {
        return false;
    }

    const Profile &profile = profiles[profileIndex];
    if (profile.tool() != tool || profile.key.isEmpty()) {
        return false;
    }

    const bool success = m_toolManager->configure(tool, profile.key, profile.model);
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

void MainWindow::onRequestFailed(const QString &error)
{
    logMessage(QStringLiteral("请求失败：%1").arg(error), kLogError);
}

void MainWindow::onInstallOutput(AiTool tool, const QString &line)
{
    Q_UNUSED(tool);
    logMessage(line, kLogMuted);
}

void MainWindow::onInstallFinished(AiTool tool, int requestId, bool success)
{
    if (m_activatingIndex < 0) {
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
    if (status.nodeOk && status.installed && status.conflictWarning.isEmpty()) {
        logMessage(
            QStringLiteral("%1 本地环境已就绪").arg(ToolManager::toolName(tool)),
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

    if (!status.nodeOk) {
        auto *button = new QPushButton(QStringLiteral("下载 Node.js"), &dialog);
        button->setFixedHeight(34);
        button->setStyleSheet(primaryButtonStyle());
        connect(button, &QPushButton::clicked, []() {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://nodejs.org/")));
        });
        addIssueRow(QStyle::SP_MessageBoxWarning,
                    QStringLiteral("Node.js 未安装"),
                    QStringLiteral("CLI 的安装和运行需要 Node.js。"),
                    button);
    } else if (!status.installed) {
        auto *button = new QPushButton(QStringLiteral("安装 CLI"), &dialog);
        button->setFixedHeight(34);
        button->setStyleSheet(primaryButtonStyle());
        connect(button, &QPushButton::clicked, this, [this, tool, button]() {
            button->setEnabled(false);
            button->setText(QStringLiteral("安装中..."));
            m_toolManager->install(tool, -1);
        });
        addIssueRow(QStyle::SP_MessageBoxWarning,
                    QStringLiteral("CLI 未安装"),
                    QStringLiteral("认证文件仍会正常更新，安装后即可直接使用。"),
                    button);
    }

    if (!desktop.installed) {
        auto *button = new QPushButton(QStringLiteral("打开下载页"), &dialog);
        button->setFixedHeight(34);
        button->setStyleSheet(secondaryButtonStyle());
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
    buttons->button(QDialogButtonBox::Ok)->setStyleSheet(primaryButtonStyle());
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
    emit loggedOut();
    close();
}
