#include "chat_dialog.h"

#include "api_client.h"
#include "app_theme.h"

#include <QComboBox>
#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace {

QString maskedKey(const QString &key)
{
    return key.size() <= 12 ? key : key.left(8) + QStringLiteral("...") + key.right(4);
}

} // namespace

ChatDialog::ChatDialog(ApiClient *apiClient, QWidget *parent)
    : QDialog(parent)
    , m_apiClient(apiClient)
{
    setupUi();
    setWindowTitle(QStringLiteral("AI 对话"));
    setMinimumSize(900, 600);
    resize(1160, 740);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    connect(m_apiClient, &ApiClient::apiKeysReceived,
            this, &ChatDialog::onApiKeysReceived);
    connect(m_apiClient, &ApiClient::modelsReceived,
            this, &ChatDialog::onModelsReceived);
    connect(m_apiClient, &ApiClient::chatChunkReceived,
            this, &ChatDialog::onChatChunk);
    connect(m_apiClient, &ApiClient::chatCompleted,
            this, &ChatDialog::onChatCompleted);
    connect(m_apiClient, &ApiClient::chatFailed,
            this, &ChatDialog::onChatFailed);
    connect(m_apiClient, &ApiClient::requestFailed,
            this, &ChatDialog::onRequestFailed);

    onNewSession();
    m_statusLabel->setText(QStringLiteral("正在读取可用 API Key..."));
    m_apiClient->getApiKeys();
}

void ChatDialog::setupUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *sidebar = new QFrame(this);
    sidebar->setFixedWidth(230);
    sidebar->setStyleSheet(QStringLiteral(
        "QFrame { background: #f8fafc; border-right: 1px solid #e4e7ec; }"
        "QLabel { border: none; background: transparent; }"));
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(14, 16, 14, 14);
    sideLayout->setSpacing(10);

    auto *brand = new QLabel(QStringLiteral("AI 对话"), sidebar);
    brand->setStyleSheet(QStringLiteral(
        "font-size: 18px; font-weight: 700; color: #101828;"));
    sideLayout->addWidget(brand);

    m_newButton = new QPushButton(QStringLiteral("新对话"), sidebar);
    m_newButton->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    m_newButton->setMinimumHeight(38);
    m_newButton->setCursor(Qt::PointingHandCursor);
    m_newButton->setStyleSheet(AppTheme::primaryButtonStyle());
    sideLayout->addWidget(m_newButton);

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
    root->addWidget(sidebar);

    auto *main = new QWidget(this);
    main->setStyleSheet(QStringLiteral("QWidget { background: #f5f7fa; }"));
    auto *mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(22, 16, 22, 18);
    mainLayout->setSpacing(12);

    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(8);
    auto *keyLabel = new QLabel(QStringLiteral("API Key"), main);
    keyLabel->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600; color: #344054;"));
    toolbar->addWidget(keyLabel);
    m_keyCombo = new QComboBox(main);
    m_keyCombo->setMinimumWidth(240);
    toolbar->addWidget(m_keyCombo);
    auto *modelLabel = new QLabel(QStringLiteral("模型"), main);
    modelLabel->setStyleSheet(keyLabel->styleSheet());
    toolbar->addWidget(modelLabel);
    m_modelCombo = new QComboBox(main);
    m_modelCombo->setMinimumWidth(220);
    toolbar->addWidget(m_modelCombo);
    toolbar->addStretch();
    m_stopButton = new QPushButton(QStringLiteral("停止"), main);
    m_stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    m_stopButton->setEnabled(false);
    m_stopButton->setStyleSheet(AppTheme::secondaryButtonStyle());
    toolbar->addWidget(m_stopButton);
    mainLayout->addLayout(toolbar);

    m_statusLabel = new QLabel(main);
    m_statusLabel->setStyleSheet(QStringLiteral("font-size: 11px; color: #667085;"));
    mainLayout->addWidget(m_statusLabel);

    m_scrollArea = new QScrollArea(main);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea { background: #f5f7fa; border: none; }"
        "QScrollBar:vertical { background: transparent; width: 8px; }"
        "QScrollBar::handle:vertical { background: #cfd6df; border-radius: 4px; min-height: 32px; }"));
    m_messagesContainer = new QWidget(m_scrollArea);
    m_messagesContainer->setStyleSheet(QStringLiteral("background: #f5f7fa;"));
    m_messagesLayout = new QVBoxLayout(m_messagesContainer);
    m_messagesLayout->setContentsMargins(18, 18, 18, 18);
    m_messagesLayout->setSpacing(14);
    m_messagesLayout->addStretch();
    m_scrollArea->setWidget(m_messagesContainer);
    mainLayout->addWidget(m_scrollArea, 1);

    auto *composer = new QFrame(main);
    composer->setStyleSheet(QStringLiteral(
        "QFrame { background: white; border: 1px solid #d0d5dd; border-radius: 8px; }"));
    auto *composerLayout = new QHBoxLayout(composer);
    composerLayout->setContentsMargins(12, 8, 8, 8);
    composerLayout->setSpacing(10);
    m_inputEdit = new QPlainTextEdit(composer);
    m_inputEdit->setPlaceholderText(QStringLiteral("输入消息，Enter 发送，Shift+Enter 换行"));
    m_inputEdit->setMinimumHeight(64);
    m_inputEdit->setMaximumHeight(120);
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
    connect(m_sessionList, &QListWidget::currentRowChanged,
            this, &ChatDialog::onSessionChanged);
    connect(m_keyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ChatDialog::onKeyChanged);
    connect(m_sendButton, &QPushButton::clicked, this, &ChatDialog::onSendClicked);
    connect(m_stopButton, &QPushButton::clicked, this, &ChatDialog::onStopClicked);
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
    m_keyCombo->blockSignals(true);
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
    }
    if (m_keyCombo->count() == 0) {
        m_keyCombo->addItem(QStringLiteral("没有可用 API Key"), QString());
        m_statusLabel->setText(QStringLiteral("请先在 Key 管理中创建并启用 API Key。"));
    }
    m_keyCombo->blockSignals(false);
    onKeyChanged(m_keyCombo->currentIndex());
}

