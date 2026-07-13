#include "chat_dialog.h"

#include "api_client.h"
#include "app_theme.h"
#include "profile_manager.h"
#include "skill_manager.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QDesktopServices>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kMaximumStoredSessions = 100;

QString maskedKey(const QString &key)
{
    return key.size() <= 12 ? key : key.left(8) + QStringLiteral("...") + key.right(4);
}

QString jsonId(const QJsonValue &value)
{
    if (value.isString()) return value.toString();
    if (value.isDouble()) return QString::number(value.toVariant().toLongLong());
    return QString();
}

int modelContextWindow(const QJsonObject &model)
{
    for (const QString &field : { QStringLiteral("context_length"),
                                  QStringLiteral("context_window"),
                                  QStringLiteral("max_context_tokens"),
                                  QStringLiteral("max_input_tokens") }) {
        const int value = model.value(field).toVariant().toInt();
        if (value > 0) return value;
    }
    return 0;
}

// 消息操作按钮：带 Unicode 图标的轻量交互按钮
QPushButton *messageAction(const QString &icon, const QString &text, QWidget *parent)
{
    auto *button = new QPushButton(QStringLiteral("%1  %2").arg(icon, text), parent);
    button->setCursor(Qt::PointingHandCursor);
    button->setFixedHeight(26);
    button->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: transparent; color: #94a3b8; border: none;"
        "  padding: 2px 8px; font-size: 11px; border-radius: 5px;"
        "}"
        "QPushButton:hover {"
        "  color: #0f766e; background: #e7f5f2;"
        "}"));
    return button;
}

QLabel *messageAvatar(bool user, QWidget *parent)
{
    auto *avatar = new QLabel(parent);
    avatar->setFixedSize(34, 34);
    avatar->setAlignment(Qt::AlignCenter);
    if (user) {
        avatar->setText(QStringLiteral("我"));
        avatar->setStyleSheet(QStringLiteral(
            "QLabel { color: white; background: #344054; border: 1px solid #1d2939;"
            " border-radius: 17px; font-size: 12px; font-weight: 700; }"));
    } else {
        const QPixmap icon(QStringLiteral(":/icons/aegisy-icon.png"));
        avatar->setPixmap(icon.scaled(34, 34, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        avatar->setToolTip(QStringLiteral("Aegisy AI"));
        avatar->setStyleSheet(QStringLiteral(
            "QLabel { background: white; border: 1px solid #b7e4da; border-radius: 8px; }"));
    }
    return avatar;
}

} // namespace

ChatDialog::ChatDialog(ApiClient *apiClient,
                       SkillManager *skillManager,
                       ProfileManager *profileManager,
                       QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
    , m_skillManager(skillManager)
    , m_profileManager(profileManager)
{
    setupUi();
    setWindowTitle(QStringLiteral("AI 对话"));
    setMinimumSize(940, 640);
    resize(1220, 780);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    connect(m_apiClient, &ApiClient::apiKeysReceived,
            this, &ChatDialog::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::modelsReceived,
            this, &ChatDialog::onModelsReceived);
    connect(m_apiClient, &ApiClient::chatChunkReceived,
            this, &ChatDialog::onChatChunk);
    connect(m_apiClient, &ApiClient::chatUsageReceived,
            this, &ChatDialog::onChatUsage);
    connect(m_apiClient, &ApiClient::chatCompleted,
            this, &ChatDialog::onChatCompleted);
    connect(m_apiClient, &ApiClient::chatFailed,
            this, &ChatDialog::onChatFailed);
    connect(m_apiClient, &ApiClient::requestFailed,
            this, &ChatDialog::onRequestFailed);
    connect(m_apiClient, &ApiClient::imageGenerated,
            this, &ChatDialog::onSkillImageGenerated);
    connect(m_apiClient, &ApiClient::imageGenerationFailed,
            this, &ChatDialog::onSkillImageFailed);
    connect(m_apiClient, &ApiClient::presentationPlanReceived,
            this, &ChatDialog::onPresentationPlanReceived);
    connect(m_apiClient, &ApiClient::presentationPlanFailed,
            this, &ChatDialog::onPresentationPlanFailed);
    if (m_skillManager) {
        connect(m_skillManager, &SkillManager::skillsChanged, this, [this]() {
            const QList<SkillInfo> current = m_skillManager->skills();
            const int enabled = std::count_if(
                current.cbegin(), current.cend(),
                [](const SkillInfo &skill) { return skill.enabled; });
            m_skillsLabel->setText(QStringLiteral("Skills 自动 · %1").arg(enabled));
            if (!m_forcedSkillId.isEmpty()
                    && !m_skillManager->skill(m_forcedSkillId).enabled) {
                clearQuickSkill();
            }
            setGenerating(m_generating);
        });
    }
    if (m_profileManager) {
        // 用户切换激活档案后，若对话仍开着则强制把 Key 指向新档案。
        connect(m_profileManager, &ProfileManager::activeProfileChanged,
                this, [this](int, int) { selectActiveProfileKey(true); });
    }

    loadHistory();
    if (m_sessions.isEmpty()) {
        onNewSession();
    } else {
        rebuildSessionList();
        m_sessionList->setCurrentRow(0);
    }
    m_statusLabel->setText(QStringLiteral("正在读取可用 API Key..."));
    m_apiClient->getApiKeys();
}

void ChatDialog::setupUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *sidebar = new QFrame(this);
    sidebar->setObjectName(QStringLiteral("chatSidebar"));
    sidebar->setFixedWidth(238);
    sidebar->setStyleSheet(QStringLiteral(
        "QFrame#chatSidebar { background: #f8fafc; border-right: 1px solid #e4e7ec; }"));
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(14, 16, 14, 14);
    sideLayout->setSpacing(10);

    auto *brand = new QLabel(QStringLiteral("AI 对话"), sidebar);
    brand->setStyleSheet(QStringLiteral(
        "font-size: 18px; font-weight: 700; color: #101828; background: transparent;"));
    sideLayout->addWidget(brand);

    m_newButton = new QPushButton(QStringLiteral("新对话"), sidebar);
    m_newButton->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    m_newButton->setMinimumHeight(36);
    m_newButton->setCursor(Qt::PointingHandCursor);
    m_newButton->setStyleSheet(AppTheme::primaryButtonStyle());
    sideLayout->addWidget(m_newButton);

    auto *historyTitle = new QLabel(QStringLiteral("历史对话"), sidebar);
    historyTitle->setStyleSheet(QStringLiteral(
        "font-size: 11px; font-weight: 700; color: #667085; background: transparent; padding: 4px 6px 0;"));
    sideLayout->addWidget(historyTitle);

    m_sessionList = new QListWidget(sidebar);
    m_sessionList->setSpacing(2);
    m_sessionList->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; outline: none; }"
        "QListWidget::item { color: #475467; padding: 9px 8px; border-radius: 6px; }"
        "QListWidget::item:selected { background: #e7f5f2; color: #0f5f59; }"
        "QListWidget::item:hover { background: #eef2f6; }"));
    sideLayout->addWidget(m_sessionList, 1);

    m_deleteButton = new QPushButton(QStringLiteral("删除当前对话"), sidebar);
    m_deleteButton->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    m_deleteButton->setMinimumHeight(34);
    m_deleteButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    sideLayout->addWidget(m_deleteButton);

    m_sessionStatsLabel = new QLabel(sidebar);
    m_sessionStatsLabel->setAlignment(Qt::AlignCenter);
    m_sessionStatsLabel->setStyleSheet(QStringLiteral(
        "font-size: 10px; color: #98a2b3; padding: 4px 6px;"
        "background: #f2f4f7; border-radius: 6px;"));
    m_sessionStatsLabel->setText(QStringLiteral("暂无用量数据"));
    sideLayout->addWidget(m_sessionStatsLabel);

    root->addWidget(sidebar);

    auto *main = new QWidget(this);
    main->setObjectName(QStringLiteral("chatMain"));
    main->setStyleSheet(QStringLiteral("QWidget#chatMain { background: #f7f8fa; }"));
    auto *mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(22, 14, 22, 16);
    mainLayout->setSpacing(9);

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);
    auto *keyLabel = new QLabel(QStringLiteral("API Key"), main);
    keyLabel->setStyleSheet(QStringLiteral(
        "font-size: 12px; font-weight: 600; color: #344054; background: transparent;"));
    toolbar->addWidget(keyLabel);
    m_keyCombo = new QComboBox(main);
    m_keyCombo->setMinimumWidth(230);
    toolbar->addWidget(m_keyCombo);
    auto *modelLabel = new QLabel(QStringLiteral("模型"), main);
    modelLabel->setStyleSheet(keyLabel->styleSheet());
    toolbar->addWidget(modelLabel);
    m_modelCombo = new QComboBox(main);
    m_modelCombo->setMinimumWidth(210);
    toolbar->addWidget(m_modelCombo);
    toolbar->addStretch();
    m_copyConversationButton = new QPushButton(QStringLiteral("复制对话"), main);
    m_copyConversationButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    m_copyConversationButton->setToolTip(QStringLiteral("按 Markdown 格式复制整个对话"));
    m_copyConversationButton->setFixedHeight(32);
    m_copyConversationButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    toolbar->addWidget(m_copyConversationButton);
    m_stopButton = new QPushButton(QStringLiteral("停止"), main);
    m_stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    m_stopButton->setEnabled(false);
    m_stopButton->setFixedHeight(32);
    m_stopButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    toolbar->addWidget(m_stopButton);
    mainLayout->addLayout(toolbar);

    auto *metaRow = new QHBoxLayout();
    m_statusLabel = new QLabel(main);
    m_statusLabel->setStyleSheet(QStringLiteral(
        "font-size: 11px; color: #667085; background: transparent;"));
    metaRow->addWidget(m_statusLabel, 1);
    m_contextLabel = new QLabel(QStringLiteral("上下文 0 条"), main);
    m_contextLabel->setStyleSheet(QStringLiteral(
        "font-size: 11px; color: #475467; background: #eef2f6;"
        " border: 1px solid #dfe6ee; border-radius: 6px; padding: 4px 8px;"));
    m_skillsLabel = new QLabel(main);
    m_skillsLabel->setStyleSheet(QStringLiteral(
        "font-size: 11px; color: #0f5f59; background: #e7f5f2;"
        " border: 1px solid #b7e4da; border-radius: 6px; padding: 4px 8px;"));
    const QList<SkillInfo> currentSkills = m_skillManager
        ? m_skillManager->skills() : QList<SkillInfo>();
    const int enabledSkills = std::count_if(
        currentSkills.cbegin(), currentSkills.cend(),
        [](const SkillInfo &skill) { return skill.enabled; });
    m_skillsLabel->setText(QStringLiteral("Skills 自动 · %1").arg(enabledSkills));
    m_skillsLabel->setToolTip(QStringLiteral(
        "明确匹配已启用 Skill 时自动调用；可使用 /image 或 /ppt 强制调用。"));
    metaRow->addWidget(m_skillsLabel);
    metaRow->addWidget(m_contextLabel);
    mainLayout->addLayout(metaRow);

    m_scrollArea = new QScrollArea(main);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea { background: #f7f8fa; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 8px; }"
        "QScrollBar::handle:vertical { background: #cfd6df; border-radius: 4px; min-height: 32px; }"));
    m_messagesContainer = new QWidget(m_scrollArea);
    m_messagesContainer->setObjectName(QStringLiteral("messagesContainer"));
    m_messagesContainer->setStyleSheet(QStringLiteral(
        "QWidget#messagesContainer { background: #f7f8fa; }"));
    m_messagesLayout = new QVBoxLayout(m_messagesContainer);
    m_messagesLayout->setContentsMargins(20, 18, 20, 18);
    m_messagesLayout->setSpacing(18);
    m_messagesLayout->addStretch();
    m_scrollArea->setWidget(m_messagesContainer);
    mainLayout->addWidget(m_scrollArea, 1);

    m_editingLabel = new QLabel(main);
    m_editingLabel->setStyleSheet(QStringLiteral(
        "font-size: 11px; color: #0f766e; background: transparent;"));
    m_editingLabel->hide();
    mainLayout->addWidget(m_editingLabel);

    auto *quickSkills = new QHBoxLayout;
    quickSkills->setSpacing(8);
    const QString quickSkillStyle = QStringLiteral(
        "QPushButton { min-height: 30px; background: white; color: #475467;"
        " border: 1px solid #d0d5dd; border-radius: 6px; padding: 0 12px; font-size: 12px; }"
        "QPushButton:hover { border-color: #75bdb2; color: #0f766e; background: #f5fbfa; }"
        "QPushButton:checked { border-color: #0f766e; color: white; background: #0f766e; }"
        "QPushButton:disabled { color: #98a2b3; background: #f2f4f7; border-color: #e4e7ec; }");
    m_imageQuickButton = new QPushButton(QStringLiteral("生图"), main);
    m_imageQuickButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));
    m_imageQuickButton->setCheckable(true);
    m_imageQuickButton->setCursor(Qt::PointingHandCursor);
    m_imageQuickButton->setStyleSheet(quickSkillStyle);
    m_imageQuickButton->setToolTip(QStringLiteral("使用 Aegisy GPT Image Skill"));
    quickSkills->addWidget(m_imageQuickButton);
    m_presentationQuickButton = new QPushButton(QStringLiteral("PPT"), main);
    m_presentationQuickButton->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    m_presentationQuickButton->setCheckable(true);
    m_presentationQuickButton->setCursor(Qt::PointingHandCursor);
    m_presentationQuickButton->setStyleSheet(quickSkillStyle);
    m_presentationQuickButton->setToolTip(QStringLiteral("使用 PPT 制作 Skill"));
    quickSkills->addWidget(m_presentationQuickButton);
    quickSkills->addStretch();
    mainLayout->addLayout(quickSkills);

    auto *composer = new QFrame(main);
    composer->setObjectName(QStringLiteral("chatComposer"));
    composer->setStyleSheet(QStringLiteral(
        "QFrame#chatComposer { background: white; border: 1px solid #cfd6df; border-radius: 8px; }"));
    auto *composerLayout = new QHBoxLayout(composer);
    composerLayout->setContentsMargins(12, 8, 8, 8);
    composerLayout->setSpacing(10);
    m_inputEdit = new QPlainTextEdit(composer);
    m_inputEdit->setPlaceholderText(QStringLiteral("输入消息，Enter 发送，Shift+Enter 换行"));
    m_inputEdit->setMinimumHeight(64);
    m_inputEdit->setMaximumHeight(128);
    m_inputEdit->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { border: none; background: white; color: #101828; font-size: 13px; }"));
    m_inputEdit->installEventFilter(this);
    composerLayout->addWidget(m_inputEdit, 1);
    m_sendButton = new QPushButton(QStringLiteral("发送"), composer);
    m_sendButton->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
    m_sendButton->setFixedSize(88, 40);
    m_sendButton->setStyleSheet(AppTheme::primaryButtonStyle());
    composerLayout->addWidget(m_sendButton, 0, Qt::AlignBottom);
    mainLayout->addWidget(composer);
    root->addWidget(main, 1);

    connect(m_newButton, &QPushButton::clicked, this, &ChatDialog::onNewSession);
    connect(m_deleteButton, &QPushButton::clicked, this, &ChatDialog::onDeleteSession);
    connect(m_copyConversationButton, &QPushButton::clicked,
            this, &ChatDialog::onCopyConversation);
    connect(m_sessionList, &QListWidget::currentRowChanged,
            this, &ChatDialog::onSessionChanged);
    connect(m_keyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ChatDialog::onKeyChanged);
    connect(m_modelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ChatDialog::onModelChanged);
    connect(m_sendButton, &QPushButton::clicked, this, &ChatDialog::onSendClicked);
    connect(m_stopButton, &QPushButton::clicked, this, &ChatDialog::onStopClicked);
    connect(m_imageQuickButton, &QPushButton::clicked, this, [this]() {
        selectQuickSkill(m_forcedSkillId == QStringLiteral("aegisy.image.generate")
            ? QString() : QStringLiteral("aegisy.image.generate"));
    });
    connect(m_presentationQuickButton, &QPushButton::clicked, this, [this]() {
        selectQuickSkill(m_forcedSkillId == QStringLiteral("aegisy.presentation.create")
            ? QString() : QStringLiteral("aegisy.presentation.create"));
    });
}