void ChatDialog::onModelsReceived(const QJsonArray &models)
{
    m_modelCombo->clear();
    for (const QJsonValue &value : models) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString().trimmed();
        if (!id.isEmpty()) m_modelCombo->addItem(id, id);
    }
    if (m_modelCombo->count() == 0) {
        m_modelCombo->addItem(QStringLiteral("没有可用模型"), QString());
        m_statusLabel->setText(QStringLiteral("当前 Key 没有返回可用模型。"));
    } else {
        m_statusLabel->setText(QStringLiteral("已加载 %1 个模型，对话内容仅保存在本次运行内存中。")
                               .arg(m_modelCombo->count()));
    }
    setGenerating(false);
}

void ChatDialog::onKeyChanged(int)
{
    m_modelCombo->clear();
    const QString key = selectedApiKey();
    if (key.isEmpty()) {
        m_modelCombo->addItem(QStringLiteral("请先选择 Key"), QString());
        return;
    }
    m_keyCombo->setEnabled(false);
    m_modelCombo->setEnabled(false);
    m_sendButton->setEnabled(false);
    m_modelCombo->addItem(QStringLiteral("正在读取模型..."), QString());
    m_statusLabel->setText(QStringLiteral("正在读取当前 Key 的模型列表..."));
    m_apiClient->getModels(key);
}

QString ChatDialog::selectedApiKey() const
{
    return m_keyCombo->currentData().toString();
}

void ChatDialog::onSendClicked()
{
    if (m_generating) return;
    const QString text = m_inputEdit->toPlainText().trimmed();
    const QString key = selectedApiKey();
    const QString model = m_modelCombo->currentData().toString();
    if (text.isEmpty()) return;
    if (key.isEmpty() || model.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法发送"),
                             QStringLiteral("请选择可用的 API Key 和模型。"));
        return;
    }
    if (m_currentSession < 0 || m_currentSession >= m_sessions.size()) onNewSession();

    ChatSession &session = m_sessions[m_currentSession];
    session.messages.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("user") },
        { QStringLiteral("content"), text }
    });
    addMessageWidget(QStringLiteral("user"), text);
    updateSessionTitle(text);
    m_inputEdit->clear();

    m_streamContent.clear();
    addMessageWidget(QStringLiteral("assistant"), QStringLiteral("正在思考..."), &m_streamBrowser);
    m_requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    setGenerating(true);
    m_statusLabel->setText(QStringLiteral("正在生成回复..."));
    m_apiClient->sendChatMessage(m_requestId, key, model, session.messages);
}

void ChatDialog::onStopClicked()
{
    if (!m_generating) return;
    m_apiClient->cancelChatMessage();
    if (m_streamContent.isEmpty()) m_streamContent = QStringLiteral("已停止生成。" );
    if (m_streamBrowser) m_streamBrowser->setMarkdown(m_streamContent);
    if (m_currentSession >= 0 && !m_streamContent.isEmpty()) {
        m_sessions[m_currentSession].messages.append(QJsonObject{
            { QStringLiteral("role"), QStringLiteral("assistant") },
            { QStringLiteral("content"), m_streamContent }
        });
    }
    m_requestId.clear();
    setGenerating(false);
    m_statusLabel->setText(QStringLiteral("已停止生成。"));
}