bool ChatDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_inputEdit && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
                && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            onSendClicked();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ChatDialog::onApiKeysReceived(const QJsonArray &keys)
{
    m_allApiKeys = keys;
    const QSignalBlocker blocker(m_keyCombo);
    m_keyCombo->clear();
    for (const QJsonValue &value : keys) {
        const QJsonObject keyObject = value.toObject();
        const QString key = keyObject.value(QStringLiteral("key")).toString();
        const QString status = keyObject.value(QStringLiteral("status")).toString();
        if (key.isEmpty() || (!status.isEmpty() && status != QStringLiteral("active"))) continue;
        const QString name = keyObject.value(QStringLiteral("name")).toString().trimmed();
        const QString group = keyObject.value(QStringLiteral("group")).toObject()
            .value(QStringLiteral("name")).toString().trimmed();
        QString label = name.isEmpty() ? maskedKey(key) : name;
        if (!group.isEmpty()) label += QStringLiteral(" · %1").arg(group);
        m_keyCombo->addItem(label, key);
        const int row = m_keyCombo->count() - 1;
        m_keyCombo->setItemData(row, jsonId(keyObject.value(QStringLiteral("id"))), Qt::UserRole + 1);
        m_keyCombo->setItemData(row, label, Qt::UserRole + 2);
    }
    if (m_keyCombo->count() == 0) {
        m_keyCombo->addItem(QStringLiteral("没有可用 API Key"), QString());
        m_statusLabel->setText(QStringLiteral("请先在 Key 管理中创建并启用 API Key。"));
        return;
    }
    applyCurrentSessionSelection();
    // 新会话（尚无历史 Key）默认跟随最近激活的档案。
    selectActiveProfileKey(false);
    onKeyChanged(m_keyCombo->currentIndex());
}

void ChatDialog::onModelsReceived(const QJsonArray &models)
{
    const QSignalBlocker blocker(m_modelCombo);
    m_modelCombo->clear();
    for (const QJsonValue &value : models) {
        const QJsonObject model = value.toObject();
        const QString id = model.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty()) continue;
        m_modelCombo->addItem(id, id);
        m_modelCombo->setItemData(m_modelCombo->count() - 1,
                                  modelContextWindow(model), Qt::UserRole + 1);
    }
    if (m_modelCombo->count() == 0) {
        m_modelCombo->addItem(QStringLiteral("没有可用模型"), QString());
        m_statusLabel->setText(QStringLiteral("当前 Key 没有返回可用模型。"));
    } else {
        const QString wanted = !m_pendingModel.isEmpty() ? m_pendingModel
            : (m_currentSession >= 0 ? m_sessions[m_currentSession].model : QString());
        const int wantedIndex = m_modelCombo->findData(wanted);
        if (wantedIndex >= 0) m_modelCombo->setCurrentIndex(wantedIndex);
        m_statusLabel->setText(QStringLiteral("已加载 %1 个模型。").arg(m_modelCombo->count()));
    }
    m_pendingModel.clear();
    setGenerating(false);
    onModelChanged(m_modelCombo->currentIndex());
}

void ChatDialog::onKeyChanged(int)
{
    m_modelCombo->clear();
    const QString key = selectedApiKey();
    if (key.isEmpty()) {
        m_modelCombo->addItem(QStringLiteral("请先选择 Key"), QString());
        return;
    }
    if (!m_applyingSessionSelection && m_currentSession >= 0) {
        ChatSession &session = m_sessions[m_currentSession];
        session.keyId = selectedKeyId();
        session.keyName = selectedKeyName();
        session.updatedAt = QDateTime::currentDateTime();
        saveHistory();
    }
    m_keyCombo->setEnabled(false);
    m_modelCombo->setEnabled(false);
    m_sendButton->setEnabled(false);
    m_modelCombo->addItem(QStringLiteral("正在读取模型..."), QString());
    m_statusLabel->setText(QStringLiteral("正在读取当前 Key 的模型列表..."));
    m_apiClient->getModels(key);
}

void ChatDialog::onModelChanged(int)
{
    if (!m_applyingSessionSelection && m_currentSession >= 0) {
        const QString model = m_modelCombo->currentData().toString();
        if (!model.isEmpty()) {
            m_sessions[m_currentSession].model = model;
            m_sessions[m_currentSession].updatedAt = QDateTime::currentDateTime();
            saveHistory();
        }
    }
    updateContextInfo();
}

void ChatDialog::selectQuickSkill(const QString &skillId)
{
    if (!skillId.isEmpty() && m_skillManager) {
        const SkillInfo selected = m_skillManager->skill(skillId);
        if (selected.id.isEmpty() || !selected.enabled || !selected.compatible) {
            m_statusLabel->setText(QStringLiteral("该 Skill 当前不可用，请先在 Skills 管理中启用。"));
            return;
        }
    }
    m_forcedSkillId = skillId;
    m_imageQuickButton->setChecked(
        skillId == QStringLiteral("aegisy.image.generate"));
    m_presentationQuickButton->setChecked(
        skillId == QStringLiteral("aegisy.presentation.create"));
    if (skillId == QStringLiteral("aegisy.image.generate")) {
        m_inputEdit->setPlaceholderText(QStringLiteral("描述要生成的图片"));
        m_statusLabel->setText(QStringLiteral("已选择 GPT Image Skill。"));
    } else if (skillId == QStringLiteral("aegisy.presentation.create")) {
        m_inputEdit->setPlaceholderText(QStringLiteral("描述 PPT 的主题、受众、页数和风格"));
        m_statusLabel->setText(QStringLiteral("已选择 PPT 制作 Skill。"));
    } else {
        m_inputEdit->setPlaceholderText(QStringLiteral("输入消息，Enter 发送，Shift+Enter 换行"));
    }
    m_inputEdit->setFocus();
}

void ChatDialog::clearQuickSkill()
{
    m_forcedSkillId.clear();
    if (m_imageQuickButton) m_imageQuickButton->setChecked(false);
    if (m_presentationQuickButton) m_presentationQuickButton->setChecked(false);
    if (m_inputEdit) {
        m_inputEdit->setPlaceholderText(QStringLiteral("输入消息，Enter 发送，Shift+Enter 换行"));
    }
}

QString ChatDialog::selectedApiKey() const
{
    return m_keyCombo->currentData().toString();
}

QString ChatDialog::selectedKeyId() const
{
    return m_keyCombo->currentData(Qt::UserRole + 1).toString();
}

QString ChatDialog::selectedKeyName() const
{
    return m_keyCombo->currentData(Qt::UserRole + 2).toString();
}

void ChatDialog::onSendClicked()
{
    if (m_generating) return;
    const QString text = m_inputEdit->toPlainText().trimmed();
    if (text.isEmpty()) return;
    if (selectedApiKey().isEmpty() || m_modelCombo->currentData().toString().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法发送"),
                             QStringLiteral("请选择可用的 API Key 和模型。"));
        return;
    }
    if (m_currentSession < 0 || m_currentSession >= m_sessions.size()) onNewSession();

    ChatSession &session = m_sessions[m_currentSession];
    if (m_editingMessageIndex >= 0 && m_editingMessageIndex < session.messages.size()) {
        QJsonObject edited = session.messages.at(m_editingMessageIndex).toObject();
        edited.insert(QStringLiteral("content"), text);
        session.messages.replace(m_editingMessageIndex, edited);
        truncateMessagesAfter(m_editingMessageIndex);
    } else {
        session.messages.append(QJsonObject{
            { QStringLiteral("role"), QStringLiteral("user") },
            { QStringLiteral("content"), text }
        });
        updateSessionTitle(text);
    }
    session.keyId = selectedKeyId();
    session.keyName = selectedKeyName();
    session.model = m_modelCombo->currentData().toString();
    session.updatedAt = QDateTime::currentDateTime();
    session.promptTokens = 0;
    session.completionTokens = 0;
    session.totalTokens = 0;
    m_editingMessageIndex = -1;
    m_editingLabel->hide();
    m_sendButton->setText(QStringLiteral("发送"));
    m_inputEdit->clear();
    rebuildMessages();
    rebuildSessionList();
    m_sessionList->setCurrentRow(m_currentSession);
    saveHistory();
    if (!startMatchedSkill(text)) startRequest();
}

void ChatDialog::startRequest()
{
    if (m_currentSession < 0 || m_currentSession >= m_sessions.size()) return;
    const QString key = selectedApiKey();
    const QString model = m_modelCombo->currentData().toString();
    if (key.isEmpty() || model.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法发送"),
                             QStringLiteral("请选择可用的 API Key 和模型。"));
        return;
    }
    ChatSession &session = m_sessions[m_currentSession];
    session.keyId = selectedKeyId();
    session.keyName = selectedKeyName();
    session.model = model;
    session.updatedAt = QDateTime::currentDateTime();
    session.promptTokens = 0;
    session.completionTokens = 0;
    session.totalTokens = 0;

    m_streamContent.clear();
    addMessageWidget(QStringLiteral("assistant"), QStringLiteral("正在思考..."),
                     -1, &m_streamBrowser);
    m_requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    setGenerating(true);
    updateContextInfo();
    QJsonArray requestMessages = session.messages;
    QString activeSkillName;
    if (!m_instructionSkillId.isEmpty() && m_skillManager) {
        const QString instructions = m_skillManager->skillInstructions(m_instructionSkillId);
        const SkillInfo activeSkill = m_skillManager->skill(m_instructionSkillId);
        if (!instructions.isEmpty()) {
            activeSkillName = activeSkill.name;
            requestMessages.insert(0, QJsonObject{
                { QStringLiteral("role"), QStringLiteral("system") },
                { QStringLiteral("content"), QStringLiteral(
                    "你正在使用 Aegisy Skill「%1」。遵循下面的本地工作流，但不要声称已经执行"
                    "尚未实际运行的命令或生成不存在的文件。\n\n%2")
                    .arg(activeSkill.name, instructions) }
            });
        } else {
            m_instructionSkillId.clear();
        }
    }
    m_statusLabel->setText(activeSkillName.isEmpty()
        ? QStringLiteral("正在生成回复...")
        : QStringLiteral("正在使用 Skill：%1").arg(activeSkillName));
    m_apiClient->sendChatMessage(m_requestId, key, model, requestMessages);
}

bool ChatDialog::startMatchedSkill(const QString &requestText)
{
    if (!m_skillManager) return false;
    const SkillInfo matched = m_forcedSkillId.isEmpty()
        ? m_skillManager->matchSkill(requestText)
        : m_skillManager->skill(m_forcedSkillId);
    clearQuickSkill();
    if (matched.id.isEmpty()) return false;

    if (matched.executor == QStringLiteral("instruction")) {
        m_instructionSkillId = matched.id;
        startRequest();
        return true;
    }

    m_pendingSkillId = matched.id;
    m_pendingSkillRequest = requestText;
    m_streamContent.clear();
    addMessageWidget(QStringLiteral("assistant"),
                     QStringLiteral("正在调用 Skill：%1...").arg(matched.name),
                     -1, &m_streamBrowser);
    setGenerating(true);
    m_statusLabel->setText(QStringLiteral("正在调用 Skill：%1").arg(matched.name));

    if (matched.executor == QStringLiteral("image")) {
        const QString key = imageSkillApiKey();
        if (key.isEmpty()) {
            finishSkillRun(QStringLiteral(
                "无法调用 GPT Image Skill：账号中没有启用的 `gpt-image` 分组 API Key。"));
            return true;
        }
        QString prompt = requestText;
        if (prompt.startsWith(QStringLiteral("/image"), Qt::CaseInsensitive)) {
            prompt = prompt.mid(QStringLiteral("/image").size()).trimmed();
        }
        m_apiClient->generateImage(key, QStringLiteral("gpt-image-2"), prompt,
                                   QStringLiteral("1024x1024"), QStringLiteral("auto"),
                                   QStringLiteral("png"));
        return true;
    }

    if (matched.executor == QStringLiteral("presentation")) {
        if (!m_skillManager->presentationRuntimeReady()) {
            finishSkillRun(QStringLiteral(
                "已匹配 PPT Skill，但本机尚未安装 PPT 运行环境。请打开 **Skills 管理**，点击“安装 PPT 运行环境”后重试。"));
            return true;
        }
        QString prompt = requestText;
        if (prompt.startsWith(QStringLiteral("/ppt"), Qt::CaseInsensitive)) {
            prompt = prompt.mid(QStringLiteral("/ppt").size()).trimmed();
        }
        m_skillRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_apiClient->requestPresentationPlan(
            m_skillRequestId, selectedApiKey(), m_modelCombo->currentData().toString(), prompt);
        return true;
    }

    m_pendingSkillId.clear();
    m_pendingSkillRequest.clear();
    setGenerating(false);
    return false;
}

QString ChatDialog::imageSkillApiKey() const
{
    for (const QJsonValue &value : m_allApiKeys) {
        const QJsonObject keyObject = value.toObject();
        const QString status = keyObject.value(QStringLiteral("status")).toString();
        if (!status.isEmpty() && status != QStringLiteral("active")) continue;
        const QJsonObject group = keyObject.value(QStringLiteral("group")).toObject();
        if (group.value(QStringLiteral("name")).toString()
                    .compare(QStringLiteral("gpt-image"), Qt::CaseInsensitive) == 0
                || group.value(QStringLiteral("allow_image_generation")).toBool()) {
            const QString key = keyObject.value(QStringLiteral("key")).toString();
            if (!key.isEmpty()) return key;
        }
    }
    return QString();
}