void ChatDialog::onNewSession()
{
    if (m_generating) onStopClicked();
    ChatSession session;
    session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.title = QStringLiteral("新对话");
    m_sessions.prepend(session);
    m_sessionList->insertItem(0, session.title);
    m_sessionList->setCurrentRow(0);
}

void ChatDialog::onSessionChanged(int row)
{
    if (row < 0 || row >= m_sessions.size()) return;
    if (m_generating) onStopClicked();
    m_currentSession = row;
    rebuildMessages();
}

void ChatDialog::onDeleteSession()
{
    if (m_currentSession < 0 || m_currentSession >= m_sessions.size()) return;
    if (m_generating) onStopClicked();
    delete m_sessionList->takeItem(m_currentSession);
    m_sessions.removeAt(m_currentSession);
    if (m_sessions.isEmpty()) {
        m_currentSession = -1;
        onNewSession();
    } else {
        m_sessionList->setCurrentRow(qMin(m_currentSession, m_sessions.size() - 1));
    }
}

void ChatDialog::onChatChunk(const QString &requestId, const QString &chunk)
{
    if (requestId != m_requestId || !m_streamBrowser) return;
    m_streamContent += chunk;
    m_streamBrowser->setMarkdown(m_streamContent);
    scrollToBottom();
}

void ChatDialog::onChatCompleted(const QString &requestId, const QString &content)
{
    if (requestId != m_requestId) return;
    const QString finalContent = content.isEmpty() ? m_streamContent : content;
    if (m_streamBrowser) m_streamBrowser->setMarkdown(finalContent);
    if (m_currentSession >= 0 && !finalContent.isEmpty()) {
        m_sessions[m_currentSession].messages.append(QJsonObject{
            { QStringLiteral("role"), QStringLiteral("assistant") },
            { QStringLiteral("content"), finalContent }
        });
    }
    m_requestId.clear();
    setGenerating(false);
    m_statusLabel->setText(QStringLiteral("回复完成。"));
    scrollToBottom();
}

void ChatDialog::onChatFailed(const QString &requestId, const QString &error)
{
    if (requestId != m_requestId) return;
    if (m_streamBrowser) m_streamBrowser->setPlainText(QStringLiteral("请求失败：%1").arg(error));
    m_requestId.clear();
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

void ChatDialog::setGenerating(bool generating)
{
    m_generating = generating;
    m_keyCombo->setEnabled(!generating);
    m_modelCombo->setEnabled(!generating);
    m_sendButton->setEnabled(!generating);
    m_stopButton->setEnabled(generating);
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
    for (const QJsonValue &value : m_sessions[m_currentSession].messages) {
        const QJsonObject message = value.toObject();
        addMessageWidget(message.value(QStringLiteral("role")).toString(),
                         message.value(QStringLiteral("content")).toString());
    }
    scrollToBottom();
}

void ChatDialog::addMessageWidget(const QString &role, const QString &content,
                                  QTextBrowser **contentBrowser)
{
    const bool user = role == QStringLiteral("user");
    auto *row = new QWidget(m_messagesContainer);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    if (user) rowLayout->addStretch();

    auto *bubble = new QFrame(row);
    bubble->setMinimumWidth(user ? 240 : 620);
    bubble->setMaximumWidth(760);
    bubble->setStyleSheet(user
        ? QStringLiteral("QFrame { background: #0f766e; border-radius: 8px; }")
        : QStringLiteral("QFrame { background: white; border: 1px solid #e4e7ec; border-radius: 8px; }"));
    auto *bubbleLayout = new QVBoxLayout(bubble);
    bubbleLayout->setContentsMargins(14, 10, 14, 10);
    auto *browser = new QTextBrowser(bubble);
    browser->setOpenExternalLinks(true);
    browser->setFrameShape(QFrame::NoFrame);
    browser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    browser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    browser->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    browser->document()->setDocumentMargin(0);
    browser->setStyleSheet(user
        ? QStringLiteral("QTextBrowser { background: transparent; color: white; font-size: 13px; }")
        : QStringLiteral("QTextBrowser { background: transparent; color: #1d2939; font-size: 13px; }"));
    browser->setMarkdown(content);
    browser->setMinimumHeight(qMax(24, int(browser->document()->size().height()) + 4));
    connect(browser->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            browser, [browser](const QSizeF &size) { browser->setMinimumHeight(int(size.height()) + 4); });
    bubbleLayout->addWidget(browser);
    rowLayout->addWidget(bubble);
    if (!user) rowLayout->addStretch();
    m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, row);
    if (contentBrowser) *contentBrowser = browser;
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
    session.title = firstMessage.simplified().left(24);
    if (firstMessage.simplified().size() > 24) session.title += QStringLiteral("...");
    if (auto *item = m_sessionList->item(m_currentSession)) item->setText(session.title);
}

void ChatDialog::reject()
{
    m_apiClient->cancelChatMessage();
    QDialog::reject();
}