void ChatDialog::finishSkillRun(const QString &content,
                                const QString &attachmentPath,
                                const QString &attachmentType)
{
    if (m_currentSession >= 0) {
        QJsonObject message{
            { QStringLiteral("role"), QStringLiteral("assistant") },
            { QStringLiteral("content"), content },
            { QStringLiteral("skill_id"), m_pendingSkillId }
        };
        if (!attachmentPath.isEmpty()) {
            message.insert(QStringLiteral("attachment_path"), attachmentPath);
            message.insert(QStringLiteral("attachment_type"), attachmentType);
        }
        m_sessions[m_currentSession].messages.append(message);
        m_sessions[m_currentSession].updatedAt = QDateTime::currentDateTime();
    }
    m_pendingSkillId.clear();
    m_pendingSkillRequest.clear();
    m_skillRequestId.clear();
    m_instructionSkillId.clear();
    m_streamBrowser = nullptr;
    setGenerating(false);
    m_statusLabel->setText(QStringLiteral("Skill 执行完成。"));
    rebuildMessages();
    rebuildSessionList();
    m_sessionList->setCurrentRow(m_currentSession);
    saveHistory();
    scrollToBottom();
}

void ChatDialog::onStopClicked()
{
    if (!m_generating) return;
    if (!m_pendingSkillId.isEmpty()) {
        if (m_pendingSkillId == QStringLiteral("aegisy.image.generate")) {
            m_apiClient->cancelImageGeneration();
        }
        finishSkillRun(QStringLiteral("Skill 执行已停止。"));
        return;
    }
    m_apiClient->cancelChatMessage();
    if (m_streamContent.isEmpty()) m_streamContent = QStringLiteral("已停止生成。");
    if (m_streamBrowser) m_streamBrowser->setMarkdown(m_streamContent);
    if (m_currentSession >= 0 && !m_streamContent.isEmpty()) {
        ChatSession &session = m_sessions[m_currentSession];
        QJsonObject stoppedMessage{
            { QStringLiteral("role"), QStringLiteral("assistant") },
            { QStringLiteral("content"), m_streamContent }
        };
        if (!m_instructionSkillId.isEmpty()) {
            stoppedMessage.insert(QStringLiteral("skill_id"), m_instructionSkillId);
        }
        session.messages.append(stoppedMessage);
        session.updatedAt = QDateTime::currentDateTime();
    }
    m_requestId.clear();
    m_instructionSkillId.clear();
    setGenerating(false);
    m_statusLabel->setText(QStringLiteral("已停止生成。"));
    rebuildMessages();
    saveHistory();
}

void ChatDialog::onNewSession()
{
    if (m_generating) onStopClicked();
    clearQuickSkill();
    m_instructionSkillId.clear();
    ChatSession session;
    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.title = QStringLiteral("新对话");
    session.createdAt = QDateTime::currentDateTime();
    session.updatedAt = session.createdAt;
    session.keyId = selectedKeyId();
    session.keyName = selectedKeyName();
    session.model = m_modelCombo ? m_modelCombo->currentData().toString() : QString();
    m_sessions.prepend(session);
    m_currentSession = 0;
    rebuildSessionList();
    m_sessionList->setCurrentRow(0);
    saveHistory();
}

void ChatDialog::onSessionChanged(int row)
{
    if (row < 0 || row >= m_sessions.size()) return;
    if (m_generating) onStopClicked();
    clearQuickSkill();
    m_instructionSkillId.clear();
    m_currentSession = row;
    m_editingMessageIndex = -1;
    m_editingLabel->hide();
    m_sendButton->setText(QStringLiteral("发送"));
    m_inputEdit->clear();
    rebuildMessages();
    applyCurrentSessionSelection();
    updateContextInfo();
}

void ChatDialog::onDeleteSession()
{
    if (m_currentSession < 0 || m_currentSession >= m_sessions.size()) return;
    if (m_generating) onStopClicked();
    m_sessions.removeAt(m_currentSession);
    if (m_sessions.isEmpty()) {
        m_currentSession = -1;
        onNewSession();
        return;
    }
    m_currentSession = qMin(m_currentSession, m_sessions.size() - 1);
    rebuildSessionList();
    m_sessionList->setCurrentRow(m_currentSession);
    saveHistory();
}

void ChatDialog::onCopyConversation()
{
    if (m_currentSession < 0 || m_currentSession >= m_sessions.size()) return;
    QString markdown;
    for (const QJsonValue &value : m_sessions[m_currentSession].messages) {
        const QJsonObject message = value.toObject();
        const bool user = message.value(QStringLiteral("role")).toString() == QStringLiteral("user");
        markdown += user ? QStringLiteral("## 你\n\n") : QStringLiteral("## Aegisy\n\n");
        markdown += message.value(QStringLiteral("content")).toString() + QStringLiteral("\n\n");
    }
    QApplication::clipboard()->setText(markdown.trimmed());
    m_statusLabel->setText(QStringLiteral("已复制整个对话。"));
}

void ChatDialog::resendUserMessage(int messageIndex)
{
    if (m_generating || m_currentSession < 0) return;
    ChatSession &session = m_sessions[m_currentSession];
    if (messageIndex < 0 || messageIndex >= session.messages.size()) return;
    truncateMessagesAfter(messageIndex);
    session.updatedAt = QDateTime::currentDateTime();
    rebuildMessages();
    saveHistory();
    const QString text = session.messages.at(messageIndex).toObject()
        .value(QStringLiteral("content")).toString();
    if (!startMatchedSkill(text)) startRequest();
}

void ChatDialog::editUserMessage(int messageIndex)
{
    if (m_generating || m_currentSession < 0) return;
    const QJsonObject message = m_sessions[m_currentSession].messages.at(messageIndex).toObject();
    m_editingMessageIndex = messageIndex;
    m_inputEdit->setPlainText(message.value(QStringLiteral("content")).toString());
    m_inputEdit->setFocus();
    m_editingLabel->setText(QStringLiteral("正在编辑历史消息；发送后将从这里重新生成后续对话。"));
    m_editingLabel->show();
    m_sendButton->setText(QStringLiteral("更新"));
}

void ChatDialog::regenerateAssistantMessage(int messageIndex)
{
    if (m_generating || m_currentSession < 0 || messageIndex <= 0) return;
    truncateMessagesAfter(messageIndex - 1);
    m_sessions[m_currentSession].updatedAt = QDateTime::currentDateTime();
    rebuildMessages();
    saveHistory();
    const QString text = m_sessions[m_currentSession].messages.at(messageIndex - 1).toObject()
        .value(QStringLiteral("content")).toString();
    if (!startMatchedSkill(text)) startRequest();
}

void ChatDialog::copyMessage(const QString &content)
{
    QApplication::clipboard()->setText(content);
    m_statusLabel->setText(QStringLiteral("已复制消息。"));
}

void ChatDialog::truncateMessagesAfter(int messageIndex)
{
    if (m_currentSession < 0) return;
    QJsonArray &messages = m_sessions[m_currentSession].messages;
    while (messages.size() > messageIndex + 1) messages.removeAt(messages.size() - 1);
    m_sessions[m_currentSession].promptTokens = 0;
    m_sessions[m_currentSession].completionTokens = 0;
    m_sessions[m_currentSession].totalTokens = 0;
    updateContextInfo();
}

void ChatDialog::onChatChunk(const QString &requestId, const QString &chunk)
{
    if (requestId != m_requestId || !m_streamBrowser) return;
    m_streamContent += chunk;
    m_streamBrowser->setMarkdown(m_streamContent);
    scrollToBottom();
}

void ChatDialog::onChatUsage(const QString &requestId,
                             int promptTokens,
                             int completionTokens,
                             int totalTokens)
{
    if (requestId != m_requestId || m_currentSession < 0) return;
    ChatSession &session = m_sessions[m_currentSession];
    session.promptTokens = promptTokens;
    session.completionTokens = completionTokens;
    session.totalTokens = totalTokens;
    updateContextInfo();
}

void ChatDialog::onChatCompleted(const QString &requestId, const QString &content)
{
    if (requestId != m_requestId) return;
    const QString finalContent = content.isEmpty() ? m_streamContent : content;
    if (m_streamBrowser) m_streamBrowser->setMarkdown(finalContent);
    if (m_currentSession >= 0 && !finalContent.isEmpty()) {
        ChatSession &session = m_sessions[m_currentSession];
        QJsonObject assistantMessage{
            { QStringLiteral("role"), QStringLiteral("assistant") },
            { QStringLiteral("content"), finalContent }
        };
        if (!m_instructionSkillId.isEmpty()) {
            assistantMessage.insert(QStringLiteral("skill_id"), m_instructionSkillId);
        }
        session.messages.append(assistantMessage);
        session.updatedAt = QDateTime::currentDateTime();
    }
    m_requestId.clear();
    m_instructionSkillId.clear();
    setGenerating(false);
    m_statusLabel->setText(QStringLiteral("回复完成。"));
    rebuildMessages();
    rebuildSessionList();
    m_sessionList->setCurrentRow(m_currentSession);
    updateContextInfo();
    saveHistory();
    scrollToBottom();
}

void ChatDialog::onChatFailed(const QString &requestId, const QString &error)
{
    if (requestId != m_requestId) return;
    if (m_streamBrowser) m_streamBrowser->setPlainText(QStringLiteral("请求失败：%1").arg(error));
    m_requestId.clear();
    m_instructionSkillId.clear();
    setGenerating(false);
    m_statusLabel->setText(QStringLiteral("请求失败：%1").arg(error));
}

void ChatDialog::onRequestFailed(const QString &error)
{
    if (!m_generating) {
        m_keyCombo->setEnabled(true);
        m_modelCombo->setEnabled(true);
        m_sendButton->setEnabled(true);
        m_statusLabel->setText(QStringLiteral("读取失败：%1").arg(error));
    }
}

void ChatDialog::onSkillImageGenerated(const QByteArray &imageData,
                                       const QString &outputFormat,
                                       const QString &revisedPrompt)
{
    if (m_pendingSkillId != QStringLiteral("aegisy.image.generate")) return;
    const QString extension = outputFormat.compare(QStringLiteral("jpeg"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("jpg") : QStringLiteral("png");
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/skill-artifacts/")
        + (m_currentSession >= 0 ? m_sessions[m_currentSession].id : QStringLiteral("shared"));
    QDir().mkpath(directory);
    const QString path = directory + QStringLiteral("/image-")
        + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))
        + QLatin1Char('.') + extension;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(imageData) != imageData.size()
            || !file.commit()) {
        finishSkillRun(QStringLiteral("图片已生成，但保存到本机失败。"));
        return;
    }
    const QString content = revisedPrompt.isEmpty()
        ? QStringLiteral("已通过 **GPT Image 生图 Skill** 生成图片。")
        : QStringLiteral("已通过 **GPT Image 生图 Skill** 生成图片。\n\n优化提示词：%1")
              .arg(revisedPrompt);
    finishSkillRun(content, path, QStringLiteral("image"));
}

void ChatDialog::onSkillImageFailed(const QString &error)
{
    if (m_pendingSkillId != QStringLiteral("aegisy.image.generate")) return;
    finishSkillRun(QStringLiteral("GPT Image Skill 执行失败：%1").arg(error));
}

void ChatDialog::onPresentationPlanReceived(const QString &requestId, const QJsonObject &plan)
{
    if (requestId != m_skillRequestId
            || m_pendingSkillId != QStringLiteral("aegisy.presentation.create")) return;
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/skill-artifacts/")
        + (m_currentSession >= 0 ? m_sessions[m_currentSession].id : QStringLiteral("shared"));
    QDir().mkpath(directory);
    QString fileName = plan.value(QStringLiteral("title")).toString(QStringLiteral("presentation"));
    fileName.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("-"));
    fileName = fileName.trimmed().left(60);
    if (fileName.isEmpty()) fileName = QStringLiteral("presentation");
    const QString path = directory + QLatin1Char('/') + fileName + QStringLiteral(".pptx");
    QString error;
    if (!m_skillManager->executePresentation(plan, path, &error)) {
        finishSkillRun(QStringLiteral("PPT Skill 执行失败：%1").arg(error));
        return;
    }
    finishSkillRun(QStringLiteral("已通过 **PPT 制作 Skill** 生成演示文稿。"),
                   path, QStringLiteral("presentation"));
}

void ChatDialog::onPresentationPlanFailed(const QString &requestId, const QString &error)
{
    if (requestId != m_skillRequestId) return;
    const QString normalized = error.toLower();
    QString friendly = error;
    if (normalized.contains(QStringLiteral("exhausted"))
            || normalized.contains(QStringLiteral("no available"))
            || normalized.contains(QStringLiteral("all available accounts"))) {
        friendly = QStringLiteral(
            "服务器账号池当前已全部占用或额度耗尽（已自动重试仍未成功）。"
            "请稍后再试，或联系管理员补充账号额度。");
    }
    finishSkillRun(QStringLiteral("PPT 大纲生成失败：%1").arg(friendly));
}

void ChatDialog::setGenerating(bool generating)
{
    m_generating = generating;
    m_keyCombo->setEnabled(!generating);
    m_modelCombo->setEnabled(!generating);
    m_sendButton->setEnabled(!generating);
    m_newButton->setEnabled(!generating);
    m_deleteButton->setEnabled(!generating);
    m_sessionList->setEnabled(!generating);
    m_stopButton->setEnabled(generating);
    if (m_imageQuickButton) {
        const SkillInfo image = m_skillManager
            ? m_skillManager->skill(QStringLiteral("aegisy.image.generate")) : SkillInfo();
        m_imageQuickButton->setEnabled(!generating && image.enabled && image.compatible);
    }
    if (m_presentationQuickButton) {
        const SkillInfo presentation = m_skillManager
            ? m_skillManager->skill(QStringLiteral("aegisy.presentation.create")) : SkillInfo();
        m_presentationQuickButton->setEnabled(
            !generating && presentation.enabled && presentation.compatible);
    }
}

void ChatDialog::rebuildSessionList()
{
    const int selected = m_currentSession;
    const QSignalBlocker blocker(m_sessionList);
    m_sessionList->clear();
    for (const ChatSession &session : m_sessions) {
        auto *item = new QListWidgetItem(session.title, m_sessionList);
        const QString time = session.updatedAt.isValid()
            ? session.updatedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
        item->setToolTip(QStringLiteral("%1\n%2\n%3")
                         .arg(session.title, session.model, time));
    }
    if (selected >= 0 && selected < m_sessionList->count()) {
        m_sessionList->setCurrentRow(selected);
    }
}

void ChatDialog::rebuildMessages()
{
    while (m_messagesLayout->count() > 1) {
        QLayoutItem *item = m_messagesLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    m_streamBrowser = nullptr;
    if (m_currentSession < 0 || m_currentSession >= m_sessions.size()) return;
    const QJsonArray &messages = m_sessions[m_currentSession].messages;
    for (int i = 0; i < messages.size(); ++i) {
        const QJsonObject message = messages.at(i).toObject();
        addMessageWidget(message.value(QStringLiteral("role")).toString(),
                         message.value(QStringLiteral("content")).toString(), i);
    }
    updateContextInfo();
    scrollToBottom();
}

void ChatDialog::addMessageWidget(const QString &role,
                                  const QString &content,
                                  int messageIndex,
                                  QTextBrowser **contentBrowser)
{
    const bool user = role == QStringLiteral("user");
    auto *row = new QWidget(m_messagesContainer);
    row->setObjectName(QStringLiteral("messageRow"));
    row->setStyleSheet(QStringLiteral("QWidget#messageRow { background: transparent; }"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(10);
    if (user) {
        rowLayout->addStretch();
    } else {
        rowLayout->addWidget(messageAvatar(false, row), 0, Qt::AlignTop);
    }

    auto *column = new QWidget(row);
    column->setObjectName(QStringLiteral("messageColumn"));
    column->setMaximumWidth(780);
    column->setStyleSheet(QStringLiteral("QWidget#messageColumn { background: transparent; }"));
    auto *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(4);

    if (user) {
        auto *bubble = new QFrame(column);
        bubble->setObjectName(QStringLiteral("userMessageBubble"));
        bubble->setMaximumWidth(680);
        bubble->setStyleSheet(QStringLiteral(
            "QFrame#userMessageBubble { background: #0f766e; border: none; border-radius: 8px; }"));
        auto *bubbleLayout = new QVBoxLayout(bubble);
        bubbleLayout->setContentsMargins(14, 10, 14, 10);
        auto *label = new QLabel(content, bubble);
        label->setTextFormat(Qt::PlainText);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        label->setMinimumWidth(180);
        label->setMaximumWidth(640);
        label->setStyleSheet(QStringLiteral(
            "QLabel { color: white; background: transparent; border: none; font-size: 13px; }"));
        bubbleLayout->addWidget(label);
        columnLayout->addWidget(bubble, 0, Qt::AlignRight);
    } else {
        auto *author = new QLabel(
            m_currentSession >= 0 && !m_sessions[m_currentSession].model.isEmpty()
                ? QStringLiteral("Aegisy · %1").arg(m_sessions[m_currentSession].model)
                : QStringLiteral("Aegisy"), column);
        author->setStyleSheet(QStringLiteral(
            "font-size: 11px; font-weight: 700; color: #667085; background: transparent;"));
        columnLayout->addWidget(author);

        auto *browser = new QTextBrowser(column);
        browser->setObjectName(QStringLiteral("assistantMessageContent"));
        browser->setOpenExternalLinks(true);
        browser->setFrameShape(QFrame::NoFrame);
        browser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        browser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        browser->setMinimumWidth(520);
        browser->setMaximumWidth(760);
        browser->setStyleSheet(QStringLiteral(
            "QTextBrowser#assistantMessageContent { background: transparent; color: #1d2939;"
            " border: none; padding: 0; font-size: 13px; }"));
        browser->document()->setDocumentMargin(0);
        browser->document()->setDefaultStyleSheet(QStringLiteral(
            "body { color: #1d2939; }"
            "pre { background: #f2f4f7; border: 1px solid #e4e7ec; padding: 10px; }"
            "code { font-family: monospace; color: #101828; }"
            "a { color: #0f766e; }"));
        browser->document()->setTextWidth(720);
        browser->setMarkdown(content);
        auto updateHeight = [browser](const QSizeF &size) {
            browser->setFixedHeight(qMax(28, int(size.height()) + 6));
        };
        updateHeight(browser->document()->size());
        connect(browser->document()->documentLayout(),
                &QAbstractTextDocumentLayout::documentSizeChanged,
                browser, updateHeight);
        columnLayout->addWidget(browser);
        if (messageIndex >= 0 && m_currentSession >= 0) {
            const QJsonObject stored = m_sessions[m_currentSession].messages.at(messageIndex).toObject();
            const QString attachmentPath = stored.value(QStringLiteral("attachment_path")).toString();
            const QString attachmentType = stored.value(QStringLiteral("attachment_type")).toString();
            if (!attachmentPath.isEmpty() && QFileInfo::exists(attachmentPath)) {
                if (attachmentType == QStringLiteral("image")) {
                    const QPixmap image(attachmentPath);
                    auto *preview = new QLabel(column);
                    preview->setMaximumSize(640, 460);
                    preview->setPixmap(image.scaled(640, 460, Qt::KeepAspectRatio,
                                                    Qt::SmoothTransformation));
                    preview->setStyleSheet(QStringLiteral(
                        "QLabel { background: white; border: 1px solid #dfe6ee;"
                        " border-radius: 8px; padding: 4px; }"));
                    columnLayout->addWidget(preview, 0, Qt::AlignLeft);
                }
                auto *openButton = new QPushButton(
                    attachmentType == QStringLiteral("presentation")
                        ? QStringLiteral("打开 PPTX") : QStringLiteral("打开图片"), column);
                openButton->setStyleSheet(AppTheme::secondaryButtonStyle());
                openButton->setFixedWidth(110);
                connect(openButton, &QPushButton::clicked, this,
                        [attachmentPath]() {
                            QDesktopServices::openUrl(QUrl::fromLocalFile(attachmentPath));
                        });
                columnLayout->addWidget(openButton, 0, Qt::AlignLeft);
            }
        }
        if (contentBrowser) *contentBrowser = browser;
    }

    if (messageIndex >= 0) {
        auto *actions = new QHBoxLayout();
        actions->setContentsMargins(0, 0, 0, 0);
        actions->setSpacing(2);
        if (user) actions->addStretch();
        auto *copyButton = messageAction(QString(), QStringLiteral("复制"), column);
        actions->addWidget(copyButton);
        connect(copyButton, &QPushButton::clicked, this,
                [this, content]() { copyMessage(content); });
        if (user) {
            auto *editButton = messageAction(QString(), QStringLiteral("编辑"), column);
            auto *resendButton = messageAction(QString(), QStringLiteral("重新发送"), column);
            actions->addWidget(editButton);
            actions->addWidget(resendButton);
            connect(editButton, &QPushButton::clicked, this,
                    [this, messageIndex]() { editUserMessage(messageIndex); });
            connect(resendButton, &QPushButton::clicked, this,
                    [this, messageIndex]() { resendUserMessage(messageIndex); });
        } else {
            auto *regenerateButton = messageAction(QString(), QStringLiteral("重新生成"), column);
            actions->addWidget(regenerateButton);
            connect(regenerateButton, &QPushButton::clicked, this,
                    [this, messageIndex]() { regenerateAssistantMessage(messageIndex); });
        }
        if (!user) actions->addStretch();
        columnLayout->addLayout(actions);
    }

    rowLayout->addWidget(column);
    if (user) {
        rowLayout->addWidget(messageAvatar(true, row), 0, Qt::AlignTop);
    } else {
        rowLayout->addStretch();
    }
    m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, row);
    scrollToBottom();
}

void ChatDialog::scrollToBottom()
{
    QTimer::singleShot(0, this, [this]() {
        m_scrollArea->verticalScrollBar()->setValue(m_scrollArea->verticalScrollBar()->maximum());
    });
}

void ChatDialog::updateSessionTitle(const QString &firstMessage)
{
    ChatSession &session = m_sessions[m_currentSession];
    if (session.title != QStringLiteral("新对话")) return;
    session.title = firstMessage.simplified().left(28);
    if (firstMessage.simplified().size() > 28) session.title += QStringLiteral("...");
}

int ChatDialog::estimatedContextTokens(const QJsonArray &messages) const
{
    int tokens = 0;
    for (const QJsonValue &value : messages) {
        const QString text = value.toObject().value(QStringLiteral("content")).toString();
        int cjk = 0;
        int other = 0;
        for (const QChar character : text) {
            const ushort code = character.unicode();
            if ((code >= 0x3400 && code <= 0x9fff) || (code >= 0x3040 && code <= 0x30ff)) ++cjk;
            else ++other;
        }
        tokens += cjk + (other + 3) / 4 + 4;
    }
    return tokens;
}

int ChatDialog::selectedContextWindow() const
{
    return m_modelCombo->currentData(Qt::UserRole + 1).toInt();
}

void ChatDialog::updateContextInfo()
{
    if (m_currentSession < 0 || m_currentSession >= m_sessions.size()) {
        m_contextLabel->setText(QStringLiteral("上下文 0 条"));
        return;
    }
    const ChatSession &session = m_sessions[m_currentSession];
    const int inputTokens = session.promptTokens > 0
        ? session.promptTokens : estimatedContextTokens(session.messages);
    QString text = session.promptTokens > 0
        ? QStringLiteral("上下文 %1 条 · 输入 %2 · 输出 %3 tokens")
              .arg(session.messages.size()).arg(inputTokens).arg(session.completionTokens)
        : QStringLiteral("上下文 %1 条 · 估算 %2 tokens")
              .arg(session.messages.size()).arg(inputTokens);
    const int contextWindow = selectedContextWindow();
    if (contextWindow > 0) {
        text += QStringLiteral(" / %1").arg(contextWindow);
    }
    m_contextLabel->setText(text);
    m_contextLabel->setToolTip(QStringLiteral(
        "每次请求会发送当前会话中该位置之前的全部消息。服务返回 usage 时显示实际 Token，否则显示本地估算值。"));

    if (m_sessionStatsLabel) {
        if (session.totalTokens > 0) {
            m_sessionStatsLabel->setText(QStringLiteral("↑%1  ↓%2  共%3 tokens")
                .arg(session.promptTokens)
                .arg(session.completionTokens)
                .arg(session.totalTokens));
        } else if (inputTokens > 0) {
            m_sessionStatsLabel->setText(QStringLiteral("估算 ~%1 tokens").arg(inputTokens));
        } else {
            m_sessionStatsLabel->setText(QStringLiteral("暂无用量数据"));
        }
    }
}

void ChatDialog::applyCurrentSessionSelection()
{
    if (m_currentSession < 0 || m_currentSession >= m_sessions.size()
            || m_keyCombo->count() == 0) return;
    const ChatSession &session = m_sessions[m_currentSession];
    m_pendingModel = session.model;
    int keyIndex = -1;
    if (!session.keyId.isEmpty()) {
        for (int i = 0; i < m_keyCombo->count(); ++i) {
            if (m_keyCombo->itemData(i, Qt::UserRole + 1).toString() == session.keyId) {
                keyIndex = i;
                break;
            }
        }
    }
    m_applyingSessionSelection = true;
    if (keyIndex >= 0 && keyIndex != m_keyCombo->currentIndex()) {
        m_keyCombo->setCurrentIndex(keyIndex);
    } else if (!session.model.isEmpty()) {
        const int modelIndex = m_modelCombo->findData(session.model);
        if (modelIndex >= 0) m_modelCombo->setCurrentIndex(modelIndex);
    }
    m_applyingSessionSelection = false;
}

bool ChatDialog::selectActiveProfileKey(bool force)
{
    if (!m_profileManager || m_keyCombo->count() == 0) {
        return false;
    }
    const int activeIndex = m_profileManager->lastActivatedIndex();
    if (activeIndex < 0) {
        return false;
    }
    const Profile profile = m_profileManager->profileWithCredential(activeIndex);
    if (profile.key.isEmpty()) {
        return false;
    }

    int row = -1;
    for (int i = 0; i < m_keyCombo->count(); ++i) {
        if (m_keyCombo->itemData(i).toString() == profile.key) {
            row = i;
            break;
        }
    }
    if (row < 0) {
        // 档案凭据与账号 API Key 是两套体系，对不上时保持当前选择，仅提示。
        m_statusLabel->setText(
            QStringLiteral("已激活档案「%1」，但其 Key 不在账号 API Key 列表中，对话沿用当前所选 Key。")
                .arg(profile.name));
        return false;
    }

    const bool sessionHasKey = m_currentSession >= 0
        && m_currentSession < m_sessions.size()
        && !m_sessions[m_currentSession].keyId.isEmpty();
    if (!force && sessionHasKey) {
        return true;   // 已有会话保留其历史 Key，不覆盖
    }
    if (row != m_keyCombo->currentIndex()) {
        m_keyCombo->setCurrentIndex(row);
    }
    return true;
}

QString ChatDialog::historyPath() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/chat-history.json");
}

void ChatDialog::loadHistory()
{
    QFile file(historyPath());
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonArray sessions = document.object().value(QStringLiteral("sessions")).toArray();
    for (const QJsonValue &value : sessions) {
        const QJsonObject object = value.toObject();
        ChatSession session;
        session.id = object.value(QStringLiteral("id")).toString();
        session.title = object.value(QStringLiteral("title")).toString(QStringLiteral("新对话"));
        session.keyId = object.value(QStringLiteral("key_id")).toString();
        session.keyName = object.value(QStringLiteral("key_name")).toString();
        session.model = object.value(QStringLiteral("model")).toString();
        session.messages = object.value(QStringLiteral("messages")).toArray();
        session.createdAt = QDateTime::fromString(
            object.value(QStringLiteral("created_at")).toString(), Qt::ISODate);
        session.updatedAt = QDateTime::fromString(
            object.value(QStringLiteral("updated_at")).toString(), Qt::ISODate);
        session.promptTokens = object.value(QStringLiteral("prompt_tokens")).toInt();
        session.completionTokens = object.value(QStringLiteral("completion_tokens")).toInt();
        session.totalTokens = object.value(QStringLiteral("total_tokens")).toInt();
        if (session.id.isEmpty()) session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!session.createdAt.isValid()) session.createdAt = QDateTime::currentDateTime();
        if (!session.updatedAt.isValid()) session.updatedAt = session.createdAt;
        m_sessions.append(session);
        if (m_sessions.size() >= kMaximumStoredSessions) break;
    }
}

void ChatDialog::saveHistory() const
{
    const QString path = historyPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonArray sessions;
    const int count = qMin(m_sessions.size(), kMaximumStoredSessions);
    for (int i = 0; i < count; ++i) {
        const ChatSession &session = m_sessions.at(i);
        sessions.append(QJsonObject{
            { QStringLiteral("id"), session.id },
            { QStringLiteral("title"), session.title },
            { QStringLiteral("key_id"), session.keyId },
            { QStringLiteral("key_name"), session.keyName },
            { QStringLiteral("model"), session.model },
            { QStringLiteral("messages"), session.messages },
            { QStringLiteral("created_at"), session.createdAt.toString(Qt::ISODate) },
            { QStringLiteral("updated_at"), session.updatedAt.toString(Qt::ISODate) },
            { QStringLiteral("prompt_tokens"), session.promptTokens },
            { QStringLiteral("completion_tokens"), session.completionTokens },
            { QStringLiteral("total_tokens"), session.totalTokens }
        });
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(QJsonDocument(QJsonObject{
        { QStringLiteral("version"), 1 },
        { QStringLiteral("sessions"), sessions }
    }).toJson(QJsonDocument::Compact));
    file.commit();
}

void ChatDialog::reject()
{
    if (m_pendingSkillId == QStringLiteral("aegisy.image.generate")) {
        m_apiClient->cancelImageGeneration();
    }
    m_apiClient->cancelChatMessage();
    saveHistory();
    QDialog::reject();
}
