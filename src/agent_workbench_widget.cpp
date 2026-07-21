#include "agent_workbench_widget.h"

#include "agent_runtime_client.h"
#ifdef AEGISY_HAS_MONACO
#include "monaco_editor_bridge.h"
#include "terminal_web_bridge.h"
#endif

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QColor>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QInputDialog>
#include <QMenu>
#include <QPalette>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStyle>
#ifdef AEGISY_HAS_MONACO
#include <QStackedWidget>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#endif
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextDocument>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <algorithm>
#include <utility>

namespace {
const QString kPanelBorder = QStringLiteral("#e4e7ec");
constexpr int kPathRole = Qt::UserRole;
constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kLoadedRole = Qt::UserRole + 2;
constexpr int kRevisionRole = Qt::UserRole + 3;
constexpr int kSearchLineRole = Qt::UserRole + 4;
constexpr int kSearchColumnRole = Qt::UserRole + 5;
constexpr int kSymbolLineRole = Qt::UserRole + 6;
constexpr int kGitOidRole = Qt::UserRole + 7;
constexpr int kSymbolColumnRole = Qt::UserRole + 7;
constexpr int kContextRole = Qt::UserRole + 8;
constexpr int kPatchDiffRole = Qt::UserRole + 9;
constexpr int kPatchReferenceRole = Qt::UserRole + 10;
constexpr int kPatchInlineTruncatedRole = Qt::UserRole + 11;
constexpr int kSessionIdRole = Qt::UserRole + 20;
constexpr int kSessionModeRole = Qt::UserRole + 21;
constexpr int kSessionProjectRole = Qt::UserRole + 22;
constexpr int kSessionStatusRole = Qt::UserRole + 23;
constexpr int kSessionTitleRole = Qt::UserRole + 24;
constexpr int kSessionRecoveryRole = Qt::UserRole + 25;
constexpr int kSessionDeletionPendingRole = Qt::UserRole + 26;
constexpr int kSessionDeletionIdRole = Qt::UserRole + 27;
constexpr int kSessionDeletionUndoUntilRole = Qt::UserRole + 28;
constexpr int kProjectIdRole = Qt::UserRole + 40;
constexpr int kProjectRootRole = Qt::UserRole + 41;
constexpr int kProjectAvailabilityRole = Qt::UserRole + 42;
constexpr int kProjectPinnedRole = Qt::UserRole + 43;
constexpr int kProjectRelinkRole = Qt::UserRole + 44;
constexpr int kMaxTurnContextItems = 16;
constexpr int kMaxInlineContextBytes = 16 * 1024;
constexpr qint64 kMaxPortableSessionBytes = 4LL * 1024 * 1024;

bool isLowerHex(const QString &value, int length)
{
    return value.size() == length
        && std::all_of(value.cbegin(), value.cend(), [](QChar character) {
            return (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
                || (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
        });
}

const QList<QPair<QString, QString>> &compactionSummaryCategories()
{
    static const QList<QPair<QString, QString>> categories{
        {QStringLiteral("decisions"), QStringLiteral("决策")},
        {QStringLiteral("unresolved_tasks"), QStringLiteral("未解决任务")},
        {QStringLiteral("changed_files"), QStringLiteral("变更文件")},
        {QStringLiteral("commands"), QStringLiteral("命令")},
        {QStringLiteral("tests"), QStringLiteral("测试")},
        {QStringLiteral("failures"), QStringLiteral("失败")},
        {QStringLiteral("next_actions"), QStringLiteral("下一步")},
    };
    return categories;
}

struct CompactionReviewFormData {
    bool accepted = false;
    QString checkpointId;
    QString preservationInstructions;
    QJsonObject summary;
    QString error;
};

CompactionReviewFormData promptCompactionReview(
    QWidget *parent, const QString &title, const QString &introText,
    const QString &submitText, const QString &initialCheckpointId = {},
    const QString &initialPreservation = {}, const QJsonObject &initialSummary = {})
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.resize(620, 620);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);
    auto *intro = new QLabel(introText, &dialog);
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color:#667085; font-size:10px;"));
    layout->addWidget(intro);
    auto *checkpointId = new QLineEdit(&dialog);
    checkpointId->setObjectName(QStringLiteral("agentCompactionCheckpointIdInput"));
    checkpointId->setMaxLength(128);
    checkpointId->setPlaceholderText(QStringLiteral("例如：checkpoint-before-refactor"));
    checkpointId->setText(initialCheckpointId);
    layout->addWidget(new QLabel(QStringLiteral("审查 ID"), &dialog));
    layout->addWidget(checkpointId);
    auto *preservation = new QPlainTextEdit(&dialog);
    preservation->setObjectName(QStringLiteral("agentCompactionPreservationInput"));
    preservation->setPlaceholderText(QStringLiteral("可选：需要保留的上下文和注意事项"));
    preservation->setPlainText(initialPreservation);
    preservation->setFixedHeight(58);
    preservation->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    layout->addWidget(new QLabel(QStringLiteral("保留指令（可选）"), &dialog));
    layout->addWidget(preservation);

    QHash<QString, QPlainTextEdit *> fields;
    for (const auto &category : compactionSummaryCategories()) {
        layout->addWidget(new QLabel(category.second, &dialog));
        auto *field = new QPlainTextEdit(&dialog);
        field->setObjectName(QStringLiteral("agentCompaction%1Input").arg(category.first));
        field->setPlaceholderText(QStringLiteral("每行一项，最多 64 项"));
        QStringList initialValues;
        for (const QJsonValue &value : initialSummary.value(category.first).toArray()) {
            initialValues.append(value.toString());
        }
        field->setPlainText(initialValues.join(QLatin1Char('\n')));
        field->setFixedHeight(48);
        field->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        fields.insert(category.first, field);
        layout->addWidget(field);
    }
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setObjectName(
        QStringLiteral("agentCompactionReviewSubmitButton"));
    buttons->button(QDialogButtonBox::Ok)->setText(submitText);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return {};

    CompactionReviewFormData result;
    result.checkpointId = checkpointId->text().trimmed();
    static const QRegularExpression checkpointPattern(
        QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    if (!checkpointPattern.match(result.checkpointId).hasMatch()) {
        result.error = QStringLiteral("审查 ID 只能包含字母、数字、下划线或连字符，且不超过 128 个字符。");
        return result;
    }
    result.preservationInstructions = preservation->toPlainText().trimmed();
    if (result.preservationInstructions.toUtf8().size() > 8 * 1024) {
        result.error = QStringLiteral("保留指令超过 8 KiB，未提交。");
        return result;
    }
    int summaryBytes = 0;
    for (const auto &category : compactionSummaryCategories()) {
        QJsonArray values;
        const QStringList lines = fields.value(category.first)->toPlainText().split(
            QLatin1Char('\n'), Qt::KeepEmptyParts);
        for (const QString &line : lines) {
            const QString value = line.trimmed();
            if (value.isEmpty()) continue;
            const int itemBytes = value.toUtf8().size();
            if (itemBytes > 1024) {
                result.error = QStringLiteral("%1中有单项超过 1024 字节，未提交。")
                    .arg(category.second);
                return result;
            }
            if (values.size() >= 64) {
                result.error = QStringLiteral("%1超过 64 项，未提交。").arg(category.second);
                return result;
            }
            summaryBytes += itemBytes;
            values.append(value);
        }
        result.summary.insert(category.first, values);
    }
    if (summaryBytes > 64 * 1024) {
        result.error = QStringLiteral("摘要内容超过 64 KiB，未提交。");
        return result;
    }
    result.accepted = true;
    return result;
}

QString formatByteCount(qint64 bytes)
{
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes >= 1024) return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

QString boundedUtf8Text(const QString &text, int byteLimit, bool *truncated = nullptr)
{
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() <= byteLimit) {
        if (truncated) *truncated = false;
        return text;
    }
    int low = 0;
    int high = text.size();
    while (low < high) {
        const int middle = low + (high - low + 1) / 2;
        if (text.left(middle).toUtf8().size() <= byteLimit) low = middle;
        else high = middle - 1;
    }
    if (truncated) *truncated = true;
    return text.left(low);
}

bool isBoundedRuntimeMetadata(const QString &value, int byteLimit, bool required)
{
    const QByteArray utf8 = value.toUtf8();
    if ((required && value.isEmpty()) || utf8.size() > byteLimit) return false;
    return std::none_of(utf8.cbegin(), utf8.cend(), [](char byte) {
        const auto value = static_cast<unsigned char>(byte);
        return value < 0x20 || value == 0x7f;
    });
}

bool isLowerHexIdentity(const QString &value, int firstLength, int secondLength = -1)
{
    if (value.size() != firstLength && value.size() != secondLength) return false;
    return std::all_of(value.cbegin(), value.cend(), [](QChar character) {
        const ushort code = character.unicode();
        return (code >= '0' && code <= '9') || (code >= 'a' && code <= 'f');
    });
}

QString runtimeMetadataLabel(const QJsonObject &binding, const QString &field)
{
    const QString value = binding.value(field).toString();
    return value.isEmpty() ? QStringLiteral("--") : value;
}

QString safeProviderLifecycleFailure(const QString &method, const QString &message, int code)
{
    const bool providerFailure = code == -32141 || code == -32143 || code == -32145
        || message.contains(QStringLiteral("provider"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("codex"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("model"), Qt::CaseInsensitive);
    if (!providerFailure) return message;
    const QString operation = method == QStringLiteral("session/archive")
        ? QStringLiteral("归档会话")
        : method == QStringLiteral("session/unarchive")
            ? QStringLiteral("恢复会话")
            : method == QStringLiteral("session/fork")
                ? QStringLiteral("创建会话分支")
                : QStringLiteral("恢复会话");
    return QStringLiteral("%1失败（错误码 %2；provider 详细信息已隐藏）")
        .arg(operation).arg(code);
}

QString safeRuntimeRestartFailure(int code)
{
    return QStringLiteral("Codex 重连失败（错误码 %1；运行时详细信息已隐藏，请检查安装与配置后重试）")
        .arg(code);
}

QString boundedContextText(const QString &text, bool *truncated = nullptr)
{
    return boundedUtf8Text(text, kMaxInlineContextBytes, truncated);
}

#ifdef AEGISY_HAS_MONACO
class LocalOnlyRequestInterceptor final : public QWebEngineUrlRequestInterceptor
{
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QString scheme = info.requestUrl().scheme().toLower();
        if (scheme != QStringLiteral("file") && scheme != QStringLiteral("qrc")
                && scheme != QStringLiteral("data") && scheme != QStringLiteral("blob")) {
            info.block(true);
        }
    }
};

class TrustedWorkbenchPage final : public QWebEnginePage
{
public:
    TrustedWorkbenchPage(QWebEngineProfile *profile, const QUrl &entry, QObject *parent)
        : QWebEnginePage(profile, parent)
        , m_entry(entry)
    {
    }

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool) override
    {
        return url == m_entry || url == QUrl(QStringLiteral("about:blank"));
    }

private:
    QUrl m_entry;
};

QString monacoEntryPath()
{
    const QString overrideRoot = qEnvironmentVariable("AEGISY_WORKBENCH_WEB_ROOT");
    if (!overrideRoot.isEmpty()) {
        return QDir::cleanPath(QDir(overrideRoot).absoluteFilePath(
            QStringLiteral("index.html")));
    }
    QDir directory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    return QDir::cleanPath(directory.absoluteFilePath(
        QStringLiteral("../Resources/workbench/index.html")));
#else
    return QDir::cleanPath(directory.absoluteFilePath(
        QStringLiteral("workbench/index.html")));
#endif
}

QString terminalEntryPath()
{
    const QString overrideRoot = qEnvironmentVariable("AEGISY_WORKBENCH_WEB_ROOT");
    if (!overrideRoot.isEmpty()) {
        return QDir::cleanPath(QDir(overrideRoot).absoluteFilePath(
            QStringLiteral("terminal.html")));
    }
    QDir directory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    return QDir::cleanPath(directory.absoluteFilePath(
        QStringLiteral("../Resources/workbench/terminal.html")));
#else
    return QDir::cleanPath(directory.absoluteFilePath(
        QStringLiteral("workbench/terminal.html")));
#endif
}
#endif

QLabel *makeSectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral(
        "color:#98a2b3; font-size:10px; font-weight:700; padding:0 2px;"));
    return label;
}

QWidget *makePlaceholder(const QString &title, const QString &detail, QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(28, 26, 28, 28);
    layout->setSpacing(6);
    auto *heading = new QLabel(title, widget);
    heading->setStyleSheet(QStringLiteral("color:#101828; font-size:16px; font-weight:700;"));
    auto *copy = new QLabel(detail, widget);
    copy->setWordWrap(true);
    copy->setStyleSheet(QStringLiteral("color:#667085; font-size:12px;"));
    layout->addWidget(heading);
    layout->addWidget(copy);
    layout->addStretch();
    return widget;
}

QString breadcrumbForPath(const QString &path)
{
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts).join(QStringLiteral(" / "));
}

int editorOffsetForLineColumn(const QString &content, int line, int column)
{
    const int targetLine = qMax(1, line);
    int lineStart = 0;
    for (int currentLine = 1; currentLine < targetLine; ++currentLine) {
        const int newline = content.indexOf(QLatin1Char('\n'), lineStart);
        if (newline < 0) return content.size();
        lineStart = newline + 1;
    }
    const int lineEnd = content.indexOf(QLatin1Char('\n'), lineStart);
    const int length = (lineEnd < 0 ? content.size() : lineEnd) - lineStart;
    return lineStart + qBound(0, column - 1, length);
}
}

AgentWorkbenchWidget::AgentWorkbenchWidget(QWidget *parent)
    : QWidget(parent)
    , m_runtime(new AgentRuntimeClient(this))
{
    buildUi();
    m_workspaceWatchTimer = new QTimer(this);
    m_workspaceWatchTimer->setSingleShot(true);
    m_workspaceWatchTimer->setInterval(1500);
    connect(m_workspaceWatchTimer, &QTimer::timeout, this, [this]() {
        if (!m_workspaceWatchId.isEmpty() && m_runtime->isReady()) {
            m_runtime->pollWorkspaceWatch(m_workspaceWatchId);
        }
    });
    m_gitStatusTimer = new QTimer(this);
    m_gitStatusTimer->setSingleShot(true);
    m_gitStatusTimer->setInterval(5000);
    connect(m_gitStatusTimer, &QTimer::timeout,
            this, &AgentWorkbenchWidget::refreshGitStatus);
    m_terminalPollTimer = new QTimer(this);
    m_terminalPollTimer->setSingleShot(true);
    m_terminalPollTimer->setInterval(75);
    connect(m_terminalPollTimer, &QTimer::timeout,
            this, &AgentWorkbenchWidget::pollActiveTerminal);
    m_runtimeHealthTimer = new QTimer(this);
    m_runtimeHealthTimer->setInterval(5000);
    connect(m_runtimeHealthTimer, &QTimer::timeout, this, [this]() {
        if (m_runtime->isReady()) m_runtime->runtimeHealth();
    });
    m_runtimeHealthTimer->start();

    connect(m_runtime, &AgentRuntimeClient::runtimeInitialized,
            this, [this](const QJsonObject &result) {
        const QJsonObject backend = result.value(QStringLiteral("backend")).toObject();
        m_runtimeRecoveryMode = backend.value(QStringLiteral("status")).toString()
            == QStringLiteral("read-only-recovery");
        m_modelProfileReadOnlyAvailable = false;
        m_modelProfileSnapshotValid = false;
        m_modelProfileCount = 0;
        m_compactionAvailable = false;
        m_backgroundNotificationInspectionAvailable = false;
        m_backgroundRecoveryInspectionAvailable = false;
        m_pinnedContextAvailable = false;
        m_imageContextAvailable = false;
        m_gitContextAvailable = false;
        bool imageImportAvailable = false;
        bool imagePreviewAvailable = false;
        for (const QJsonValue &capability : result.value(QStringLiteral("capabilities")).toArray()) {
            const QString name = capability.toString();
            if (name == QStringLiteral("session.compaction.checkpoint-review")) {
                m_compactionAvailable = true;
            } else if (name == QStringLiteral("background-notification.outbox.read-only")) {
                m_backgroundNotificationInspectionAvailable = true;
            } else if (name == QStringLiteral("background-job.recovery.inspect")) {
                m_backgroundRecoveryInspectionAvailable = true;
            } else if (name == QStringLiteral("turn.context.pinned-selected")) {
                m_pinnedContextAvailable = true;
            } else if (name == QStringLiteral("workspace.git-context.read-only")) {
                m_gitContextAvailable = true;
            } else if (name == QStringLiteral("workspace.image.import-user")) {
                imageImportAvailable = true;
            } else if (name == QStringLiteral("workspace.image.preview")) {
                imagePreviewAvailable = true;
            } else if (name == QStringLiteral("model.profile.read-only")) {
                m_modelProfileReadOnlyAvailable = true;
            }
        }
        m_imageContextAvailable = imageImportAvailable && imagePreviewAvailable;
        if (m_pinnedContextAvailable && !m_projectId.isEmpty()) requestPinnedContext();
        if (m_pinFileContextAction) {
            m_pinFileContextAction->setEnabled(
                m_pinnedContextAvailable && !m_runtimeRecoveryMode && !m_projectId.isEmpty());
        }
        if (m_pinImageContextAction) {
            m_pinImageContextAction->setEnabled(m_pinnedContextAvailable
                && m_imageContextAvailable && !m_runtimeRecoveryMode
                && !m_projectId.isEmpty() && !m_workSessionId.isEmpty());
        }
        m_runtimeRestartRequired = false;
        m_runtimeDegradationsAvailable = false;
        m_runtimeDegradationStates.clear();
        updateRuntimeCapabilityUi();
        updateRecoveryUi();
        updateGitPinControls();
    });
    connect(m_runtime, &AgentRuntimeClient::modelProfilesListed,
            this, [this](const QString &, const QJsonObject &result) {
        const QJsonArray profiles = result.value(QStringLiteral("profiles")).toArray();
        const bool authoritySafe = !result.value(QStringLiteral("selection_allowed")).toBool(true)
            && !result.value(QStringLiteral("routing_authority")).toBool(true)
            && !result.value(QStringLiteral("token_issued")).toBool(true)
            && !result.value(QStringLiteral("turn_started")).toBool(true);
        m_modelProfileSnapshotValid = result.value(QStringLiteral("schema_version")).toString()
                == QStringLiteral("model-profile-list/0.1")
            && result.value(QStringLiteral("store_schema_version")).toString()
                == QStringLiteral("model-profile-store/0.1")
            && profiles.size() <= 256
            && authoritySafe;
        m_modelProfileCount = m_modelProfileSnapshotValid ? profiles.size() : 0;
        updateSessionRuntimePresentation();
    });
    connect(m_runtime, &AgentRuntimeClient::modelProfileRead,
            this, [this](const QString &, const QJsonObject &) {
        // Profile reads are available for future detail views; the picker remains read-only.
    });
    connect(m_runtime, &AgentRuntimeClient::runtimeDegradationsRead,
            this, [this](const QString &, const QJsonObject &result) {
        if (result.value(QStringLiteral("schema_version")).toString()
                != QStringLiteral("runtime-degradations/0.1")) {
            m_runtimeDegradationsAvailable = false;
            m_runtimeDegradationStates.clear();
            updateRuntimeCapabilityUi();
            return;
        }
        m_runtimeDegradationStates.clear();
        for (const QJsonValue &value : result.value(QStringLiteral("degradations")).toArray()) {
            const QJsonObject degradation = value.toObject();
            const QString feature = degradation.value(QStringLiteral("feature")).toString();
            const QString state = degradation.value(QStringLiteral("state")).toString();
            if (!feature.isEmpty() && !state.isEmpty()) {
                m_runtimeDegradationStates.insert(feature, state);
            }
        }
        m_runtimeDegradationsAvailable = true;
        updateRuntimeCapabilityUi();
    });
    connect(m_runtime, &AgentRuntimeClient::runtimeHealthRead,
            this, [this](const QJsonObject &health) {
        const QString state = health.value(QStringLiteral("state")).toString();
        m_runtimeRestartRequired = health.value(QStringLiteral("restart_required")).toBool();
        if (state == QStringLiteral("exited") || state == QStringLiteral("unavailable")) {
            m_runtimeStatus->setText(QStringLiteral("○ Codex 不可用"));
            m_runtimeStatus->setToolTip(QStringLiteral("Codex 进程已退出，可点击重启"));
        }
        updateRecoveryUi();
    });
    connect(m_runtime, &AgentRuntimeClient::runtimeRestarted,
            this, [this](const QString &, const QJsonObject &result) {
        m_runtimeRestartRequired = false;
        const QJsonObject health = result.value(QStringLiteral("health")).toObject();
        m_runtimeStatus->setText(QStringLiteral("● Codex 已恢复"));
        m_runtimeStatus->setToolTip(QStringLiteral("Codex 运行时已重新连接"));
        if (m_runtimeRestartButton) {
            m_runtimeRestartButton->setText(QStringLiteral("重启 Codex"));
        }
        if (health.value(QStringLiteral("state")).toString() != QStringLiteral("running")) {
            m_runtimeRestartRequired = true;
        }
        updateRecoveryUi();
    });
    connect(m_runtime, &AgentRuntimeClient::projectionRecoveryStatusRead,
            this, [this](const QJsonObject &status) {
        const QJsonObject startup = status.value(QStringLiteral("startup")).toObject();
        m_startupRebuiltSessionCount = startup.value(
            QStringLiteral("rebuilt_sessions")).toVariant().toULongLong();
        m_quarantinedSessionCount = status.value(
            QStringLiteral("current_quarantined_sessions")).toVariant().toULongLong();
        updateRecoveryUi();
    });
    connect(m_runtime, &AgentRuntimeClient::runtimeRecoveryStatusRead,
            this, [this](const QJsonObject &status) {
        if (m_recoveryBanner) {
            m_recoveryBanner->setToolTip(
                status.value(QStringLiteral("reason_code")).toString());
        }
        updateRecoveryUi();
    });
    connect(m_runtime, &AgentRuntimeClient::sessionRecoveryStatusRead,
            this, [this](const QJsonObject &status) {
        const QString sessionId = status.value(QStringLiteral("session_id")).toString();
        if (status.value(QStringLiteral("recovery_required")).toBool()) {
            m_recoverySessionIds.insert(sessionId);
        } else {
            m_recoverySessionIds.remove(sessionId);
        }
        if (m_recoveryBanner) {
            QStringList issues;
            for (const QJsonValue &value : status.value(QStringLiteral("issues")).toArray()) {
                issues.append(value.toString());
            }
            if (!issues.isEmpty()) m_recoveryBanner->setToolTip(issues.join(QLatin1Char('\n')));
        }
        updateRecoveryUi();
    });
    connect(m_runtime, &AgentRuntimeClient::operationStatusRead,
            this, [this](const QString &requestId, const QJsonObject &status) {
        if (!requestId.isEmpty() && requestId != m_operationStatusRequestId) return;
        if (requestId.isEmpty() || requestId == m_operationStatusRequestId) {
            m_operationStatusRequestId.clear();
        }
        updateOperationStatusUi(status);
    });
    connect(m_runtime, &AgentRuntimeClient::operationProbeRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (!requestId.isEmpty() && requestId != m_operationProbeRequestId) return;
        if (requestId.isEmpty() || requestId == m_operationProbeRequestId) {
            m_operationProbeRequestId.clear();
        }
        const QString sessionId = m_operationStatusSessionId;
        const QString operationId = m_operationReview.value(QStringLiteral("operation_id"))
            .toString();
        const QString kind = m_operationReview.value(QStringLiteral("kind")).toString();
        const bool valid = result.value(QStringLiteral("schema_version")).toString()
                == QStringLiteral("operation-reconciliation-probe/0.1")
            && result.value(QStringLiteral("session_id")).toString() == sessionId
            && result.value(QStringLiteral("operation_id")).toString() == operationId
            && result.value(QStringLiteral("kind")).toString() == kind
            && result.value(QStringLiteral("evidence")).isObject();
        if (!valid || operationId.isEmpty() || sessionId.isEmpty() || kind.isEmpty()) {
            showOperationReviewFailure(
                QStringLiteral("重新检查返回了无效的 operation/probe 结果，当前会话继续保持只读。"));
            return;
        }
        m_operationReconcileRequestId = m_runtime->operationReconcile({
            {QStringLiteral("operation_id"), operationId},
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("evidence"), result.value(QStringLiteral("evidence"))},
        });
        if (m_operationReconcileRequestId.isEmpty()) {
            showOperationReviewFailure(
                QStringLiteral("无法提交重新检查结果，当前会话继续保持只读。"));
            return;
        }
        if (m_operationStatusBanner) {
            m_operationStatusBanner->setText(QStringLiteral(
                "正在记录重新检查结果；在明确终态前不会恢复会话写入。"));
        }
        updateOperationReviewButton();
    });
    connect(m_runtime, &AgentRuntimeClient::operationReconciled,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (!requestId.isEmpty() && requestId != m_operationReconcileRequestId) return;
        if (requestId.isEmpty() || requestId == m_operationReconcileRequestId) {
            m_operationReconcileRequestId.clear();
        }
        const QJsonObject reconciliation = result.value(QStringLiteral("reconciliation"))
            .toObject();
        const bool valid = result.value(QStringLiteral("schema_version")).toString()
                == QStringLiteral("operation-reconciliation-result/0.1")
            && reconciliation.value(QStringLiteral("session_id")).toString()
                == m_operationStatusSessionId
            && reconciliation.value(QStringLiteral("operation_id")).toString()
                == m_operationReview.value(QStringLiteral("operation_id")).toString();
        if (!valid) {
            showOperationReviewFailure(
                QStringLiteral("重新检查结果未通过身份校验，当前会话继续保持只读。"));
            return;
        }
        addNotice(QStringLiteral("已完成一次只读操作复核；运行时将按新证据重新评估会话门控。"));
        requestOperationStatus();
    });
    connect(m_runtime, &AgentRuntimeClient::compactionCheckpointCreated,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_compactionRequestId
                || m_compactionOperation != QStringLiteral("create")) return;
        m_compactionRequestId.clear();
        m_compactionOperation.clear();
        showCompactionReview(result, result.value(QStringLiteral("idempotent_replay")).toBool());
    });
    connect(m_runtime, &AgentRuntimeClient::compactionCheckpointRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_compactionRequestId
                || m_compactionOperation != QStringLiteral("read")) return;
        m_compactionRequestId.clear();
        m_compactionOperation.clear();
        showCompactionReview(result, true);
    });
    connect(m_runtime, &AgentRuntimeClient::compactionCheckpointRevised,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_compactionRequestId
                || m_compactionOperation != QStringLiteral("revise")) return;
        m_compactionRequestId.clear();
        m_compactionOperation.clear();
        showCompactionReview(result, result.value(QStringLiteral("idempotent_replay")).toBool());
    });
    connect(m_runtime, &AgentRuntimeClient::backgroundNotificationsRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_backgroundNotificationRequestId) return;
        m_backgroundNotificationRequestId.clear();
        showBackgroundNotificationPage(result);
    });
    connect(m_runtime, &AgentRuntimeClient::backgroundRecoveryRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_backgroundRecoveryRequestId) return;
        m_backgroundRecoveryRequestId.clear();
        showBackgroundRecoveryPage(result);
    });
    connect(m_runtime, &AgentRuntimeClient::connectionStateChanged,
            this, [this](bool ready, const QString &detail) {
        m_runtimeStatus->setText(
            ready && m_runtimeRecoveryMode ? QStringLiteral("◇ 只读恢复")
            : ready ? QStringLiteral("● 运行时就绪")
                    : QStringLiteral("○ 运行时离线"));
        m_runtimeStatus->setToolTip(detail);
        m_runtimeStatus->setStyleSheet(
            ready && m_runtimeRecoveryMode
                ? QStringLiteral("color:#b54708; font-size:11px; font-weight:600;")
            : ready ? QStringLiteral("color:#067647; font-size:11px; font-weight:600;")
                  : QStringLiteral("color:#b54708; font-size:11px; font-weight:600;"));
        if (!ready) {
            m_turnRunning = false;
            m_turnCancelling = false;
            m_activeTurnSessionId.clear();
            m_activeTurnId.clear();
            m_turnCancelRequestId.clear();
        }
        updateTurnAction();
        updateEditorActions();
        updateTerminalControls();
        if (m_importSessionButton) {
            m_importSessionButton->setEnabled(
                ready && !m_runtimeRecoveryMode && m_portableSessionRequestId.isEmpty());
        }
        if (ready && !m_runtimeRecoveryMode) {
            m_runtime->runRetentionMaintenance();
            requestTerminalList();
            requestSessionList();
            requestProjectList();
        }
        else if (m_terminalPollTimer) m_terminalPollTimer->stop();
        if (!ready && !detail.contains(QStringLiteral("正在连接"))) addNotice(detail, true);
        updateRecoveryUi();
    });
    connect(m_runtime, &AgentRuntimeClient::requestFailed,
            this, [this](const QString &requestId, const QString &method,
                         const QString &, int code) {
        if (method == QStringLiteral("operation/status")) {
            if (requestId.isEmpty() || requestId != m_operationStatusRequestId) return;
            m_operationStatusRequestId.clear();
            showOperationReviewFailure(
                QStringLiteral("无法读取版本化 operation/status，当前会话保持只读门控。"));
            return;
        }
        if (method == QStringLiteral("operation/probe")) {
            if (requestId.isEmpty() || requestId != m_operationProbeRequestId) return;
            m_operationProbeRequestId.clear();
            showOperationReviewFailure(
                QStringLiteral("无法采集 operation/probe 证据，当前会话保持只读门控。"));
            return;
        }
        if (method == QStringLiteral("operation/reconcile")) {
            if (requestId.isEmpty() || requestId != m_operationReconcileRequestId) return;
            m_operationReconcileRequestId.clear();
            showOperationReviewFailure(
                QStringLiteral("无法记录 operation/reconcile 结果，当前会话保持只读门控。"));
            return;
        }
        if (method == QStringLiteral("session/compaction/checkpoint/create")
                || method == QStringLiteral("session/compaction/checkpoint/read")
                || method == QStringLiteral("session/compaction/checkpoint/revise")) {
            if (requestId.isEmpty() || requestId != m_compactionRequestId) return;
            m_compactionRequestId.clear();
            m_compactionOperation.clear();
            m_compactionSessionId.clear();
            addNotice(QStringLiteral("压缩审查操作失败（错误码 %1）；原始会话历史未改变。")
                          .arg(code), true);
            return;
        }
        if (method == QStringLiteral("session/background-notifications")) {
            if (requestId.isEmpty() || requestId != m_backgroundNotificationRequestId) return;
            m_backgroundNotificationRequestId.clear();
            if (m_backgroundNotificationMoreButton) {
                m_backgroundNotificationMoreButton->setEnabled(
                    !m_backgroundNotificationCursor.isEmpty());
            }
            addNotice(QStringLiteral("后台通知记录读取失败（错误码 %1）。").arg(code), true);
            return;
        }
        if (method == QStringLiteral("session/background-recovery")) {
            if (requestId.isEmpty() || requestId != m_backgroundRecoveryRequestId) return;
            m_backgroundRecoveryRequestId.clear();
            if (m_backgroundRecoveryMoreButton) {
                m_backgroundRecoveryMoreButton->setEnabled(
                    !m_backgroundRecoveryCursor.isEmpty());
            }
            addNotice(QStringLiteral("后台恢复状态读取失败（错误码 %1）。").arg(code), true);
            return;
        }
        if (method != QStringLiteral("runtime/restart")) return;
        m_runtimeRestartRequired = true;
        if (m_runtimeRestartButton) m_runtimeRestartButton->setText(QStringLiteral("重试 Codex"));
        m_runtimeStatus->setToolTip(safeRuntimeRestartFailure(code));
        updateRecoveryUi();
    });
    connect(m_runtime, &AgentRuntimeClient::projectsListed,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_projectListRequestId) return;
        m_projectListRequestId.clear();
        populateProjectList(result);
    });
    connect(m_runtime, &AgentRuntimeClient::projectNavigationChanged,
            this, [this](const QString &requestId, const QJsonObject &) {
        if (requestId != m_projectNavigationRequestId) return;
        m_projectNavigationRequestId.clear();
        requestProjectList();
    });
    connect(m_runtime, &AgentRuntimeClient::projectOpened,
            this, [this](const QString &, const QJsonObject &project) {
        m_projectId = project.value(QStringLiteral("id")).toString();
        m_projectRoot = project.value(QStringLiteral("root")).toString();
        m_workspaceRootId = QStringLiteral("root-1");
        m_workspaceRootPath = m_projectRoot;
        m_projectRootsRequestId.clear();
        m_projectRootMutationRequestId.clear();
        m_projectTrustRequestId.clear();
        m_projectTrustReview = {};
        const QString name = project.value(QStringLiteral("name")).toString();
        m_projectLabel->setText(name);
        m_projectLabel->setToolTip(m_projectRoot);
        if (m_retentionSettingsButton) {
            m_retentionSettingsButton->setEnabled(!m_runtimeRecoveryMode);
        }
        requestProjectList();
        m_workSessionId.clear();
        m_pendingTerminalKind.clear();
        m_pendingTerminalName.clear();
        m_activeTerminalId.clear();
        m_terminalAttachRequestId.clear();
        m_terminalListRequestId.clear();
        m_terminalOutputOffset = 0;
        m_terminalGeneration = 0;
        m_terminalRunning = false;
        m_terminalStopping = false;
        if (m_terminalPollTimer) m_terminalPollTimer->stop();
        if (m_terminalPicker) {
            const QSignalBlocker blocker(m_terminalPicker);
            m_terminalPicker->clear();
        }
        if (m_terminalExcerptPreview) m_terminalExcerptPreview->clear();
        if (m_terminalStatus) {
            m_terminalStatus->setText(QStringLiteral("创建 Work 会话后可使用项目终端"));
        }
        m_workspaceEditArtifactRequestId.clear();
        m_workspaceEditId.clear();
        m_workspaceEditReference.clear();
        m_workspaceEditOffset = 0;
        if (m_workspaceEditFiles) m_workspaceEditFiles->clear();
        if (m_workspaceEditDiff) m_workspaceEditDiff->clear();
        if (m_workspaceEditMoreButton) m_workspaceEditMoreButton->setEnabled(false);
        if (m_workspaceEditSummary) {
            m_workspaceEditSummary->setText(QStringLiteral("暂无结构化变更提案"));
        }
        updateTerminalControls();
        m_workspaceWatchTimer->stop();
        m_gitStatusTimer->stop();
        m_workspaceWatchId.clear();
        m_watchedDirectories.clear();
        m_workspaceListRequests.clear();
        m_workspaceReadRequests.clear();
        m_workspaceMetadataRequests.clear();
        m_workspaceMetadataMessages.clear();
        m_gitStatuses.clear();
        m_gitStatusPending = false;
        m_gitOverviewRequestId.clear();
        m_gitLogRequestId.clear();
        m_gitDiffRequestId.clear();
        m_gitDiffRequestedScope.clear();
        m_gitDiffRequestedOid.clear();
        m_gitContextRequestId.clear();
        m_gitContextRequestedKind.clear();
        m_gitContextRequestedScope.clear();
        m_gitContextRequestedOid.clear();
        m_gitContextRequestedLabel.clear();
        m_selectedGitOid.clear();
        m_gitCurrentBranch.clear();
        if (m_gitHistory) m_gitHistory->clear();
        if (m_gitDiffPreview) m_gitDiffPreview->clear();
        if (m_gitSummary) m_gitSummary->setText(QStringLiteral("未检测到仓库"));
        updateGitPinControls();
        if (!m_workspaceSearchId.isEmpty()) {
            m_runtime->cancelWorkspaceSearch(
                m_workspaceSearchId, m_projectId, m_workspaceRootId);
        }
        m_workspaceSearchRequestId.clear();
        m_workspaceSearchId.clear();
        m_workspaceSearchCursor.clear();
        m_workspaceSearchAppending = false;
        m_workspaceSearchStale = false;
        m_workspaceSearchResults->clear();
        m_workspaceSearchStatus->setText(QStringLiteral("输入关键词后搜索当前项目"));
        m_workspaceSearchButton->setEnabled(true);
        m_workspaceSearchCancelButton->setEnabled(false);
        m_workspaceSearchMoreButton->setEnabled(false);
        m_repositoryIndexRequestId.clear();
        m_repositoryIndexCancelRequestId.clear();
        m_repositoryIndexId.clear();
        m_repositoryMapRequestId.clear();
        m_repositoryIndexSummary.clear();
        m_repositoryIndexLoaded = false;
        m_repositoryIndexStale = false;
        if (m_repositoryRefreshButton) m_repositoryRefreshButton->setEnabled(true);
        if (m_repositoryCancelButton) m_repositoryCancelButton->setEnabled(false);
        if (m_workspaceSearchButton) m_workspaceSearchButton->setEnabled(true);
        if (m_workspaceSearchCancelButton) m_workspaceSearchCancelButton->setEnabled(false);
        m_repositorySymbols->clear();
        m_repositoryDependencies->clear();
        m_languageResults->clear();
        m_languageDiagnostics->clear();
        m_diagnosticRawPreview->clear();
        m_diagnosticRawRequestId.clear();
        m_diagnosticRawReference.clear();
        m_languageRawButton->setEnabled(false);
        m_languageServersRequestId.clear();
        m_languageRequestId.clear();
        m_languageResultsStale = false;
        clearContextItems();
        m_pinnedContextItems.clear();
        m_pinnedContextSetIdentity.clear();
        m_pinnedContextListRequestId.clear();
        m_pinnedContextMutationRequestId.clear();
        m_pinnedFileReadRequestId.clear();
        m_pinnedImageImportRequestId.clear();
        m_pinnedImagePreviewRequestId.clear();
        m_pinnedImagePreviewReference.clear();
        m_pendingPinnedSelection = {};
        m_pendingPinnedDiagnostic = {};
        m_pendingPinnedIncludeId.clear();
        m_pendingPinnedDiagnosticRequestId.clear();
        m_terminalExcerptRequestId.clear();
        m_pendingContext = {};
        m_pendingPinnedContextSetIdentity.clear();
        m_pendingPinnedContextIds.clear();
        rebuildContextPanel();
        m_languageStatus->setText(QStringLiteral("LSP · 正在检查本地服务器…"));
        m_repositoryMapPreview->clear();
        m_repositoryStatus->setText(QStringLiteral("进入结构页后建立项目索引"));
        m_repositoryRefreshButton->setEnabled(true);
        m_repositoryCancelButton->setEnabled(false);
        m_directoryStatus.clear();
        m_treeItems.clear();
        m_fileTree->clear();
        resetEditorModel();
        loadEditorViewState();
        m_fileFilter->clear();
        updateContextStrip();
        addNotice(QStringLiteral("项目已绑定：%1").arg(m_projectRoot));
        requestDirectoryListing(QString());
        refreshGitStatus();
        requestLanguageServers();
        requestSessionList();
        requestPinnedContext();
        if (m_pinFileContextAction) {
            m_pinFileContextAction->setEnabled(
                m_pinnedContextAvailable && !m_runtimeRecoveryMode);
        }
        if (m_pinSelectionContextAction) {
            m_pinSelectionContextAction->setEnabled(false);
        }
    });
    connect(m_runtime, &AgentRuntimeClient::projectRelinkRequired,
            this, [this](const QString &, const QJsonObject &project,
                         const QJsonObject &identity) {
        const QString projectId = project.value(QStringLiteral("id")).toString();
        const QString rootId = identity.value(QStringLiteral("root_id"))
            .toString(QStringLiteral("root-1"));
        const QString expectedIdentity = identity.value(QStringLiteral("stored_root_identity"))
            .toString(identity.value(QStringLiteral("root_identity")).toString());
        QString candidate = identity.value(QStringLiteral("candidate_root")).toString();
        if (candidate.isEmpty()) {
            candidate = QFileDialog::getExistingDirectory(
                this, QStringLiteral("为不可用项目选择新目录"), QDir::homePath());
        }
        if (candidate.isEmpty()) {
            addNotice(QStringLiteral("项目仍保持不可用，尚未执行重绑定。"), true);
            return;
        }
        const auto answer = QMessageBox::question(
            this, QStringLiteral("确认重绑定项目"),
            QStringLiteral("已找到项目：%1\n原保存路径：%2\n新目录：%3\n\n只有确认后才会更新项目根绑定。")
                .arg(project.value(QStringLiteral("name")).toString(),
                     project.value(QStringLiteral("root")).toString(),
                     QDir::toNativeSeparators(candidate)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            addNotice(QStringLiteral("已取消项目重绑定。"));
            return;
        }
        m_projectRootMutationRequestId = m_runtime->relinkProject(
            projectId, rootId, candidate, expectedIdentity);
        addNotice(QStringLiteral("正在验证新项目目录并写入重绑定事件…"));
    });
    connect(m_runtime, &AgentRuntimeClient::projectTrustReviewRequired,
            this, [this](const QString &, const QJsonObject &project,
                         const QJsonObject &review) {
        if (!m_projectLabel || project.value(QStringLiteral("id")).toString() != m_projectId) {
            return;
        }
        auto summarize = [](const QJsonArray &values) {
            QStringList paths;
            for (const QJsonValue &value : values) {
                const QString path = value.toObject().value(QStringLiteral("relative_path"))
                                         .toString();
                if (!path.isEmpty() && paths.size() < 4) paths << path;
            }
            if (values.size() > paths.size()) paths << QStringLiteral("…");
            return paths.isEmpty() ? QStringLiteral("无") : paths.join(QStringLiteral(", "));
        };
        const QJsonArray repositories = review.value(QStringLiteral("repositories")).toArray();
        const QJsonArray instructions = review.value(QStringLiteral("instructions")).toArray();
        const QJsonArray hooks = review.value(QStringLiteral("executable_hooks")).toArray();
        const QJsonObject policy = review.value(QStringLiteral("policy_impact")).toObject();
        m_projectTrustReview = review;
        const QString trustState = review.value(QStringLiteral("trust_state"))
                                       .toString(QStringLiteral("unreviewed"));
        const QString stateLabel = trustState == QStringLiteral("acknowledged")
            ? QStringLiteral("已确认")
            : (trustState == QStringLiteral("invalidated")
                   ? QStringLiteral("内容变化，需重新确认")
                   : QStringLiteral("待确认"));
        const QString message = QStringLiteral(
            "项目信任审查：%1（仅发现，不执行） · 根：%2 · 仓库：%3（%4） · 指令：%5（%6） · 可执行 Hook：%7（%8） · Agent：%9 · 写入：%10 · Hook：%11")
            .arg(stateLabel, m_projectRoot,
                 QString::number(repositories.size()), summarize(repositories),
                 QString::number(instructions.size()), summarize(instructions),
                 QString::number(hooks.size()), summarize(hooks),
                 policy.value(QStringLiteral("agent_execution")).toString(),
                 policy.value(QStringLiteral("workspace_write")).toString(),
                 policy.value(QStringLiteral("hooks")).toString());
        addNotice(message);
        m_projectLabel->setToolTip(QStringLiteral("%1\n信任审查：%2")
                                       .arg(m_projectRoot,
                                            review.value(QStringLiteral("review_id")).toString()));
    });
    connect(m_runtime, &AgentRuntimeClient::projectTrustAcknowledged,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_projectTrustRequestId) return;
        m_projectTrustRequestId.clear();
        if (result.value(QStringLiteral("project_id")).toString() != m_projectId) return;
        m_projectTrustReview = result.value(QStringLiteral("review")).toObject();
        addNotice(QStringLiteral(
            "已记录当前项目信任审核；Agent 仍为只读，Hook、网络和可执行项目内容仍未获得权限。"));
        if (m_projectLabel) {
            m_projectLabel->setToolTip(QStringLiteral("%1\n信任审查：已确认\n%2")
                .arg(m_projectRoot,
                     m_projectTrustReview.value(QStringLiteral("review_id")).toString()));
        }
    });
    connect(m_runtime, &AgentRuntimeClient::projectRootsListed,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_projectRootsRequestId) return;
        m_projectRootsRequestId.clear();
        showProjectRootsDialog(result);
    });
    connect(m_runtime, &AgentRuntimeClient::projectRootChanged,
            this, [this](const QString &requestId, const QString &method,
                         const QJsonObject &result) {
        if (requestId != m_projectRootMutationRequestId) return;
        m_projectRootMutationRequestId.clear();
        const int count = result.value(QStringLiteral("roots")).toArray().size();
        addNotice(method == QStringLiteral("project/root-add")
            ? QStringLiteral("项目根已添加；当前记录 %1 个独立范围，尚未自动授权给 Agent。")
                  .arg(count)
            : QStringLiteral("项目根已移除；磁盘内容未修改，当前剩余 %1 个范围。")
                  .arg(count));
    });
    connect(m_runtime, &AgentRuntimeClient::sessionStarted,
            this, [this](const QString &, const QJsonObject &session) {
        const QString id = session.value(QStringLiteral("id")).toString();
        const QString mode = session.value(QStringLiteral("mode")).toString();
        m_archivedSessionIds.remove(id);
        m_recoverySessionIds.remove(id);
        if (mode == QStringLiteral("work")) m_workSessionId = id;
        else {
            m_chatSessionId = id;
            m_chatSessionProjectId = session.value(QStringLiteral("project_id")).toString();
        }
        if (mode == m_mode) requestOperationStatus();
        const QString title = session.value(QStringLiteral("title")).toString(id);
        const QString display = title == id ? id : QStringLiteral("%1 · %2").arg(title, id);
        auto *sessionItem = new QListWidgetItem(mode == QStringLiteral("work")
            ? QStringLiteral("项目任务 · %1").arg(display)
            : QStringLiteral("新对话 · %1").arg(display));
        sessionItem->setData(kSessionIdRole, id);
        sessionItem->setData(kSessionModeRole, mode);
        sessionItem->setData(kSessionProjectRole,
                             session.value(QStringLiteral("project_id")).toString());
        sessionItem->setData(kSessionStatusRole, QStringLiteral("active"));
        sessionItem->setData(kSessionTitleRole, title);
        m_sessionList->insertItem(0, sessionItem);
        m_sessionList->setCurrentRow(0);
        const QJsonObject runtime = session.value(QStringLiteral("runtime")).toObject();
        storeSessionRuntimeBinding(id, runtime);
        storeSessionWorkspaceBinding(
            id, session.value(QStringLiteral("workspace")).toObject());
        const QJsonObject binding = m_sessionRuntimeBindings.value(id);
        const QString provider = runtimeMetadataLabel(
            binding, QStringLiteral("provider"));
        const QString model = runtimeMetadataLabel(binding, QStringLiteral("model"));
        updateContextStrip();
        addNotice(QStringLiteral("已连接 %1 / %2（只读）").arg(provider, model));
        if (mode == QStringLiteral("work")) {
            if (!m_pendingTerminalKind.isEmpty()) {
                const QString kind = m_pendingTerminalKind;
                const QString name = m_pendingTerminalName;
                m_pendingTerminalKind.clear();
                m_pendingTerminalName.clear();
                m_runtime->openUserTerminal(id, kind, name);
            } else {
                requestTerminalList();
            }
        }
        if (!m_pendingPrompt.isEmpty() && mode == m_mode) {
            startPendingTurnIfReady();
        }
        requestSessionList();
    });
    connect(m_runtime, &AgentRuntimeClient::sessionResumed,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_sessionResumeRequestId) return;
        m_sessionResumeRequestId.clear();
        const QJsonObject session = result.value(QStringLiteral("session")).toObject();
        const QString id = session.value(QStringLiteral("id")).toString();
        const QString mode = session.value(QStringLiteral("mode")).toString();
        if (id.isEmpty() || mode.isEmpty()) return;
        setMode(mode);
        if (mode == QStringLiteral("work")) m_workSessionId = id;
        else {
            m_chatSessionId = id;
            m_chatSessionProjectId = session.value(QStringLiteral("project_id")).toString();
        }
        if (mode == m_mode) requestOperationStatus();
        const QJsonObject runtime = result.value(QStringLiteral("runtime")).toObject();
        storeSessionRuntimeBinding(id, runtime);
        storeSessionWorkspaceBinding(
            id, result.value(QStringLiteral("workspace")).toObject());
        resetSessionHistoryPagination();
        const QString readRequest = m_runtime->readSession(id);
        if (!readRequest.isEmpty()) m_sessionReadRequestId = readRequest;
        addNotice(QStringLiteral("会话已恢复，可继续发送新任务。"));
        requestSessionList();
    });
    connect(m_runtime, &AgentRuntimeClient::sessionForked,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_sessionForkRequestId) return;
        m_sessionForkRequestId.clear();
        const QJsonObject session = result.value(QStringLiteral("session")).toObject();
        const QString id = session.value(QStringLiteral("id")).toString();
        const QString mode = session.value(QStringLiteral("mode")).toString();
        if (id.isEmpty() || mode.isEmpty()) return;
        setMode(mode);
        if (mode == QStringLiteral("work")) m_workSessionId = id;
        else {
            m_chatSessionId = id;
            m_chatSessionProjectId = session.value(QStringLiteral("project_id")).toString();
        }
        if (mode == m_mode) requestOperationStatus();
        storeSessionRuntimeBinding(
            id, result.value(QStringLiteral("runtime")).toObject());
        storeSessionWorkspaceBinding(
            id, result.value(QStringLiteral("workspace")).toObject());
        resetSessionHistoryPagination();
        const QString readRequest = m_runtime->readSession(id);
        if (!readRequest.isEmpty()) m_sessionReadRequestId = readRequest;
        addNotice(QStringLiteral("已创建会话分支，历史已复制到最新边界。"));
        requestSessionList();
    });
    connect(m_runtime, &AgentRuntimeClient::sessionsListed,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_sessionListRequestId) return;
        m_sessionListRequestId.clear();
        populateSessionList(result);
        if (std::exchange(m_sessionListRefreshPending, false)) requestSessionList();
    });
    connect(m_runtime, &AgentRuntimeClient::sessionChanged,
            this, [this](const QString &requestId, const QString &method,
                         const QJsonObject &result) {
        if (requestId == m_sessionMutationRequestId) m_sessionMutationRequestId.clear();
        const QString sessionId = result.value(QStringLiteral("session_id")).toString();
        const QString status = result.value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("archived")) m_archivedSessionIds.insert(sessionId);
        else if (status == QStringLiteral("active")) m_archivedSessionIds.remove(sessionId);
        if (method == QStringLiteral("session/title")) {
            addNotice(QStringLiteral("会话标题已更新。"));
        } else if (status == QStringLiteral("archived")) {
            addNotice(QStringLiteral("会话已归档，仍可查看历史。"));
        } else {
            addNotice(QStringLiteral("会话已恢复。"));
        }
        updateTurnAction();
        updateTerminalControls();
        requestSessionList();
    });
    connect(m_runtime, &AgentRuntimeClient::sessionDeletionPreviewed,
            this, [this](const QString &requestId, const QJsonObject &preview) {
        if (requestId != m_sessionDeletionRequestId) return;
        m_sessionDeletionRequestId.clear();
        confirmSessionDeletion(preview);
    });
    connect(m_runtime, &AgentRuntimeClient::sessionDeletionChanged,
            this, [this](const QString &requestId, const QString &method,
                         const QJsonObject &result) {
        if (!m_sessionDeletionRequestId.isEmpty()
                && requestId != m_sessionDeletionRequestId) return;
        if (requestId == m_sessionDeletionRequestId) m_sessionDeletionRequestId.clear();
        const QString sessionId = result.value(QStringLiteral("root_session_id")).toString();
        if (method == QStringLiteral("session/delete/schedule")) {
            if (!sessionId.isEmpty()) m_pendingDeletionSessionIds.insert(sessionId);
            const QDateTime undoUntil = QDateTime::fromMSecsSinceEpoch(
                result.value(QStringLiteral("undo_until_ms")).toVariant().toLongLong());
            addNotice(QStringLiteral("已安排删除，%1 前可从会话菜单撤销。")
                          .arg(undoUntil.toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
        } else {
            if (!sessionId.isEmpty()) m_pendingDeletionSessionIds.remove(sessionId);
            addNotice(QStringLiteral("已撤销会话删除。"));
        }
        updateTurnAction();
        updateTerminalControls();
        if (method == QStringLiteral("session/delete/undo")
                && sessionId == (m_mode == QStringLiteral("work")
                    ? m_workSessionId : m_chatSessionId)) {
            requestOperationStatus();
        }
        requestSessionList();
    });
    connect(m_runtime, &AgentRuntimeClient::portableSessionExportPreviewed,
            this, [this](const QString &requestId, const QJsonObject &preview) {
        if (requestId != m_portableSessionRequestId
                || m_portableSessionOperation != QStringLiteral("export-preview")) return;
        m_portableSessionRequestId.clear();
        confirmPortableSessionExport(preview);
    });
    connect(m_runtime, &AgentRuntimeClient::portableSessionExported,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_portableSessionRequestId
                || m_portableSessionOperation != QStringLiteral("export")) return;
        m_portableSessionRequestId.clear();
        const QJsonObject package = result.value(QStringLiteral("package")).toObject();
        const QByteArray bytes = QJsonDocument(package).toJson(QJsonDocument::Compact);
        if (package.isEmpty() || bytes.isEmpty() || bytes.size() > kMaxPortableSessionBytes) {
            addNotice(QStringLiteral("导出包为空或超过 4 MiB，未写入文件。"), true);
            m_portableSessionOperation.clear();
            m_portableSessionPath.clear();
            return;
        }
        QSaveFile file(m_portableSessionPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
                || !file.commit()) {
            addNotice(QStringLiteral("无法原子写入会话导出文件。"), true);
            m_portableSessionOperation.clear();
            m_portableSessionPath.clear();
            return;
        }
        addNotice(QStringLiteral("会话已安全导出：%1").arg(m_portableSessionPath));
        m_portableSessionPath.clear();
        m_portableSessionOperation.clear();
    });
    connect(m_runtime, &AgentRuntimeClient::portableSessionImportPreviewed,
            this, [this](const QString &requestId, const QJsonObject &preview) {
        if (requestId != m_portableSessionRequestId
                || m_portableSessionOperation != QStringLiteral("import-preview")) return;
        m_portableSessionRequestId.clear();
        confirmPortableSessionImport(preview);
    });
    connect(m_runtime, &AgentRuntimeClient::portableSessionImported,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_portableSessionRequestId
                || m_portableSessionOperation != QStringLiteral("import")) return;
        m_portableSessionRequestId.clear();
        const QJsonObject session = result.value(QStringLiteral("session")).toObject();
        const QString sessionId = session.value(QStringLiteral("id")).toString();
        const QString mode = session.value(QStringLiteral("mode")).toString();
        if (sessionId.isEmpty() || (mode != QStringLiteral("chat")
                && mode != QStringLiteral("work"))) {
            addNotice(QStringLiteral("导入完成，但返回的会话标识无效。"), true);
            return;
        }
        setMode(mode);
        if (mode == QStringLiteral("work")) m_workSessionId = sessionId;
        else {
            m_chatSessionId = sessionId;
            m_chatSessionProjectId = session.value(QStringLiteral("project_id")).toString();
        }
        requestOperationStatus();
        m_portableSessionPackage = {};
        m_portableSessionPath.clear();
        m_portableSessionOperation.clear();
        if (m_importSessionButton) m_importSessionButton->setEnabled(true);
        addNotice(QStringLiteral("会话已导入；本地历史可用，provider continuation 未携带。"));
        requestSessionList();
        if (m_sessionReadRequestId.isEmpty()) {
            resetSessionHistoryPagination();
            m_sessionReadRequestId = m_runtime->readSession(sessionId);
        }
    });
    connect(m_runtime, &AgentRuntimeClient::retentionMaintenanceCompleted,
            this, [this](const QString &, const QJsonObject &result) {
        const QJsonObject retention = result.value(QStringLiteral("retention")).toObject();
        const QJsonObject deletions = result.value(QStringLiteral("deletions")).toObject();
        const qint64 changed = retention.value(QStringLiteral("archived_sessions"))
                .toVariant().toLongLong()
            + retention.value(QStringLiteral("scheduled_deletions"))
                .toVariant().toLongLong()
            + deletions.value(QStringLiteral("purged")).toVariant().toLongLong();
        if (changed > 0) requestSessionList();
    });
    connect(m_runtime, &AgentRuntimeClient::retentionPolicyRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_retentionPolicyRequestId) return;
        m_retentionPolicyRequestId.clear();
        showRetentionPolicyDialog(result);
    });
    connect(m_runtime, &AgentRuntimeClient::retentionPolicyChanged,
            this, [this](const QString &requestId, const QString &method,
                         const QJsonObject &result) {
        if (requestId != m_retentionPolicyRequestId) return;
        m_retentionPolicyRequestId.clear();
        if (method == QStringLiteral("retention/policy/remove")) {
            addNotice(result.value(QStringLiteral("removed")).toBool()
                ? QStringLiteral("已移除自动保留策略。")
                : QStringLiteral("当前范围没有自动保留策略。"));
        } else {
            addNotice(QStringLiteral("保留策略已更新。"));
        }
        m_runtime->runRetentionMaintenance();
    });
    connect(m_runtime, &AgentRuntimeClient::sessionRead,
            this, [this](const QString &requestId, const QJsonObject &snapshot) {
        if (requestId != m_sessionReadRequestId) return;
        const bool appendingHistory = m_sessionHistoryAppending;
        m_sessionReadRequestId.clear();
        m_sessionHistoryAppending = false;
        const QJsonObject session = snapshot.value(QStringLiteral("session")).toObject();
        const QString id = session.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) return;
        if (appendingHistory && id != m_sessionHistoryId) {
            resetSessionHistoryPagination();
            return;
        }
        const QString mode = session.value(QStringLiteral("mode")).toString();
        if (snapshot.value(QStringLiteral("status")).toString() == QStringLiteral("archived")) {
            m_archivedSessionIds.insert(id);
        } else {
            m_archivedSessionIds.remove(id);
        }
        setMode(mode);
        if (mode == QStringLiteral("work")) m_workSessionId = id;
        else {
            m_chatSessionId = id;
            m_chatSessionProjectId = session.value(QStringLiteral("project_id")).toString();
        }
        storeSessionRuntimeBinding(
            id, snapshot.value(QStringLiteral("runtime")).toObject());
        storeSessionWorkspaceBinding(
            id, snapshot.value(QStringLiteral("workspace")).toObject());
        if (!appendingHistory) {
            while (m_timelineLayout && m_timelineLayout->count() > 2) {
                QLayoutItem *item = m_timelineLayout->takeAt(1);
                if (item->widget()) item->widget()->deleteLater();
                delete item;
            }
            m_itemLabels.clear();
            m_itemArtifactButtons.clear();
            m_commandArtifactRequests.clear();
            m_emptyTimeline->show();
            m_sessionHistoryId = id;
        }
        const QJsonArray items = snapshot.value(QStringLiteral("items")).toArray();
        if (appendingHistory) {
            for (int index = items.size() - 1; index >= 0; --index) {
                addTimelineItem(items.at(index).toObject(), true);
            }
            QTimer::singleShot(0, this, [this]() {
                if (!m_timelineScroll) return;
                QScrollBar *bar = m_timelineScroll->verticalScrollBar();
                const int addedHeight = qMax(0, bar->maximum() - m_sessionHistoryScrollMaximum);
                bar->setValue(m_sessionHistoryScrollValue + addedHeight);
            });
        } else {
            for (const QJsonValue &value : items) addTimelineItem(value.toObject());
            addNotice(QStringLiteral("已恢复会话：%1")
                .arg(session.value(QStringLiteral("title")).toString(id)));
        }
        const QJsonObject page = snapshot.value(QStringLiteral("history_page")).toObject();
        const QJsonObject recovery = snapshot.value(QStringLiteral("recovery")).toObject();
        if (recovery.value(QStringLiteral("status")).toString()
                == QStringLiteral("projection-rebuilt")) {
            m_recoverySessionIds.remove(id);
            addNotice(QStringLiteral("已从完整事件记录安全重建会话索引。"));
        }
        const bool hasOlder = page.value(QStringLiteral("has_older")).toBool();
        m_sessionHistoryCursor = hasOlder
            ? page.value(QStringLiteral("older_cursor")).toString() : QString();
        if (m_sessionHistoryMoreButton) {
            m_sessionHistoryMoreButton->setText(QStringLiteral("加载更早记录"));
            m_sessionHistoryMoreButton->setEnabled(!m_sessionHistoryCursor.isEmpty());
            m_sessionHistoryMoreButton->setVisible(!m_sessionHistoryCursor.isEmpty());
        }
        updateContextStrip();
        updateRecoveryUi();
        updateTurnAction();
        updateTerminalControls();
        if (mode == QStringLiteral("work")) requestTerminalList();
    });
    connect(m_runtime, &AgentRuntimeClient::timelineEvent,
            this, [this](const QJsonObject &event) {
        const QString eventName = event.value(QStringLiteral("event")).toString();
        const QString sessionId = event.value(QStringLiteral("session_id")).toString();
        const QString turnId = event.value(QStringLiteral("turn_id")).toString();
        const QJsonObject item = event.value(QStringLiteral("item")).toObject();
        if (!item.isEmpty()) addTimelineItem(item);
        const auto showRuntimeFailure = [this](const QJsonObject &failureItem,
                                                const QString &operation) {
            const QJsonObject data = failureItem.value(QStringLiteral("data")).toObject();
            if (data.value(QStringLiteral("schema_version")).toString()
                    != QStringLiteral("runtime-error/0.1")) {
                return;
            }
            const QString errorClass = data.value(QStringLiteral("class"))
                .toString(QStringLiteral("adapter"));
            const QString classLabel = errorClass == QStringLiteral("transport")
                ? QStringLiteral("传输")
                : errorClass == QStringLiteral("timeout")
                    ? QStringLiteral("超时")
                    : errorClass == QStringLiteral("provider")
                        ? QStringLiteral("模型服务")
                        : (errorClass == QStringLiteral("persistence")
                           || errorClass == QStringLiteral("storage"))
                            ? QStringLiteral("本地存储")
                            : errorClass == QStringLiteral("protocol")
                                ? QStringLiteral("协议")
                                : errorClass == QStringLiteral("sandbox")
                                    ? QStringLiteral("沙箱")
                                    : errorClass == QStringLiteral("policy")
                                        ? QStringLiteral("策略")
                                        : errorClass == QStringLiteral("tool")
                                            ? QStringLiteral("工具")
                                            : errorClass == QStringLiteral("workspace")
                                                ? QStringLiteral("工作区")
                                                : errorClass == QStringLiteral("git")
                                                    ? QStringLiteral("Git")
                                                    : errorClass == QStringLiteral("budget")
                                                        ? QStringLiteral("预算")
                                                        : QStringLiteral("适配器");
            const bool retryable = data.value(QStringLiteral("retryable")).toBool();
            addNotice(QStringLiteral("%1失败 · 类型：%2 · %3")
                          .arg(operation, classLabel,
                               retryable ? QStringLiteral("可以重试")
                                         : QStringLiteral("请检查配置或运行时状态")),
                      true);
        };
        if (eventName == QStringLiteral("diagnostics.observed")) {
            const QJsonObject data = item.value(QStringLiteral("data")).toObject();
            const QString projectId = data.value(QStringLiteral("project_id")).toString();
            if (projectId.isEmpty() || projectId == m_projectId) {
                populateLanguageDiagnostics(data, false);
            }
        }
        if (eventName == QStringLiteral("turn.started")) {
            m_activeTurnSessionId = sessionId;
            m_activeTurnId = turnId;
            m_turnRunning = true;
            m_turnCancelling = false;
            m_turnCancelRequestId.clear();
            updateTurnAction();
        } else if (eventName == QStringLiteral("turn.cancellation-acknowledged")) {
            if (turnId == m_activeTurnId) {
                m_turnCancelling = true;
                updateTurnAction();
            }
        } else if (eventName == QStringLiteral("turn.cancellation-failed")) {
            showRuntimeFailure(item, QStringLiteral("停止请求"));
            if (turnId == m_activeTurnId) {
                m_turnCancelling = false;
                m_turnCancelRequestId.clear();
                updateTurnAction();
            }
        } else if ((eventName == QStringLiteral("turn.completed")
                    || eventName == QStringLiteral("turn.failed")
                    || eventName == QStringLiteral("turn.interrupted"))
                   && (m_activeTurnId.isEmpty() || turnId.isEmpty()
                       || turnId == m_activeTurnId)) {
            if (eventName == QStringLiteral("turn.failed")) {
                showRuntimeFailure(item, QStringLiteral("任务"));
            }
            if (eventName == QStringLiteral("turn.interrupted")) {
                addNotice(QStringLiteral("任务已停止。"));
            } else if (m_turnCancelling) {
                addNotice(QStringLiteral("任务已在取消生效前结束。"));
            }
            m_turnRunning = false;
            m_turnCancelling = false;
            m_activeTurnSessionId.clear();
            m_activeTurnId.clear();
            m_turnCancelRequestId.clear();
            updateTurnAction();
        } else if (eventName == QStringLiteral("turn.steering-failed")) {
            showRuntimeFailure(item, QStringLiteral("引导"));
        }
    });
    connect(m_runtime, &AgentRuntimeClient::turnCancellationRequested,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_turnCancelRequestId
                || result.value(QStringLiteral("turn_id")).toString() != m_activeTurnId) {
            return;
        }
        m_turnCancelling = true;
        updateTurnAction();
    });
    connect(m_runtime, &AgentRuntimeClient::turnContextInspected,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_contextInspectRequestId) return;
        m_contextInspectRequestId.clear();
        updateContextStrip();
        const QString currentSession = m_mode == QStringLiteral("work")
            ? m_workSessionId : m_chatSessionId;
        if (result.value(QStringLiteral("session_id")).toString() != currentSession) return;
        showContextInspection(result);
    });
    connect(m_runtime, &AgentRuntimeClient::pinnedContextListed,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_pinnedContextListRequestId
                || result.value(QStringLiteral("project_id")).toString() != m_projectId) {
            return;
        }
        m_pinnedContextListRequestId.clear();
        applyPinnedContextResult(result);
    });
    connect(m_runtime, &AgentRuntimeClient::pinnedContextChanged,
            this, [this](const QString &requestId, const QString &method,
                         const QJsonObject &result) {
        if (requestId != m_pinnedContextMutationRequestId
                || result.value(QStringLiteral("project_id")).toString() != m_projectId) {
            return;
        }
        m_pinnedContextMutationRequestId.clear();
        applyPinnedContextResult(result);
        updateGitPinControls();
        addNotice(method == QStringLiteral("workspace/pinned-context/remove")
            ? QStringLiteral("已解除固定上下文。")
            : QStringLiteral("固定上下文已保存。"));
    });
    connect(m_runtime, &AgentRuntimeClient::pinnedImageImported,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_pinnedImageImportRequestId) return;
        m_pinnedImageImportRequestId.clear();
        finishPinnedImageImport(result);
    });
    connect(m_runtime, &AgentRuntimeClient::pinnedImageRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_pinnedImagePreviewRequestId) return;
        m_pinnedImagePreviewRequestId.clear();
        showPinnedImagePreview(result);
        m_pinnedImagePreviewReference.clear();
        rebuildContextPanel();
    });
    connect(m_runtime, &AgentRuntimeClient::terminalsListed,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_terminalListRequestId
                || result.value(QStringLiteral("session_id")).toString() != m_workSessionId) {
            return;
        }
        m_terminalListRequestId.clear();
        const QJsonArray terminals = result.value(QStringLiteral("terminals")).toArray();
        QString selectedId = m_activeTerminalId;
        QString preferredId;
        {
            const QSignalBlocker blocker(m_terminalPicker);
            m_terminalPicker->clear();
            for (const QJsonValue &value : terminals) {
                const QJsonObject terminal = value.toObject();
                const QString id = terminal.value(QStringLiteral("terminal_id")).toString();
                const QString name = terminal.value(QStringLiteral("name")).toString();
                const QString kind = terminal.value(QStringLiteral("kind")).toString();
                const QString state = terminal.value(QStringLiteral("state")).toString();
                const QString label = QStringLiteral("%1 · %2 · %3")
                    .arg(name,
                         kind == QStringLiteral("background") ? QStringLiteral("后台")
                                                                : QStringLiteral("前台"),
                         state);
                m_terminalPicker->addItem(label, id);
                if (preferredId.isEmpty() && kind == QStringLiteral("foreground")) {
                    preferredId = id;
                }
            }
            int index = m_terminalPicker->findData(selectedId);
            if (index < 0 && !preferredId.isEmpty()) {
                index = m_terminalPicker->findData(preferredId);
            }
            if (index < 0 && m_terminalPicker->count() > 0) index = 0;
            m_terminalPicker->setCurrentIndex(index);
            selectedId = index >= 0 ? m_terminalPicker->itemData(index).toString() : QString();
        }
        if (selectedId != m_activeTerminalId || m_activeTerminalId.isEmpty()) {
            activateTerminal(selectedId);
        } else {
            updateTerminalControls();
        }
    });
    connect(m_runtime, &AgentRuntimeClient::terminalOpened,
            this, [this](const QString &, const QJsonObject &terminal) {
        m_activeTerminalId = terminal.value(QStringLiteral("terminal_id")).toString();
        applyTerminalSnapshot(terminal, true);
        requestTerminalList();
    });
    connect(m_runtime, &AgentRuntimeClient::terminalAttached,
            this, [this](const QString &requestId, const QJsonObject &terminal) {
        if (requestId != m_terminalAttachRequestId) return;
        m_terminalAttachRequestId.clear();
        applyTerminalSnapshot(terminal);
    });
    connect(m_runtime, &AgentRuntimeClient::terminalExcerptRead,
            this, [this](const QString &requestId, const QJsonObject &excerpt) {
        if (requestId != m_terminalExcerptRequestId) return;
        m_terminalExcerptRequestId.clear();
        finishPinnedTerminalExcerpt(excerpt);
        updateTerminalControls();
    });
    connect(m_runtime, &AgentRuntimeClient::terminalStopped,
            this, [this](const QString &, const QJsonObject &terminal) {
        applyTerminalSnapshot(terminal);
        requestTerminalList();
    });
    connect(m_runtime, &AgentRuntimeClient::terminalRestarted,
            this, [this](const QString &, const QJsonObject &terminal) {
        applyPinnedContextInvalidation(terminal);
        markPinnedTerminalStale(terminal.value(QStringLiteral("terminal_id")).toString());
        m_activeTerminalId = terminal.value(QStringLiteral("terminal_id")).toString();
        applyTerminalSnapshot(terminal, true);
        requestTerminalList();
    });
    connect(m_runtime, &AgentRuntimeClient::terminalRemoved,
            this, [this](const QString &, const QJsonObject &result) {
        applyPinnedContextInvalidation(result);
        markPinnedTerminalStale(result.value(QStringLiteral("terminal_id")).toString());
        if (result.value(QStringLiteral("terminal_id")).toString() == m_activeTerminalId) {
            activateTerminal(QString());
        }
        requestTerminalList();
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceListed,
            this, [this](const QString &requestId, const QJsonObject &listing) {
        m_workspaceListRequests.remove(requestId);
        populateDirectory(listing);
        updateWorkspaceWatch(listing.value(QStringLiteral("path")).toString());
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceFileRead,
            this, [this](const QString &requestId, const QJsonObject &file) {
        if (requestId == m_pinnedFileReadRequestId) {
            m_pinnedFileReadRequestId.clear();
            finishPinnedFileRead(file);
            return;
        }
        const bool restoring = m_editorRestoreRequests.remove(requestId);
        m_workspaceReadRequests.remove(requestId);
        const QString path = file.value(QStringLiteral("path")).toString();
        const qint64 size = file.value(QStringLiteral("size")).toVariant().toLongLong();
        EditorBuffer buffer;
        buffer.content = file.value(QStringLiteral("content")).toString();
        buffer.revision = file.value(QStringLiteral("revision")).toString();
        buffer.encoding = file.value(QStringLiteral("encoding")).toString();
        buffer.newline = file.value(QStringLiteral("newline")).toString();
        buffer.saveSupported = file.value(QStringLiteral("save_supported")).toBool();
        if (m_restoredEditorViews.contains(path)) {
            const EditorViewState view = m_restoredEditorViews.take(path);
            buffer.cursorPosition = view.cursorPosition;
            buffer.anchorPosition = view.anchorPosition;
            buffer.verticalScroll = view.verticalScroll;
            buffer.horizontalScroll = view.horizontalScroll;
        }
        if (path == m_pendingSearchPath) {
            const int offset = editorOffsetForLineColumn(
                buffer.content, m_pendingSearchLine, m_pendingSearchColumn);
            buffer.cursorPosition = offset;
            buffer.anchorPosition = offset;
            m_pendingSearchPath.clear();
        }
        const QString newlineLabel = buffer.newline == QStringLiteral("crlf")
            ? QStringLiteral("CRLF")
            : buffer.newline == QStringLiteral("mixed") ? QStringLiteral("混合换行")
                                                           : QStringLiteral("LF");
        buffer.metadata = QStringLiteral("%1 · %2 · %3 字节%4")
            .arg(buffer.encoding.toUpper(), newlineLabel)
            .arg(size)
            .arg(buffer.saveSupported ? QStringLiteral(" · 用户可编辑")
                                      : QStringLiteral(" · 只读降级"));
        m_editorBuffers.insert(path, buffer);
        if (editorTabIndex(path) < 0) {
            m_editorTabs->addTab(QFileInfo(path).fileName());
            m_editorTabs->setTabData(m_editorTabs->count() - 1, path);
        }
        addRecentFile(path);
        if (!restoring) m_editorLoading = false;
        if (!restoring || path == m_editorRestoreActivePath || m_openEditorPath.isEmpty()) {
            activateEditorBuffer(path);
        }
        if (path == m_editorRestoreActivePath) m_editorRestoreActivePath.clear();
        updateEditorTab(path);
        if (restoring && m_editorRestoreRequests.isEmpty()) restoreEditorGroups();
        else if (!restoring) saveEditorViewState();
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceFileSaved,
            this, [this](const QString &requestId, const QJsonObject &file) {
        if (requestId != m_editorSaveRequestId) return;
        m_editorSaveRequestId.clear();
        applyPinnedContextInvalidation(file);
        m_editorRevision = file.value(QStringLiteral("revision")).toString();
        m_editorConflict = false;
        m_editor->document()->setModified(false);
        storeActiveEditorState();
        m_editorPath->setText(m_openEditorPath);
        m_fileStatus->setText(QStringLiteral("已原子保存 · Agent 仍保持只读"));
        markPinnedContextStale(QSet<QString>{m_openEditorPath});
        if (m_workspaceSearchResults->topLevelItemCount() > 0) {
            m_workspaceSearchStale = true;
            m_workspaceSearchStatus->setText(
                QStringLiteral("工作区已变化 · 当前搜索结果可能已过期"));
        }
        markRepositoryIndexStale();
        markLanguageResultsStale();
        updateEditorActions();
        updateEditorTab(m_openEditorPath);
        saveEditorViewState();
        QString parent = QFileInfo(m_openEditorPath).path();
        if (parent == QStringLiteral(".")) parent.clear();
        requestDirectoryListing(parent);
        m_gitStatusTimer->stop();
        refreshGitStatus();
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceMetadataRead,
            this, [this](const QString &requestId, const QJsonObject &metadata) {
        const QString path = m_workspaceMetadataRequests.take(requestId);
        if (!path.isEmpty()) {
            showEditorFallback(path, metadata,
                               m_workspaceMetadataMessages.take(requestId));
        }
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceWatchConfigured,
            this, [this](const QString &, const QJsonObject &watch) {
        m_workspaceWatchId = watch.value(QStringLiteral("watch_id")).toString();
        const int interval = watch.value(QStringLiteral("poll_interval_ms")).toInt(1500);
        if (!m_workspaceWatchId.isEmpty()) m_workspaceWatchTimer->start(interval);
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceGitStatusRead,
            this, [this](const QString &, const QJsonObject &status) {
        m_gitStatusPending = false;
        m_gitStatuses.clear();
        if (status.value(QStringLiteral("repository")).toBool()) {
            for (const QJsonValue &value : status.value(QStringLiteral("entries")).toArray()) {
                const QJsonObject entry = value.toObject();
                m_gitStatuses.insert(entry.value(QStringLiteral("path")).toString(),
                                     entry.value(QStringLiteral("status")).toString());
            }
        }
        applyGitDecorations();
        if (!m_projectId.isEmpty()) m_gitStatusTimer->start();
    });
    connect(m_runtime, &AgentRuntimeClient::gitOverviewRead,
            this, [this](const QString &requestId, const QJsonObject &overview) {
        if (requestId != m_gitOverviewRequestId) return;
        m_gitOverviewRequestId.clear();
        populateGitOverview(overview);
    });
    connect(m_runtime, &AgentRuntimeClient::gitLogRead,
            this, [this](const QString &requestId, const QJsonObject &log) {
        if (requestId != m_gitLogRequestId) return;
        m_gitLogRequestId.clear();
        populateGitLog(log);
    });
    connect(m_runtime, &AgentRuntimeClient::gitDiffRead,
            this, [this](const QString &requestId, const QJsonObject &diff) {
        if (requestId != m_gitDiffRequestId) return;
        m_gitDiffRequestId.clear();
        const QString requestedScope = m_gitDiffRequestedScope;
        const QString requestedOid = m_gitDiffRequestedOid;
        m_gitDiffRequestedScope.clear();
        m_gitDiffRequestedOid.clear();
        const QString desiredScope = m_gitDiffScope->currentData().toString();
        const QString desiredOid = desiredScope == QStringLiteral("commit")
            ? m_selectedGitOid : QString();
        if (requestedScope == desiredScope && requestedOid == desiredOid) {
            populateGitDiff(diff);
        } else {
            requestGitDiff();
        }
    });
    connect(m_runtime, &AgentRuntimeClient::gitContextRead,
            this, [this](const QString &requestId, const QJsonObject &context) {
        if (requestId != m_gitContextRequestId) return;
        m_gitContextRequestId.clear();
        finishPinnedGitContext(context);
        m_gitContextRequestedKind.clear();
        m_gitContextRequestedScope.clear();
        m_gitContextRequestedOid.clear();
        m_gitContextRequestedLabel.clear();
        updateGitPinControls();
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceSearchCompleted,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_workspaceSearchRequestId) return;
        if (result.value(QStringLiteral("root_id")).toString() != m_workspaceRootId) return;
        m_workspaceSearchRequestId.clear();
        appendWorkspaceSearchResults(result);
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceIndexed,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_repositoryIndexRequestId
                || result.value(QStringLiteral("project_id")).toString() != m_projectId
                || result.value(QStringLiteral("root_id")).toString() != m_workspaceRootId
                || result.value(QStringLiteral("index_id")).toString() != m_repositoryIndexId) {
            return;
        }
        m_repositoryIndexRequestId.clear();
        m_repositoryIndexId.clear();
        m_repositoryCancelButton->setEnabled(false);
        if (result.value(QStringLiteral("cancelled")).toBool()) {
            m_repositoryRefreshButton->setEnabled(true);
            m_repositoryStatus->setText(QStringLiteral("索引已取消 · 保留上一次完整快照"));
            return;
        }
        populateRepositoryIndex(result);
        requestRepositoryMap();
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceIndexCancelled,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_repositoryIndexCancelRequestId) return;
        m_repositoryIndexCancelRequestId.clear();
        if (result.value(QStringLiteral("root_id")).toString() != m_workspaceRootId
                || result.value(QStringLiteral("index_id")).toString() != m_repositoryIndexId) {
            return;
        }
        m_repositoryIndexId.clear();
        m_repositoryCancelButton->setEnabled(false);
        m_repositoryRefreshButton->setEnabled(true);
        m_repositoryStatus->setText(QStringLiteral("索引已取消 · 迟到结果将被忽略"));
    });
    connect(m_runtime, &AgentRuntimeClient::repositoryMapRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_repositoryMapRequestId
                || result.value(QStringLiteral("project_id")).toString() != m_projectId
                || result.value(QStringLiteral("root_id")).toString() != m_workspaceRootId) {
            return;
        }
        m_repositoryMapRequestId.clear();
        m_repositoryMapPreview->setPlainText(result.value(QStringLiteral("text")).toString());
        const int used = result.value(QStringLiteral("estimated_tokens")).toInt();
        const int budget = result.value(QStringLiteral("token_budget")).toInt();
        const QString truncated = result.value(QStringLiteral("truncated")).toBool()
            ? QStringLiteral(" · 已按预算截断") : QString();
        m_repositoryStatus->setText(QStringLiteral("%1 · 地图 %2/%3 tokens%4")
            .arg(m_repositoryIndexSummary).arg(used).arg(budget).arg(truncated));
        m_repositoryRefreshButton->setEnabled(true);
    });
    connect(m_runtime, &AgentRuntimeClient::languageServersRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_languageServersRequestId
                || result.value(QStringLiteral("project_id")).toString() != m_projectId
                || result.value(QStringLiteral("root_id")).toString() != m_workspaceRootId) {
            return;
        }
        m_languageServersRequestId.clear();
        const QJsonArray servers = result.value(QStringLiteral("servers")).toArray();
        QStringList installed;
        QStringList detail;
        for (const QJsonValue &value : servers) {
            const QJsonObject server = value.toObject();
            const QString id = server.value(QStringLiteral("server_id")).toString();
            const bool available = server.value(QStringLiteral("installed")).toBool();
            const bool running = server.value(QStringLiteral("running")).toBool();
            if (available) installed.append(id);
            detail.append(QStringLiteral("%1 · %2")
                .arg(server.value(QStringLiteral("language")).toString(),
                     running ? QStringLiteral("运行中")
                             : available ? QStringLiteral("可用") : QStringLiteral("未安装")));
        }
        m_languageStatus->setText(QStringLiteral("LSP · 已发现 %1/%2%3")
            .arg(installed.size()).arg(servers.size())
            .arg(installed.isEmpty() ? QString() : QStringLiteral(" · %1").arg(installed.join(", "))));
        m_languageStatus->setToolTip(detail.join(QLatin1Char('\n')));
        updateEditorActions();
    });
    connect(m_runtime, &AgentRuntimeClient::languageServerStopped,
            this, [this](const QString &, const QJsonObject &result) {
        const bool stopped = result.value(QStringLiteral("stopped")).toBool();
        m_languageStatus->setText(stopped ? QStringLiteral("LSP · 已停止当前语言服务器")
                                          : QStringLiteral("LSP · 当前语言服务器未运行"));
        m_languageStopButton->setEnabled(false);
        requestLanguageServers();
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceDefinitionsRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_languageRequestId
                || result.value(QStringLiteral("root_id")).toString() != m_workspaceRootId) return;
        m_languageRequestId.clear();
        populateLanguageLocations(result, QStringLiteral("定义"));
        updateEditorActions();
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceReferencesRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_languageRequestId
                || result.value(QStringLiteral("root_id")).toString() != m_workspaceRootId) return;
        m_languageRequestId.clear();
        populateLanguageLocations(result, QStringLiteral("引用"));
        updateEditorActions();
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceDiagnosticsRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId != m_languageRequestId
                || result.value(QStringLiteral("root_id")).toString() != m_workspaceRootId) return;
        m_languageRequestId.clear();
        applyPinnedContextInvalidation(result);
        markPinnedContextStale(QSet<QString>{result.value(QStringLiteral("path")).toString()});
        populateLanguageDiagnostics(result);
        updateEditorActions();
    });
    connect(m_runtime, &AgentRuntimeClient::diagnosticRawRead,
            this, [this](const QString &requestId, const QJsonObject &result) {
        if (requestId == m_pendingPinnedDiagnosticRequestId) {
            if (result.value(QStringLiteral("project_id")).toString() != m_projectId
                    || result.value(QStringLiteral("root_id")).toString() != m_workspaceRootId) {
                m_pendingPinnedDiagnosticRequestId.clear();
                m_pendingPinnedDiagnostic = {};
                addNotice(QStringLiteral("诊断原始记录的项目或 root 身份不匹配。"), true);
                return;
            }
            m_pendingPinnedDiagnosticRequestId.clear();
            finishPinnedDiagnosticRaw(result);
            return;
        }
        if (requestId != m_diagnosticRawRequestId
                || (result.contains(QStringLiteral("root_id"))
                    && result.value(QStringLiteral("root_id")).toString()
                        != m_workspaceRootId)) return;
        m_diagnosticRawRequestId.clear();
        const QByteArray content = result.value(QStringLiteral("content")).toString().toUtf8();
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(content, &error);
        m_diagnosticRawPreview->setPlainText(
            error.error == QJsonParseError::NoError
                ? QString::fromUtf8(document.toJson(QJsonDocument::Indented))
                : QString::fromUtf8(content));
        m_workspaceTabs->setCurrentIndex(m_structureWorkspaceTab);
        m_repositoryViews->setCurrentIndex(m_diagnosticRawView);
        m_languageStatus->setText(QStringLiteral("诊断原始记录 · %1 字节 · SHA-256 %2")
            .arg(result.value(QStringLiteral("size")).toInt())
            .arg(result.value(QStringLiteral("sha256")).toString().left(12)));
        m_languageRawButton->setEnabled(!m_diagnosticRawReference.isEmpty());
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceEditPreviewed,
            this, [this](const QString &, const QJsonObject &preview) {
        if (preview.value(QStringLiteral("project_id")).toString() != m_projectId) return;
        populateWorkspaceEditPreview(preview);
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceEditArtifactRead,
            this, [this](const QString &requestId, const QJsonObject &page) {
        if (requestId != m_workspaceEditArtifactRequestId
                || page.value(QStringLiteral("edit_id")).toString() != m_workspaceEditId
                || page.value(QStringLiteral("reference")).toString()
                    != m_workspaceEditReference) {
            return;
        }
        m_workspaceEditArtifactRequestId.clear();
        const QByteArray bytes = QByteArray::fromBase64(
            page.value(QStringLiteral("data_base64")).toString().toLatin1());
        QTextCursor cursor = m_workspaceEditDiff->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(QString::fromUtf8(bytes));
        const QJsonValue nextOffset = page.value(QStringLiteral("next_offset"));
        m_workspaceEditOffset = nextOffset.isNull()
            ? page.value(QStringLiteral("total_bytes")).toVariant().toLongLong()
            : nextOffset.toVariant().toLongLong();
        m_workspaceEditMoreButton->setEnabled(!nextOffset.isNull());
    });
    connect(m_runtime, &AgentRuntimeClient::commandArtifactRead,
            this, [this](const QString &requestId, const QJsonObject &artifact) {
        const QString itemId = m_commandArtifactRequests.take(requestId);
        if (QPushButton *button = m_itemArtifactButtons.value(itemId, nullptr)) {
            button->setEnabled(true);
        }
        auto *dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("命令输出 Artifact"));
        dialog->resize(760, 520);
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(8);
        auto *metadata = new QLabel(dialog);
        metadata->setTextFormat(Qt::PlainText);
        metadata->setText(QStringLiteral(
            "%1 字节 · 保留 %2 · 省略 %3 · SHA-256 %4")
            .arg(artifact.value(QStringLiteral("total_bytes")).toVariant().toULongLong())
            .arg(artifact.value(QStringLiteral("retained_bytes")).toInt())
            .arg(artifact.value(QStringLiteral("omitted_bytes")).toVariant().toULongLong())
            .arg(artifact.value(QStringLiteral("sha256")).toString().left(16)));
        metadata->setStyleSheet(QStringLiteral("color:#667085; font-size:10px;"));
        layout->addWidget(metadata);
        auto *content = new QPlainTextEdit(dialog);
        content->setObjectName(QStringLiteral("commandArtifactPreview"));
        content->setReadOnly(true);
        content->setLineWrapMode(QPlainTextEdit::NoWrap);
        content->setPlainText(artifact.value(QStringLiteral("content")).toString());
        content->setStyleSheet(QStringLiteral(
            "QPlainTextEdit { background:#101828; color:#d0d5dd; border:none;"
            "padding:10px; font-family:Menlo,Consolas,monospace; font-size:10px; }"));
        layout->addWidget(content, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        auto *pin = buttons->addButton(
            QStringLiteral("固定完整输出"), QDialogButtonBox::ActionRole);
        pin->setObjectName(QStringLiteral("commandArtifactPinButton"));
        pin->setIcon(QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")));
        QString pinReason;
        pin->setEnabled(canPinCommandArtifact(artifact, &pinReason));
        pin->setToolTip(pinReason.isEmpty()
            ? QStringLiteral("将完整命令输出固定到当前 Work 会话；不会自动发送")
            : pinReason);
        connect(pin, &QPushButton::clicked, dialog, [this, artifact, pin]() {
            pin->setEnabled(false);
            pin->setText(QStringLiteral("正在固定"));
            pinCommandArtifact(artifact);
        });
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog->show();
    });
    connect(m_runtime, &AgentRuntimeClient::workspaceChanged,
            this, [this](const QString &, const QJsonObject &result) {
        applyPinnedContextInvalidation(result);
        const QJsonArray changes = result.value(QStringLiteral("changes")).toArray();
        QSet<QString> directories;
        QSet<QString> changedPaths;
        QSet<QString> changedEditorPaths;
        bool watchSetChanged = false;
        for (const QJsonValue &value : changes) {
            const QJsonObject change = value.toObject();
            QString parent = change.value(QStringLiteral("parent")).toString();
            const QString path = change.value(QStringLiteral("path")).toString();
            if (!path.isEmpty()) changedPaths.insert(path);
            if (change.value(QStringLiteral("kind")).toString() == QStringLiteral("unavailable")) {
                markDirectoryUnavailable(path, QStringLiteral("目录已移动、删除或不可访问"));
                watchSetChanged = m_watchedDirectories.remove(path) || watchSetChanged;
                parent = QFileInfo(path).path();
                if (parent == QStringLiteral(".")) parent.clear();
            }
            directories.insert(parent);
            if (m_editorBuffers.contains(path)) changedEditorPaths.insert(path);
        }
        for (const QString &directory : directories) {
            if (m_watchedDirectories.contains(directory)) {
                requestDirectoryListing(directory);
            }
        }
        if (!changes.isEmpty()) {
            markPinnedContextStale(changedPaths);
            m_gitStatusTimer->stop();
            refreshGitStatus();
            if (m_workspaceSearchResults->topLevelItemCount() > 0) {
                m_workspaceSearchStale = true;
                m_workspaceSearchStatus->setText(
                    QStringLiteral("工作区已变化 · 当前搜索结果可能已过期"));
            }
            markRepositoryIndexStale();
            markLanguageResultsStale();
        }
        for (const QString &path : changedEditorPaths) {
            EditorBuffer &buffer = m_editorBuffers[path];
            buffer.conflict = true;
            if (path == m_openEditorPath) {
                m_editorConflict = true;
                m_editorPath->setText(QStringLiteral("%1 · 外部已修改").arg(path));
                m_fileStatus->setText(m_editor->document()->isModified()
                    ? QStringLiteral("保存已阻止：本地编辑与外部修改发生冲突")
                    : QStringLiteral("文件已在工作区变化，请重新载入后继续编辑"));
                updateEditorActions();
            }
            updateEditorTab(path);
        }
        if (watchSetChanged) {
            QStringList paths = m_watchedDirectories.values();
            paths.sort(Qt::CaseInsensitive);
            m_runtime->watchWorkspace(m_projectId, paths, m_workspaceWatchId,
                                      m_workspaceRootId);
        } else if (!m_workspaceWatchId.isEmpty()) {
            m_workspaceWatchTimer->start();
        }
    });
    connect(m_runtime, &AgentRuntimeClient::requestFailed,
            this, [this](const QString &requestId, const QString &method,
                         const QString &message, int code) {
        m_pendingPrompt.clear();
        m_pendingContext = {};
        m_pendingPinnedContextSetIdentity.clear();
        m_pendingPinnedContextIds.clear();
        updateTurnAction();
        if (method == QStringLiteral("turn/cancel")
                && requestId == m_turnCancelRequestId) {
            m_turnCancelRequestId.clear();
            m_turnCancelling = false;
            updateTurnAction();
        } else if (method == QStringLiteral("turn/context/inspect")
                   && requestId == m_contextInspectRequestId) {
            m_contextInspectRequestId.clear();
            updateContextStrip();
            addNotice(QStringLiteral("上下文预检失败：%1").arg(message), true);
        } else if (method == QStringLiteral("workspace/pinned-context/list")
                   && requestId == m_pinnedContextListRequestId) {
            m_pinnedContextListRequestId.clear();
            addNotice(QStringLiteral("读取固定上下文失败：%1").arg(message), true);
        } else if ((method == QStringLiteral("workspace/pinned-context/save")
                    || method == QStringLiteral("workspace/pinned-context/remove"))
                   && requestId == m_pinnedContextMutationRequestId) {
            m_pinnedContextMutationRequestId.clear();
            m_pendingPinnedIncludeId.clear();
            addNotice(QStringLiteral("更新固定上下文失败：%1").arg(message), true);
            if (code == -32041) requestPinnedContext();
            rebuildContextPanel();
            updateGitPinControls();
        } else if (method == QStringLiteral("workspace/read")
                   && requestId == m_pinnedFileReadRequestId) {
            m_pinnedFileReadRequestId.clear();
            m_pendingPinnedSelection = {};
            addNotice(QStringLiteral("读取待固定文件失败：%1").arg(message), true);
        } else if (method == QStringLiteral("workspace/image/import-user")
                   && requestId == m_pinnedImageImportRequestId) {
            m_pinnedImageImportRequestId.clear();
            addNotice(QStringLiteral("导入固定图片失败：%1").arg(message), true);
        } else if (method == QStringLiteral("workspace/image/read")
                   && requestId == m_pinnedImagePreviewRequestId) {
            m_pinnedImagePreviewRequestId.clear();
            m_pinnedImagePreviewReference.clear();
            addNotice(QStringLiteral("读取固定图片预览失败：%1").arg(message), true);
            rebuildContextPanel();
        } else if (method == QStringLiteral("runtime/degradations")) {
            m_runtimeDegradationsAvailable = false;
            m_runtimeDegradationStates.clear();
            updateRuntimeCapabilityUi();
        } else if (method == QStringLiteral("project/list")
                   && requestId == m_projectListRequestId) {
            m_projectListRequestId.clear();
            addNotice(QStringLiteral("读取项目导航失败：%1").arg(message), true);
        } else if (method == QStringLiteral("project/navigation")
                   && requestId == m_projectNavigationRequestId) {
            m_projectNavigationRequestId.clear();
            addNotice(QStringLiteral("更新项目导航失败：%1").arg(message), true);
        } else if (method == QStringLiteral("project/trust-acknowledge")
                   && requestId == m_projectTrustRequestId) {
            m_projectTrustRequestId.clear();
            addNotice(QStringLiteral("确认项目信任审核失败：%1").arg(message), true);
        } else if (method == QStringLiteral("project/root-list")
                   && requestId == m_projectRootsRequestId) {
            m_projectRootsRequestId.clear();
            addNotice(QStringLiteral("读取项目根失败：%1").arg(message), true);
        } else if (method == QStringLiteral("session/resume")
                   && (requestId.isEmpty() || requestId == m_sessionResumeRequestId)) {
            m_sessionResumeRequestId.clear();
            addNotice(QStringLiteral("恢复会话失败：%1")
                          .arg(safeProviderLifecycleFailure(method, message, code)), true);
        } else if (method == QStringLiteral("session/fork")
                   && (requestId.isEmpty() || requestId == m_sessionForkRequestId)) {
            m_sessionForkRequestId.clear();
            addNotice(QStringLiteral("创建会话分支失败：%1")
                          .arg(safeProviderLifecycleFailure(method, message, code)), true);
        } else if ((method == QStringLiteral("project/root-add")
                    || method == QStringLiteral("project/root-remove"))
                   && requestId == m_projectRootMutationRequestId) {
            m_projectRootMutationRequestId.clear();
            addNotice(QStringLiteral("项目根操作失败：%1").arg(message), true);
        } else if (method == QStringLiteral("project/relink")
                   && requestId == m_projectRootMutationRequestId) {
            m_projectRootMutationRequestId.clear();
            addNotice(QStringLiteral("项目重绑定失败：%1").arg(message), true);
        } else if ((method == QStringLiteral("session/title")
                    || method == QStringLiteral("session/archive")
                    || method == QStringLiteral("session/unarchive"))
                   && (requestId.isEmpty() || requestId == m_sessionMutationRequestId)) {
            m_sessionMutationRequestId.clear();
            addNotice(QStringLiteral("会话操作失败：%1")
                          .arg(safeProviderLifecycleFailure(method, message, code)), true);
        } else if ((method == QStringLiteral("session/delete/preview")
                    || method == QStringLiteral("session/delete/schedule")
                    || method == QStringLiteral("session/delete/undo"))
                   && requestId == m_sessionDeletionRequestId) {
            m_sessionDeletionRequestId.clear();
            addNotice(QStringLiteral("会话删除操作失败：%1").arg(message), true);
        } else if (method.startsWith(QStringLiteral("session/export"))
                   || method.startsWith(QStringLiteral("session/import"))) {
            if (requestId != m_portableSessionRequestId) return;
            m_portableSessionRequestId.clear();
            m_portableSessionOperation.clear();
            m_portableSessionPackage = {};
            m_portableSessionPath.clear();
            if (m_importSessionButton) m_importSessionButton->setEnabled(true);
            addNotice(QStringLiteral("会话导入/导出失败（%1）：%2").arg(code).arg(message), true);
        } else if ((method == QStringLiteral("retention/policy/read")
                    || method == QStringLiteral("retention/policy/set")
                    || method == QStringLiteral("retention/policy/remove"))
                   && requestId == m_retentionPolicyRequestId) {
            m_retentionPolicyRequestId.clear();
            addNotice(QStringLiteral("保留策略操作失败：%1").arg(message), true);
        } else if ((method == QStringLiteral("session/list")
                    || method == QStringLiteral("session/search"))
                   && requestId == m_sessionListRequestId) {
            m_sessionListRequestId.clear();
            m_sessionListRefreshPending = false;
            if (m_sessionList) {
                m_sessionList->clear();
                m_sessionList->addItem(QStringLiteral("会话列表暂不可用"));
            }
            addNotice(QStringLiteral("读取会话列表失败：%1").arg(message), true);
        } else if (method == QStringLiteral("session/read")
                   && requestId == m_sessionReadRequestId) {
            m_sessionReadRequestId.clear();
            const bool appendingHistory = std::exchange(m_sessionHistoryAppending, false);
            if (m_sessionHistoryMoreButton) {
                m_sessionHistoryMoreButton->setText(QStringLiteral("加载更早记录"));
                m_sessionHistoryMoreButton->setEnabled(!m_sessionHistoryCursor.isEmpty());
                m_sessionHistoryMoreButton->setVisible(!m_sessionHistoryCursor.isEmpty());
            }
            addNotice(appendingHistory
                ? QStringLiteral("加载更早记录失败：%1").arg(message)
                : QStringLiteral("恢复会话失败：%1").arg(message), true);
        } else if (method == QStringLiteral("session/start")
                && !m_pendingTerminalKind.isEmpty()) {
            m_pendingTerminalKind.clear();
            m_pendingTerminalName.clear();
            m_terminalStatus->setText(QStringLiteral("Work 会话创建失败：%1").arg(message));
        } else if (method.startsWith(QStringLiteral("terminal/"))) {
            if (requestId == m_terminalAttachRequestId) m_terminalAttachRequestId.clear();
            if (requestId == m_terminalListRequestId) m_terminalListRequestId.clear();
            if (requestId == m_terminalExcerptRequestId) {
                m_terminalExcerptRequestId.clear();
                addNotice(QStringLiteral("固定终端摘录失败：%1").arg(message), true);
            }
            m_terminalStatus->setText(QStringLiteral("终端操作失败：%1").arg(message));
            if (method == QStringLiteral("terminal/restart-user")) requestTerminalList();
            updateTerminalControls();
        } else if (method == QStringLiteral("artifact/read-command-output")) {
            const QString itemId = m_commandArtifactRequests.take(requestId);
            if (QPushButton *button = m_itemArtifactButtons.value(itemId, nullptr)) {
                button->setEnabled(true);
            }
        } else if (method == QStringLiteral("workspace/edit/preview")) {
            m_workspaceEditSummary->setText(
                QStringLiteral("变更预览失败：%1").arg(message));
            m_workspaceEditSummary->setStyleSheet(QStringLiteral(
                "color:#B42318; font-size:11px; font-weight:600;"));
        } else if (method == QStringLiteral("workspace/edit/artifact/read")) {
            if (requestId == m_workspaceEditArtifactRequestId) {
                m_workspaceEditArtifactRequestId.clear();
                m_workspaceEditMoreButton->setEnabled(true);
            }
        } else if (method == QStringLiteral("workspace/list")) {
            markDirectoryUnavailable(m_workspaceListRequests.take(requestId), message);
        } else if (method == QStringLiteral("workspace/read")) {
            const bool restoring = m_editorRestoreRequests.remove(requestId);
            const QString path = m_workspaceReadRequests.take(requestId);
            if (restoring) {
                if (path == m_editorRestoreActivePath) m_editorRestoreActivePath.clear();
                if (m_editorRestoreRequests.isEmpty()) restoreEditorGroups();
                return;
            }
            if (!path.isEmpty() && (code == -32033 || code == -32034)) {
                const QString metadataRequest = m_runtime->workspaceMetadata(
                    m_projectId, path, m_workspaceRootId);
                if (!metadataRequest.isEmpty()) {
                    m_workspaceMetadataRequests.insert(metadataRequest, path);
                    m_workspaceMetadataMessages.insert(metadataRequest, message);
                } else {
                    m_editorLoading = false;
                }
            } else if (!path.isEmpty()) {
                m_editorLoading = false;
                showEditorFallback(path, {}, message);
            }
        } else if (method == QStringLiteral("workspace/metadata")) {
            const QString path = m_workspaceMetadataRequests.take(requestId);
            m_workspaceMetadataMessages.remove(requestId);
            if (!path.isEmpty()) showEditorFallback(path, {}, message);
        } else if (method == QStringLiteral("workspace/save-user-text")) {
            if (requestId == m_editorSaveRequestId) m_editorSaveRequestId.clear();
            if (code == -32042) {
                m_editorConflict = true;
                m_editorPath->setText(QStringLiteral("%1 · 保存冲突").arg(m_openEditorPath));
                m_fileStatus->setText(QStringLiteral("文件已被外部修改，重新载入后才能保存"));
            } else {
                m_fileStatus->setText(QStringLiteral("保存失败：%1").arg(message));
            }
            updateEditorActions();
        } else if (method == QStringLiteral("workspace/git-status")) {
            m_gitStatusPending = false;
            if (!m_projectId.isEmpty()) m_gitStatusTimer->start();
        } else if (method == QStringLiteral("workspace/git/context/read")
                   && requestId == m_gitContextRequestId) {
            m_gitContextRequestId.clear();
            m_gitContextRequestedKind.clear();
            m_gitContextRequestedScope.clear();
            m_gitContextRequestedOid.clear();
            m_gitContextRequestedLabel.clear();
            addNotice(QStringLiteral("读取待固定 Git 上下文失败：%1").arg(message), true);
            updateGitPinControls();
        } else if (method.startsWith(QStringLiteral("workspace/git/"))) {
            if (requestId == m_gitOverviewRequestId) m_gitOverviewRequestId.clear();
            if (requestId == m_gitLogRequestId) m_gitLogRequestId.clear();
            if (requestId == m_gitDiffRequestId) {
                m_gitDiffRequestId.clear();
                m_gitDiffRequestedScope.clear();
                m_gitDiffRequestedOid.clear();
            }
            if (m_gitSummary) m_gitSummary->setText(QStringLiteral("Git 查询失败"));
            m_gitCurrentBranch.clear();
            updateContextStrip();
            updateGitPinControls();
        } else if (method == QStringLiteral("workspace/search")
                   && requestId == m_workspaceSearchRequestId) {
            m_workspaceSearchRequestId.clear();
            m_workspaceSearchButton->setEnabled(true);
            m_workspaceSearchCancelButton->setEnabled(false);
            m_workspaceSearchMoreButton->setEnabled(false);
            m_workspaceSearchStatus->setText(QStringLiteral("搜索失败：%1").arg(message));
        } else if (method == QStringLiteral("workspace/index")
                   && requestId == m_repositoryIndexRequestId) {
            m_repositoryIndexRequestId.clear();
            m_repositoryIndexId.clear();
            m_repositoryRefreshButton->setEnabled(true);
            m_repositoryCancelButton->setEnabled(false);
            m_repositoryStatus->setText(QStringLiteral("索引失败：%1").arg(message));
        } else if (method == QStringLiteral("workspace/index/cancel")
                   && requestId == m_repositoryIndexCancelRequestId) {
            m_repositoryIndexCancelRequestId.clear();
            m_repositoryCancelButton->setEnabled(false);
            m_repositoryRefreshButton->setEnabled(true);
            m_repositoryStatus->setText(QStringLiteral("取消索引失败：%1").arg(message));
        } else if (method == QStringLiteral("workspace/repository-map")
                   && requestId == m_repositoryMapRequestId) {
            m_repositoryMapRequestId.clear();
            m_repositoryRefreshButton->setEnabled(true);
            m_repositoryStatus->setText(QStringLiteral("仓库地图失败：%1").arg(message));
        } else if (method == QStringLiteral("workspace/language-servers")
                   && requestId == m_languageServersRequestId) {
            m_languageServersRequestId.clear();
            m_languageStatus->setText(QStringLiteral("LSP 状态读取失败：%1").arg(message));
        } else if ((method == QStringLiteral("workspace/definition")
                    || method == QStringLiteral("workspace/references")
                    || method == QStringLiteral("workspace/diagnostics"))
                   && requestId == m_languageRequestId) {
            m_languageRequestId.clear();
            m_languageStatus->setText(QStringLiteral("LSP 请求失败：%1").arg(message));
            updateEditorActions();
        } else if (method == QStringLiteral("workspace/language-server/stop")) {
            m_languageStatus->setText(QStringLiteral("停止 LSP 失败：%1").arg(message));
        } else if (method == QStringLiteral("workspace/diagnostics/raw")
                   && requestId == m_pendingPinnedDiagnosticRequestId) {
            m_pendingPinnedDiagnosticRequestId.clear();
            m_pendingPinnedDiagnostic = {};
            addNotice(QStringLiteral("读取待固定诊断原始记录失败：%1").arg(message), true);
        } else if (method == QStringLiteral("workspace/diagnostics/raw")
                   && requestId == m_diagnosticRawRequestId) {
            m_diagnosticRawRequestId.clear();
            m_languageRawButton->setEnabled(!m_diagnosticRawReference.isEmpty());
            m_languageStatus->setText(QStringLiteral("读取诊断原始记录失败：%1").arg(message));
        }
        if (method.startsWith(QStringLiteral("workspace/"))
                && method != QStringLiteral("workspace/git-status")
                && method != QStringLiteral("workspace/search")
                && method != QStringLiteral("workspace/search/cancel")
                && method != QStringLiteral("workspace/index")
                && method != QStringLiteral("workspace/index/cancel")
                && method != QStringLiteral("workspace/repository-map")
                && !method.startsWith(QStringLiteral("workspace/language-server"))
                && method != QStringLiteral("workspace/language-servers")
                && method != QStringLiteral("workspace/definition")
                && method != QStringLiteral("workspace/references")
                && method != QStringLiteral("workspace/diagnostics")
                && method != QStringLiteral("workspace/observed-diagnostics")
                && method != QStringLiteral("workspace/diagnostics/raw")
                && method != QStringLiteral("workspace/save-user-text")) {
            m_fileStatus->setText(QStringLiteral("无法打开：%1").arg(message));
        }
        if (method != QStringLiteral("workspace/watch/poll")
                && method != QStringLiteral("workspace/git-status")
                && method != QStringLiteral("workspace/read")
                && method != QStringLiteral("workspace/metadata")
                && method != QStringLiteral("workspace/index")
                && method != QStringLiteral("workspace/index/cancel")
                && method != QStringLiteral("workspace/repository-map")
                && !method.startsWith(QStringLiteral("workspace/language-server"))
                && method != QStringLiteral("workspace/language-servers")
                && method != QStringLiteral("workspace/definition")
                && method != QStringLiteral("workspace/references")
                && method != QStringLiteral("workspace/diagnostics")
                && method != QStringLiteral("workspace/observed-diagnostics")
                && method != QStringLiteral("workspace/diagnostics/raw")
                && method != QStringLiteral("workspace/save-user-text")
                && method != QStringLiteral("runtime/degradations")) {
            const bool providerLifecycleMethod = method == QStringLiteral("session/resume")
                || method == QStringLiteral("session/fork")
                || method == QStringLiteral("session/archive")
                || method == QStringLiteral("session/unarchive");
            addNotice(method == QStringLiteral("runtime/restart")
                          ? safeRuntimeRestartFailure(code)
                          : providerLifecycleMethod
                              ? safeProviderLifecycleFailure(method, message, code)
                              : message,
                      true);
        }
    });
    connect(m_runtime, &AgentRuntimeClient::diagnosticMessage,
            this, [this](const QString &message) { m_runtimeStatus->setToolTip(message); });

    QTimer::singleShot(0, m_runtime, &AgentRuntimeClient::start);
}

AgentWorkbenchWidget::~AgentWorkbenchWidget()
{
    storeActiveEditorState();
    saveEditorViewState();
#ifdef AEGISY_HAS_MONACO
    delete m_terminalPage;
    m_terminalPage = nullptr;
    delete m_terminalProfile;
    m_terminalProfile = nullptr;
    delete m_monacoPage;
    m_monacoPage = nullptr;
    delete m_monacoProfile;
    m_monacoProfile = nullptr;
#endif
    if (!m_runtime) return;
    QObject::disconnect(m_runtime, nullptr, this, nullptr);
    m_runtime->stop();
}

void AgentWorkbenchWidget::updateRuntimeCapabilityUi()
{
    if (!m_runtimeCapabilityStatus) return;
    if (!m_runtimeDegradationsAvailable) {
        m_runtimeCapabilityStatus->setText(QStringLiteral("能力未知"));
        m_runtimeCapabilityStatus->setToolTip(
            QStringLiteral("未收到版本化 runtime/degradations 声明；依赖能力保持只读门控"));
        m_runtimeCapabilityStatus->setStyleSheet(
            QStringLiteral("border:none; color:#b54708; font-size:10px; font-weight:600;"));
        return;
    }
    const QString providerState = m_runtimeDegradationStates.value(
        QStringLiteral("codex-provider"));
    if (providerState == QStringLiteral("unavailable")) {
        m_runtimeCapabilityStatus->setText(QStringLiteral("Provider 不可用"));
        m_runtimeCapabilityStatus->setToolTip(
            QStringLiteral("当前运行时未提供 Codex provider 能力；不会模拟可用状态"));
        m_runtimeCapabilityStatus->setStyleSheet(
            QStringLiteral("border:none; color:#b42318; font-size:10px; font-weight:600;"));
        return;
    }
    const bool readOnly = m_runtimeDegradationStates.value(
        QStringLiteral("agent-mutation")) == QStringLiteral("disabled");
    const bool compactBlocked = m_runtimeDegradationStates.value(
        QStringLiteral("provider-thread-compact")) == QStringLiteral("blocked");
    const bool deleteBlocked = m_runtimeDegradationStates.value(
        QStringLiteral("provider-thread-delete")) == QStringLiteral("blocked");
    QStringList summary;
    if (readOnly) summary.append(QStringLiteral("Agent 只读"));
    if (compactBlocked) summary.append(QStringLiteral("Compact 不可用"));
    if (deleteBlocked) summary.append(QStringLiteral("删除不可用"));
    if (summary.isEmpty()) summary.append(QStringLiteral("能力已协商"));
    m_runtimeCapabilityStatus->setText(summary.join(QStringLiteral(" · ")));
    m_runtimeCapabilityStatus->setToolTip(
        QStringLiteral("运行时能力声明已加载；不可用功能不会显示为成功"));
    m_runtimeCapabilityStatus->setStyleSheet(
        QStringLiteral("border:none; color:#475467; font-size:10px; font-weight:600;"));
}

bool AgentWorkbenchWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_workspaceTabs && event->type() == QEvent::Resize) {
        updateResponsiveEditorChrome();
    }
    return QWidget::eventFilter(watched, event);
}

void AgentWorkbenchWidget::buildUi()
{
    setObjectName(QStringLiteral("agentWorkbench"));
    setStyleSheet(QStringLiteral("QWidget#agentWorkbench { background:#f8fafc; }"));
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("agentToolbar"));
    toolbar->setFixedHeight(54);
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget#agentToolbar { background:#ffffff; border-bottom:1px solid #e4e7ec; }"));
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(16, 8, 16, 8);
    toolbarLayout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("Aegisy Coding"), toolbar);
    title->setStyleSheet(QStringLiteral("border:none; color:#101828; font-size:14px; font-weight:700;"));
    toolbarLayout->addWidget(title);

    auto *modeFrame = new QFrame(toolbar);
    modeFrame->setStyleSheet(QStringLiteral("QFrame { background:#f2f4f7; border:none; border-radius:7px; }"));
    auto *modeLayout = new QHBoxLayout(modeFrame);
    modeLayout->setContentsMargins(3, 3, 3, 3);
    modeLayout->setSpacing(2);
    m_modeGroup = new QButtonGroup(this);
    m_modeGroup->setExclusive(true);
    for (const auto &entry : {qMakePair(QStringLiteral("Chat"), QStringLiteral("chat")),
                              qMakePair(QStringLiteral("Work"), QStringLiteral("work"))}) {
        auto *button = new QPushButton(entry.first, modeFrame);
        button->setProperty("mode", entry.second);
        button->setCheckable(true);
        button->setChecked(entry.second == m_mode);
        button->setFixedSize(70, 30);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background:transparent; border:none; border-radius:5px;"
            "color:#667085; font-size:12px; }"
            "QPushButton:hover { color:#101828; }"
            "QPushButton:checked { background:#ffffff; color:#165DFF; font-weight:700; }"));
        m_modeGroup->addButton(button);
        modeLayout->addWidget(button);
    }
    connect(m_modeGroup, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
            this, [this](QAbstractButton *button) { setMode(button->property("mode").toString()); });
    toolbarLayout->addWidget(modeFrame);

    m_projectLabel = new QLabel(QStringLiteral("未打开项目"), toolbar);
    m_projectLabel->setStyleSheet(QStringLiteral("border:none; color:#667085; font-size:11px;"));
    toolbarLayout->addWidget(m_projectLabel);
    toolbarLayout->addStretch();

    m_modelPicker = new QComboBox(toolbar);
    m_modelPicker->addItem(QStringLiteral("Aegisy / Codex Auto"));
    m_modelPicker->setToolTip(QStringLiteral("将在首次创建会话后显示实际模型"));
    m_modelPicker->setMinimumWidth(166);
    m_modelPicker->setStyleSheet(QStringLiteral(
        "QComboBox { background:#ffffff; color:#344054; border:1px solid #d0d5dd;"
        "border-radius:6px; padding:5px 28px 5px 9px; font-size:11px; }"
        "QComboBox::drop-down { subcontrol-origin:padding; subcontrol-position:top right;"
        "width:24px; background:#f9fafb; border:none; border-left:1px solid #e4e7ec;"
        "border-top-right-radius:6px; border-bottom-right-radius:6px; }"));
    toolbarLayout->addWidget(m_modelPicker);
    m_runtimeStatus = new QLabel(QStringLiteral("○ 正在连接"), toolbar);
    m_runtimeStatus->setObjectName(QStringLiteral("agentRuntimeStatus"));
    m_runtimeStatus->setStyleSheet(QStringLiteral("border:none; color:#b54708; font-size:11px; font-weight:600;"));
    toolbarLayout->addWidget(m_runtimeStatus);
    m_runtimeCapabilityStatus = new QLabel(QStringLiteral("能力未知"), toolbar);
    m_runtimeCapabilityStatus->setObjectName(QStringLiteral("agentRuntimeCapabilityStatus"));
    m_runtimeCapabilityStatus->setToolTip(
        QStringLiteral("等待版本化 runtime/degradations 能力声明"));
    m_runtimeCapabilityStatus->setStyleSheet(
        QStringLiteral("border:none; color:#b54708; font-size:10px; font-weight:600;"));
    toolbarLayout->addWidget(m_runtimeCapabilityStatus);
    m_runtimeRestartButton = new QPushButton(QStringLiteral("重启 Codex"), toolbar);
    m_runtimeRestartButton->setObjectName(QStringLiteral("agentRuntimeRestartButton"));
    m_runtimeRestartButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/rotate-ccw.svg")));
    m_runtimeRestartButton->setFixedHeight(30);
    m_runtimeRestartButton->setEnabled(false);
    m_runtimeRestartButton->setToolTip(QStringLiteral("Codex 进程退出后重新建立运行时连接"));
    m_runtimeRestartButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#ffffff; color:#475467; border:1px solid #d0d5dd;"
        "border-radius:6px; padding:0 9px; font-size:10px; }"
        "QPushButton:hover { background:#f2f4f7; color:#101828; }"
        "QPushButton:disabled { color:#98a2b3; background:#f9fafb; }"));
    connect(m_runtimeRestartButton, &QPushButton::clicked, this, [this]() {
        if (!m_runtimeRestartRequired || m_runtimeRecoveryMode) return;
        m_runtimeRestartButton->setEnabled(false);
        m_runtimeRestartButton->setText(QStringLiteral("重启中…"));
        m_runtime->restartRuntime();
    });
    toolbarLayout->addWidget(m_runtimeRestartButton);
    root->addWidget(toolbar);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(1);
    splitter->setStyleSheet(QStringLiteral("QSplitter::handle { background:#e4e7ec; }"));
    splitter->addWidget(buildProductRail());
    splitter->addWidget(buildAgentSurface());
    splitter->addWidget(buildWorkCanvas());
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    splitter->setSizes({188, 390, 520});
    root->addWidget(splitter, 1);

    auto *sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(sendShortcut, &QShortcut::activated, this, &AgentWorkbenchWidget::submitPrompt);
}

QWidget *AgentWorkbenchWidget::buildProductRail()
{
    auto *rail = new QWidget(this);
    rail->setObjectName(QStringLiteral("agentProductRail"));
    rail->setMinimumWidth(164);
    rail->setMaximumWidth(224);
    rail->setStyleSheet(QStringLiteral("QWidget#agentProductRail { background:#ffffff; }"));
    auto *layout = new QVBoxLayout(rail);
    layout->setContentsMargins(12, 14, 12, 14);
    layout->setSpacing(8);

    m_newSessionButton = new QPushButton(QStringLiteral("新建会话"), rail);
    auto *newTask = m_newSessionButton;
    newTask->setObjectName(QStringLiteral("agentNewSessionButton"));
    newTask->setIcon(QIcon(QStringLiteral(":/icons/lucide/plus.svg")));
    newTask->setCursor(Qt::PointingHandCursor);
    newTask->setFixedHeight(36);
    newTask->setStyleSheet(QStringLiteral(
        "QPushButton { background:#165DFF; color:white; border:none; border-radius:6px;"
        "font-size:12px; font-weight:700; text-align:left; padding:0 10px; }"
        "QPushButton:hover { background:#0F46C6; }"));
    connect(newTask, &QPushButton::clicked, this, [this]() {
        if (m_mode == QStringLiteral("work")) m_workSessionId.clear();
        else {
            m_chatSessionId.clear();
            m_chatSessionProjectId.clear();
        }
        resetSessionHistoryPagination();
        clearContextItems();
        m_itemLabels.clear();
        m_itemArtifactButtons.clear();
        m_commandArtifactRequests.clear();
        while (m_timelineLayout && m_timelineLayout->count() > 2) {
            QLayoutItem *item = m_timelineLayout->takeAt(1);
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        if (m_emptyTimeline) m_emptyTimeline->show();
        updateTurnAction();
        updateTerminalControls();
        m_composer->setFocus();
    });
    layout->addWidget(newTask);

    m_openProjectButton = new QPushButton(QStringLiteral("打开文件夹"), rail);
    auto *openProject = m_openProjectButton;
    openProject->setObjectName(QStringLiteral("agentOpenProjectButton"));
    openProject->setIcon(QIcon(QStringLiteral(":/icons/lucide/folder-open.svg")));
    openProject->setCursor(Qt::PointingHandCursor);
    openProject->setFixedHeight(34);
    openProject->setStyleSheet(QStringLiteral(
        "QPushButton { background:#ffffff; color:#344054; border:1px solid #d0d5dd;"
        "border-radius:6px; font-size:12px; text-align:left; padding:0 10px; }"
        "QPushButton:hover { background:#f9fafb; color:#101828; }"));
    connect(openProject, &QPushButton::clicked, this, &AgentWorkbenchWidget::chooseProject);
    layout->addWidget(openProject);

    layout->addSpacing(6);
    layout->addWidget(makeSectionLabel(QStringLiteral("项目"), rail));
    m_projectList = new QListWidget(rail);
    m_projectList->setFrameShape(QFrame::NoFrame);
    m_projectList->setMaximumHeight(180);
    m_projectList->setStyleSheet(QStringLiteral(
        "QListWidget { background:transparent; color:#475467; font-size:11px; outline:none; }"
        "QListWidget::item { padding:7px 6px; border-radius:5px; }"
        "QListWidget::item:selected { background:#EEF4FF; color:#165DFF; }"));
    m_projectList->addItem(QStringLiteral("正在读取项目…"));
    connect(m_projectList, &QListWidget::itemClicked,
            this, &AgentWorkbenchWidget::openProjectFromList);
    m_projectList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_projectList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint &position) {
        QListWidgetItem *item = m_projectList->itemAt(position);
        if (!item || m_runtimeRecoveryMode || !m_runtime || !m_runtime->isReady()) return;
        const QString projectId = item->data(kProjectIdRole).toString();
        const QString root = item->data(kProjectRootRole).toString();
        if (projectId.isEmpty() || root.isEmpty()) return;
        QMenu menu(m_projectList);
        const bool pinned = item->data(kProjectPinnedRole).toBool();
        QAction *open = menu.addAction(QIcon(QStringLiteral(":/icons/lucide/folder-open.svg")),
                                       QStringLiteral("打开项目"));
        QAction *pin = menu.addAction(
            pinned ? QStringLiteral("取消固定") : QStringLiteral("固定到项目栏"));
        QAction *manage = menu.addAction(QIcon(QStringLiteral(":/icons/lucide/folder-open.svg")),
                                         QStringLiteral("管理项目根…"));
        QAction *trust = nullptr;
        if (projectId == m_projectId && !m_projectTrustReview.isEmpty()
                && m_projectTrustReview.value(QStringLiteral("trust_state")).toString()
                    != QStringLiteral("acknowledged")) {
            trust = menu.addAction(QStringLiteral("确认项目信任审核…"));
        }
        QAction *selected = menu.exec(m_projectList->viewport()->mapToGlobal(position));
        if (selected == open) {
            m_runtime->openProject(root);
        } else if (selected == pin) {
            m_projectNavigationRequestId = m_runtime->updateProjectNavigation(
                projectId, !pinned);
        } else if (selected == manage && projectId == m_projectId) {
            beginProjectRootManagement();
        } else if (selected == trust && trust) {
            const QJsonArray instructions = m_projectTrustReview
                .value(QStringLiteral("instructions")).toArray();
            const QJsonArray hooks = m_projectTrustReview
                .value(QStringLiteral("executable_hooks")).toArray();
            const auto answer = QMessageBox::question(
                this, QStringLiteral("确认项目信任审核"),
                QStringLiteral(
                    "确认当前审核快照？\n\n发现 %1 个指令文件、%2 个 Hook。"
                    "确认只会记录你已查看当前内容摘要，不会授予 Agent 写入、命令、Hook 或网络权限。"
                    "指令或 Hook 内容变化后，确认会自动失效。")
                    .arg(instructions.size()).arg(hooks.size()),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes) return;
            m_projectTrustRequestId = m_runtime->acknowledgeProjectTrustReview(
                m_projectId, QStringLiteral("root-1"),
                m_projectTrustReview.value(QStringLiteral("root_identity")).toString(),
                m_projectTrustReview.value(QStringLiteral("review_id")).toString());
            addNotice(QStringLiteral("正在重新验证并记录项目信任审核…"));
        }
    });
    layout->addWidget(m_projectList);

    layout->addWidget(makeSectionLabel(QStringLiteral("最近会话"), rail));
    m_sessionSearchInput = new QLineEdit(rail);
    m_sessionSearchInput->setObjectName(QStringLiteral("agentSessionSearchInput"));
    m_sessionSearchInput->setPlaceholderText(QStringLiteral("搜索会话"));
    m_sessionSearchInput->setClearButtonEnabled(true);
    m_sessionSearchInput->setMaxLength(256);
    m_sessionSearchInput->addAction(
        QIcon(QStringLiteral(":/icons/lucide/search.svg")), QLineEdit::LeadingPosition);
    m_sessionSearchInput->setToolTip(QStringLiteral("搜索会话标题和本地对话记录"));
    connect(m_sessionSearchInput, &QLineEdit::textChanged, this,
            [this](const QString &text) {
        const QString expected = text;
        QTimer::singleShot(180, this, [this, expected]() {
            if (!m_sessionSearchInput
                    || m_sessionSearchInput->text() != expected) return;
            requestSessionList();
        });
    });
    layout->addWidget(m_sessionSearchInput);
    m_sessionList = new QListWidget(rail);
    m_sessionList->setObjectName(QStringLiteral("agentSessionList"));
    m_sessionList->setFrameShape(QFrame::NoFrame);
    m_sessionList->setStyleSheet(QStringLiteral(
        "QListWidget { background:transparent; color:#475467; font-size:11px; outline:none; }"
        "QListWidget::item { padding:8px 6px; border-radius:5px; }"
        "QListWidget::item:selected { background:#f2f4f7; color:#101828; }"));
    m_sessionList->addItem(QStringLiteral("会话将在首次发送后创建"));
    connect(m_sessionList, &QListWidget::itemClicked,
            this, &AgentWorkbenchWidget::loadSessionFromList);
    m_sessionList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sessionList, &QListWidget::customContextMenuRequested,
            this, [this](const QPoint &position) {
        QListWidgetItem *item = m_sessionList->itemAt(position);
        if (!item || m_runtimeRecoveryMode || !m_runtime->isReady()
                || !m_sessionMutationRequestId.isEmpty()
                || !m_sessionDeletionRequestId.isEmpty()
                || !m_portableSessionRequestId.isEmpty()) return;
        const QString sessionId = item->data(kSessionIdRole).toString();
        if (sessionId.isEmpty()) return;
        const bool archived = item->data(kSessionStatusRole).toString()
            == QStringLiteral("archived");
        const bool recoveryRequired = item->data(kSessionRecoveryRole).toBool();
        const bool deletionPending = item->data(kSessionDeletionPendingRole).toBool();
        QMenu menu(m_sessionList);
        QAction *resume = menu.addAction(QStringLiteral("继续会话"));
        QAction *fork = menu.addAction(QStringLiteral("从此会话分支"));
        QAction *rename = menu.addAction(QStringLiteral("重命名会话"));
        QAction *lifecycle = menu.addAction(
            archived ? QStringLiteral("恢复会话") : QStringLiteral("归档会话"));
        QAction *exportSession = menu.addAction(QStringLiteral("导出会话…"));
        QAction *retentionPolicy = menu.addAction(QStringLiteral("会话保留策略…"));
        QAction *createCompaction = menu.addAction(QStringLiteral("创建压缩审查…"));
        QAction *readCompaction = menu.addAction(QStringLiteral("查看压缩审查…"));
        QAction *backgroundNotifications = menu.addAction(QStringLiteral("后台通知记录…"));
        QAction *backgroundRecovery = menu.addAction(QStringLiteral("后台恢复状态…"));
        rename->setEnabled(!recoveryRequired && !deletionPending);
        resume->setEnabled(!archived && !recoveryRequired && !deletionPending
            && !m_turnRunning && m_sessionResumeRequestId.isEmpty());
        fork->setEnabled(!recoveryRequired && !deletionPending && !m_turnRunning
            && m_sessionForkRequestId.isEmpty());
        lifecycle->setEnabled(
            !recoveryRequired && !deletionPending && (archived || !m_turnRunning));
        exportSession->setEnabled(!recoveryRequired
            && (!m_turnRunning || m_activeTurnSessionId != sessionId));
        retentionPolicy->setEnabled(!recoveryRequired && !deletionPending);
        const bool compactionReady = m_compactionAvailable && !recoveryRequired && !deletionPending
            && !archived && !m_turnRunning && m_compactionRequestId.isEmpty();
        createCompaction->setEnabled(compactionReady);
        readCompaction->setEnabled(m_compactionAvailable && !recoveryRequired && !deletionPending
            && m_compactionRequestId.isEmpty());
        backgroundNotifications->setEnabled(m_backgroundNotificationInspectionAvailable
            && m_backgroundNotificationRequestId.isEmpty());
        backgroundRecovery->setEnabled(m_backgroundRecoveryInspectionAvailable
            && m_backgroundRecoveryRequestId.isEmpty());
        menu.addSeparator();
        QAction *deletion = menu.addAction(
            deletionPending ? QStringLiteral("撤销删除") : QStringLiteral("删除会话…"));
        deletion->setEnabled(!recoveryRequired && (!m_turnRunning || deletionPending));
        QAction *recoveryStatus = nullptr;
        if (recoveryRequired) {
            menu.addSeparator();
            recoveryStatus = menu.addAction(QStringLiteral("查看恢复状态"));
        }
        QAction *selected = menu.exec(m_sessionList->viewport()->mapToGlobal(position));
        if (selected == resume) {
            m_sessionResumeRequestId = m_runtime->resumeSession(sessionId);
            addNotice(QStringLiteral("正在恢复会话运行时…"));
        } else if (selected == fork) {
            m_sessionForkRequestId = m_runtime->forkSession(sessionId);
            addNotice(QStringLiteral("正在创建会话分支…"));
        } else if (selected == rename) {
            bool accepted = false;
            const QString title = QInputDialog::getText(
                this, QStringLiteral("重命名会话"), QStringLiteral("会话标题"),
                QLineEdit::Normal, item->data(kSessionTitleRole).toString(), &accepted).trimmed();
            if (!accepted || title.isEmpty()) return;
            m_sessionMutationRequestId = m_runtime->renameSession(sessionId, title);
        } else if (selected == lifecycle) {
            m_sessionMutationRequestId = archived
                ? m_runtime->unarchiveSession(sessionId)
                : m_runtime->archiveSession(sessionId);
        } else if (selected == exportSession) {
            beginPortableSessionExport(item);
        } else if (selected == retentionPolicy) {
            QString label = item->data(kSessionTitleRole).toString();
            if (label.isEmpty()) label = sessionId;
            beginRetentionPolicy(
                QStringLiteral("session"), sessionId, label);
        } else if (selected == createCompaction) {
            beginCompactionCheckpoint(sessionId);
        } else if (selected == readCompaction) {
            beginCompactionCheckpointRead(sessionId);
        } else if (selected == backgroundNotifications) {
            beginBackgroundNotificationInspection(sessionId);
        } else if (selected == backgroundRecovery) {
            beginBackgroundRecoveryInspection(sessionId);
        } else if (selected == deletion) {
            if (deletionPending) {
                const QString deletionId = item->data(kSessionDeletionIdRole).toString();
                if (!deletionId.isEmpty()) {
                    m_sessionDeletionRequestId = m_runtime->undoSessionDeletion(deletionId);
                }
            } else {
                beginSessionDeletion(item);
            }
        } else if (selected == recoveryStatus) {
            m_runtime->sessionRecoveryStatus(sessionId);
        }
    });
    layout->addWidget(m_sessionList, 1);

    m_importSessionButton = new QPushButton(QStringLiteral("导入会话"), rail);
    m_importSessionButton->setObjectName(QStringLiteral("agentImportSessionButton"));
    m_importSessionButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/folder-open.svg")));
    m_importSessionButton->setEnabled(false);
    m_importSessionButton->setFixedHeight(32);
    m_importSessionButton->setToolTip(QStringLiteral("从脱敏的 Aegisy 便携包导入会话"));
    m_importSessionButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:transparent; border:none; color:#475467;"
        "text-align:left; padding:6px; }"
        "QPushButton:hover { background:#f2f4f7; color:#101828; }"
        "QPushButton:disabled { background:transparent; color:#98a2b3; }"));
    connect(m_importSessionButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::beginPortableSessionImport);
    layout->addWidget(m_importSessionButton);

    m_retentionSettingsButton = new QPushButton(QStringLiteral("项目保留策略"), rail);
    auto *settings = m_retentionSettingsButton;
    settings->setObjectName(QStringLiteral("agentRetentionSettingsButton"));
    settings->setIcon(QIcon(QStringLiteral(":/icons/lucide/settings.svg")));
    settings->setEnabled(false);
    settings->setFixedHeight(32);
    settings->setToolTip(QStringLiteral("设置当前项目会话的自动归档与删除周期"));
    settings->setStyleSheet(QStringLiteral(
        "QPushButton { background:transparent; border:none; color:#475467;"
        "text-align:left; padding:6px; }"
        "QPushButton:hover { background:#f2f4f7; color:#101828; }"
        "QPushButton:disabled { background:transparent; color:#98a2b3; }"));
    connect(settings, &QPushButton::clicked, this, [this]() {
        if (m_projectId.isEmpty()) return;
        beginRetentionPolicy(
            QStringLiteral("project"), m_projectId,
            m_projectLabel ? m_projectLabel->text() : m_projectId);
    });
    layout->addWidget(settings);
    return rail;
}

QWidget *AgentWorkbenchWidget::buildAgentSurface()
{
    auto *surface = new QWidget(this);
    surface->setObjectName(QStringLiteral("agentSurface"));
    surface->setMinimumWidth(330);
    surface->setMaximumWidth(520);
    surface->setStyleSheet(QStringLiteral("QWidget#agentSurface { background:#ffffff; }"));
    auto *layout = new QVBoxLayout(surface);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *header = new QWidget(surface);
    header->setFixedHeight(48);
    auto *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(16, 9, 16, 7);
    headerLayout->setSpacing(1);
    auto *title = new QLabel(QStringLiteral("AI 对话"), header);
    title->setStyleSheet(QStringLiteral("color:#101828; font-size:13px; font-weight:700;"));
    m_contextStrip = new QLabel(header);
    m_contextStrip->setObjectName(QStringLiteral("agentExecutionContextStrip"));
    m_contextStrip->setStyleSheet(QStringLiteral("color:#667085; font-size:10px;"));
    headerLayout->addWidget(title);
    headerLayout->addWidget(m_contextStrip);
    layout->addWidget(header);

    m_recoveryBanner = new QLabel(surface);
    m_recoveryBanner->setObjectName(QStringLiteral("agentRecoveryBanner"));
    m_recoveryBanner->setWordWrap(true);
    m_recoveryBanner->setTextFormat(Qt::PlainText);
    m_recoveryBanner->setStyleSheet(QStringLiteral(
        "QLabel { background:#fffaeb; color:#93370d; border-top:1px solid #fedf89;"
        "border-bottom:1px solid #fedf89; padding:8px 12px; font-size:10px; }"));
    m_recoveryBanner->hide();
    layout->addWidget(m_recoveryBanner);

    m_operationStatusRow = new QWidget(surface);
    m_operationStatusRow->setObjectName(QStringLiteral("agentOperationStatusRow"));
    auto *operationStatusLayout = new QHBoxLayout(m_operationStatusRow);
    operationStatusLayout->setContentsMargins(0, 0, 0, 0);
    operationStatusLayout->setSpacing(8);
    m_operationStatusBanner = new QLabel(m_operationStatusRow);
    m_operationStatusBanner->setObjectName(QStringLiteral("agentOperationStatusBanner"));
    m_operationStatusBanner->setWordWrap(true);
    m_operationStatusBanner->setTextFormat(Qt::PlainText);
    m_operationStatusBanner->setStyleSheet(QStringLiteral(
        "QLabel { background:#fffaeb; color:#93370d; border-top:1px solid #fedf89;"
        "border-bottom:1px solid #fedf89; padding:8px 12px; font-size:10px; }"));
    m_operationStatusBanner->hide();
    m_operationStatusReviewButton = new QPushButton(QStringLiteral("重新检查"),
                                                    m_operationStatusRow);
    m_operationStatusReviewButton->setObjectName(
        QStringLiteral("agentOperationStatusReviewButton"));
    m_operationStatusReviewButton->setIcon(
        QIcon(QStringLiteral(":/icons/lucide/rotate-ccw.svg")));
    m_operationStatusReviewButton->setToolTip(QStringLiteral(
        "读取受限运行时证据并记录复核；不会执行恢复或修改项目文件"));
    m_operationStatusReviewButton->setFixedHeight(30);
    m_operationStatusReviewButton->setCursor(Qt::PointingHandCursor);
    m_operationStatusReviewButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#fff7ed; color:#9a3412; border:1px solid #fed7aa;"
        "border-radius:5px; padding:0 9px; font-size:10px; }"
        "QPushButton:hover { background:#ffedd5; }"
        "QPushButton:disabled { color:#98a2b3; background:#f8fafc; border-color:#e4e7ec; }"));
    m_operationStatusReviewButton->setEnabled(false);
    m_operationStatusReviewButton->hide();
    operationStatusLayout->addWidget(m_operationStatusBanner, 1);
    operationStatusLayout->addWidget(m_operationStatusReviewButton);
    connect(m_operationStatusReviewButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::reviewOperationStatus);
    m_operationStatusRow->hide();
    layout->addWidget(m_operationStatusRow);

    m_sessionHistoryMoreButton = new QPushButton(QStringLiteral("加载更早记录"), surface);
    m_sessionHistoryMoreButton->setObjectName(QStringLiteral("agentSessionHistoryMoreButton"));
    m_sessionHistoryMoreButton->setIcon(
        QIcon(QStringLiteral(":/icons/lucide/rotate-ccw.svg")));
    m_sessionHistoryMoreButton->setToolTip(QStringLiteral("加载当前会话的更早记录"));
    m_sessionHistoryMoreButton->setFixedHeight(30);
    m_sessionHistoryMoreButton->setCursor(Qt::PointingHandCursor);
    m_sessionHistoryMoreButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#f8fafc; color:#475467; border:none;"
        "border-top:1px solid #e4e7ec; border-bottom:1px solid #e4e7ec;"
        "padding:0 12px; font-size:10px; text-align:center; }"
        "QPushButton:hover { background:#f2f4f7; color:#101828; }"
        "QPushButton:disabled { color:#98a2b3; }"));
    m_sessionHistoryMoreButton->hide();
    connect(m_sessionHistoryMoreButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::loadOlderSessionHistory);
    layout->addWidget(m_sessionHistoryMoreButton);

    m_timelineScroll = new QScrollArea(surface);
    m_timelineScroll->setWidgetResizable(true);
    m_timelineScroll->setFrameShape(QFrame::NoFrame);
    m_timelineScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_timelineContent = new QWidget(m_timelineScroll);
    m_timelineContent->setObjectName(QStringLiteral("agentTimeline"));
    m_timelineContent->setStyleSheet(QStringLiteral(
        "QWidget#agentTimeline { background:#f8fafc; }"));
    m_timelineLayout = new QVBoxLayout(m_timelineContent);
    m_timelineLayout->setContentsMargins(14, 16, 14, 16);
    m_timelineLayout->setSpacing(10);
    m_emptyTimeline = new QLabel(
        QStringLiteral("从这里开始与 Aegisy Agent 对话。\nChat 默认不修改文件；Work 会绑定当前项目。"),
        m_timelineContent);
    m_emptyTimeline->setAlignment(Qt::AlignCenter);
    m_emptyTimeline->setWordWrap(true);
    m_emptyTimeline->setStyleSheet(QStringLiteral("color:#98a2b3; font-size:12px; padding:24px;"));
    m_timelineLayout->addWidget(m_emptyTimeline);
    m_timelineLayout->addStretch();
    m_timelineScroll->setWidget(m_timelineContent);
    layout->addWidget(m_timelineScroll, 1);

    auto *composerFrame = new QFrame(surface);
    composerFrame->setStyleSheet(QStringLiteral("QFrame { background:#ffffff; border-top:1px solid #e4e7ec; }"));
    auto *composerLayout = new QVBoxLayout(composerFrame);
    composerLayout->setContentsMargins(12, 10, 12, 12);
    composerLayout->setSpacing(7);
    m_composer = new QTextEdit(composerFrame);
    m_composer->setObjectName(QStringLiteral("agentComposer"));
    m_composer->setPlaceholderText(QStringLiteral("向 Aegisy Agent 发送消息…"));
    m_composer->setAcceptRichText(false);
    m_composer->setMinimumHeight(70);
    m_composer->setMaximumHeight(110);
    m_composer->setStyleSheet(QStringLiteral(
        "QTextEdit { background:#ffffff; color:#101828; border:1px solid #d0d5dd;"
        "border-radius:7px; padding:8px; font-size:12px; }"
        "QTextEdit:focus { border-color:#84A8FF; }"));
    composerLayout->addWidget(m_composer);
    m_contextPanel = new QWidget(composerFrame);
    m_contextPanel->setObjectName(QStringLiteral("agentContextPanel"));
    m_contextPanel->setStyleSheet(QStringLiteral(
        "QWidget#agentContextPanel { background:#f8fafc; border:1px solid #e4e7ec;"
        "border-radius:6px; }"));
    auto *contextLayout = new QVBoxLayout(m_contextPanel);
    contextLayout->setContentsMargins(7, 5, 7, 5);
    contextLayout->setSpacing(3);
    m_contextSummary = new QLabel(QStringLiteral("上下文"), m_contextPanel);
    m_contextSummary->setObjectName(QStringLiteral("agentContextSummary"));
    m_contextSummary->setStyleSheet(QStringLiteral(
        "color:#475467; font-size:9px; font-weight:600; border:none;"));
    contextLayout->addWidget(m_contextSummary);
    m_contextList = new QListWidget(m_contextPanel);
    m_contextList->setObjectName(QStringLiteral("agentContextList"));
    m_contextList->setFrameShape(QFrame::NoFrame);
    m_contextList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_contextList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_contextList->setStyleSheet(QStringLiteral(
        "QListWidget { background:transparent; border:none; outline:none; }"
        "QListWidget::item { border:none; min-height:28px; }"));
    contextLayout->addWidget(m_contextList);
    m_contextPanel->hide();
    composerLayout->addWidget(m_contextPanel);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(6);
    m_attachContextButton = new QPushButton(composerFrame);
    m_attachContextButton->setObjectName(QStringLiteral("agentAttachContextButton"));
    m_attachContextButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")));
    m_attachContextButton->setToolTip(QStringLiteral("添加对话上下文"));
    m_attachContextButton->setFixedSize(30, 30);
    m_attachContextButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:transparent; border:none; border-radius:5px; }"
        "QPushButton:hover { background:#eaecf0; }"
        "QPushButton::menu-indicator { image:none; width:0; }"));
    auto *contextMenu = new QMenu(m_attachContextButton);
    contextMenu->setObjectName(QStringLiteral("agentAttachContextMenu"));
    QAction *fileContext = contextMenu->addAction(QStringLiteral("添加文件树选中文件"));
    fileContext->setObjectName(QStringLiteral("agentAddFileContextAction"));
    QAction *selectionContext = contextMenu->addAction(QStringLiteral("添加编辑器选区"));
    selectionContext->setObjectName(QStringLiteral("agentAddSelectionContextAction"));
    QAction *searchContext = contextMenu->addAction(QStringLiteral("添加选中搜索结果"));
    searchContext->setObjectName(QStringLiteral("agentAddSearchContextAction"));
    QAction *diagnosticContext = contextMenu->addAction(QStringLiteral("添加选中诊断"));
    diagnosticContext->setObjectName(QStringLiteral("agentAddDiagnosticContextAction"));
    contextMenu->addSeparator();
    m_pinFileContextAction = contextMenu->addAction(
        QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")),
        QStringLiteral("固定文件树选中文件"));
    m_pinFileContextAction->setObjectName(QStringLiteral("agentPinFileContextAction"));
    connect(fileContext, &QAction::triggered,
            this, &AgentWorkbenchWidget::addSelectedFileContext);
    connect(selectionContext, &QAction::triggered,
            this, &AgentWorkbenchWidget::addEditorSelectionContext);
    connect(searchContext, &QAction::triggered,
            this, &AgentWorkbenchWidget::addSearchResultContext);
    connect(diagnosticContext, &QAction::triggered,
            this, &AgentWorkbenchWidget::addDiagnosticContext);
    connect(m_pinFileContextAction, &QAction::triggered,
            this, &AgentWorkbenchWidget::pinSelectedFileContext);
    m_pinSelectionContextAction = contextMenu->addAction(
        QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")),
        QStringLiteral("固定编辑器选区"));
    m_pinSelectionContextAction->setObjectName(
        QStringLiteral("agentPinSelectionContextAction"));
    connect(m_pinSelectionContextAction, &QAction::triggered,
            this, &AgentWorkbenchWidget::pinEditorSelectionContext);
    m_pinDiagnosticContextAction = contextMenu->addAction(
        QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")),
        QStringLiteral("固定选中诊断"));
    m_pinDiagnosticContextAction->setObjectName(
        QStringLiteral("agentPinDiagnosticContextAction"));
    connect(m_pinDiagnosticContextAction, &QAction::triggered,
            this, &AgentWorkbenchWidget::pinSelectedDiagnosticContext);
    m_pinImageContextAction = contextMenu->addAction(
        QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")),
        QStringLiteral("固定图片"));
    m_pinImageContextAction->setObjectName(
        QStringLiteral("agentPinImageContextAction"));
    connect(m_pinImageContextAction, &QAction::triggered,
            this, &AgentWorkbenchWidget::pinImageContext);
    connect(contextMenu, &QMenu::aboutToShow, this,
            [this, fileContext, selectionContext, searchContext, diagnosticContext]() {
        QTreeWidgetItem *file = m_fileTree ? m_fileTree->currentItem() : nullptr;
        fileContext->setEnabled(file
            && file->data(0, kKindRole).toString() == QStringLiteral("file"));
        const EditorBuffer buffer = m_editorBuffers.value(m_openEditorPath);
        selectionContext->setEnabled(!m_openEditorPath.isEmpty()
            && buffer.cursorPosition != buffer.anchorPosition);
        searchContext->setEnabled(m_workspaceSearchResults
            && m_workspaceSearchResults->currentItem());
        diagnosticContext->setEnabled(m_languageDiagnostics
            && m_languageDiagnostics->currentItem());
        if (m_pinFileContextAction) {
            m_pinFileContextAction->setEnabled(m_pinnedContextAvailable
                && !m_runtimeRecoveryMode && !m_projectId.isEmpty()
                && !currentSessionRecoveryRequired() && !currentSessionDeletionPending()
                && !currentOperationStatusBlocked()
                && m_pinnedContextMutationRequestId.isEmpty()
                && m_pinnedImageImportRequestId.isEmpty()
                && m_pinnedFileReadRequestId.isEmpty() && fileContext->isEnabled());
        }
        if (m_pinSelectionContextAction) {
            const EditorBuffer buffer = m_editorBuffers.value(m_openEditorPath);
            m_pinSelectionContextAction->setEnabled(m_pinnedContextAvailable
                && !m_runtimeRecoveryMode && !m_projectId.isEmpty()
                && !currentSessionRecoveryRequired() && !currentSessionDeletionPending()
                && !currentOperationStatusBlocked()
                && m_pinnedContextMutationRequestId.isEmpty()
                && m_pinnedImageImportRequestId.isEmpty()
                && m_pinnedFileReadRequestId.isEmpty() && selectionContext->isEnabled()
                && !buffer.modified && !buffer.conflict);
        }
        if (m_pinDiagnosticContextAction) {
            const QTreeWidgetItem *diagnostic = m_languageDiagnostics
                ? m_languageDiagnostics->currentItem() : nullptr;
            const bool hasRaw = diagnostic
                && !diagnostic->data(0, kContextRole).toMap()
                    .value(QStringLiteral("raw_output_ref")).toString().isEmpty();
            m_pinDiagnosticContextAction->setEnabled(m_pinnedContextAvailable
                && !m_runtimeRecoveryMode && !m_projectId.isEmpty()
                && !currentSessionRecoveryRequired() && !currentSessionDeletionPending()
                && !currentOperationStatusBlocked()
                && m_pinnedContextMutationRequestId.isEmpty()
                && m_pinnedImageImportRequestId.isEmpty()
                && m_pendingPinnedDiagnosticRequestId.isEmpty() && hasRaw);
        }
        if (m_pinImageContextAction) {
            m_pinImageContextAction->setEnabled(m_pinnedContextAvailable
                && m_imageContextAvailable && !m_runtimeRecoveryMode
                && m_mode == QStringLiteral("work") && !m_projectId.isEmpty()
                && !m_workSessionId.isEmpty() && !currentSessionRecoveryRequired()
                && !currentSessionDeletionPending() && !currentOperationStatusBlocked()
                && m_pinnedContextMutationRequestId.isEmpty()
                && m_pinnedImageImportRequestId.isEmpty()
                && m_pinnedImagePreviewRequestId.isEmpty());
        }
    });
    m_attachContextButton->setMenu(contextMenu);
    actions->addWidget(m_attachContextButton);
    m_contextInspectButton = new QPushButton(composerFrame);
    m_contextInspectButton->setObjectName(QStringLiteral("agentContextInspectButton"));
    m_contextInspectButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/search.svg")));
    m_contextInspectButton->setToolTip(QStringLiteral("预检发送上下文（不启动模型）"));
    m_contextInspectButton->setFixedSize(30, 30);
    m_contextInspectButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:transparent; border:none; border-radius:5px; }"
        "QPushButton:hover { background:#eaecf0; }"
        "QPushButton:disabled { opacity:0.45; }"));
    connect(m_contextInspectButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::inspectContext);
    actions->addWidget(m_contextInspectButton);
    actions->addStretch();
    auto *hint = new QLabel(QStringLiteral("Ctrl+Enter"), composerFrame);
    hint->setStyleSheet(QStringLiteral("border:none; color:#98a2b3; font-size:10px;"));
    actions->addWidget(hint);
    m_sendButton = new QPushButton(QStringLiteral("发送"), composerFrame);
    m_sendButton->setObjectName(QStringLiteral("agentSendButton"));
    m_sendButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/send.svg")));
    m_sendButton->setEnabled(false);
    m_sendButton->setFixedSize(84, 30);
    m_sendButton->setCursor(Qt::PointingHandCursor);
    m_sendButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#165DFF; color:white; border:none; border-radius:6px;"
        "padding:0 12px; font-size:11px; font-weight:700; }"
        "QPushButton:hover { background:#0F46C6; }"
        "QPushButton:disabled { background:#d0d5dd; color:#ffffff; }"));
    connect(m_sendButton, &QPushButton::clicked, this, [this]() {
        if (m_turnRunning) cancelActiveTurn();
        else submitPrompt();
    });
    actions->addWidget(m_sendButton);
    composerLayout->addLayout(actions);
    layout->addWidget(composerFrame);
    updateContextStrip();
    return surface;
}

QWidget *AgentWorkbenchWidget::buildWorkCanvas()
{
    auto *canvas = new QWidget(this);
    canvas->setObjectName(QStringLiteral("agentWorkCanvas"));
    canvas->setMinimumWidth(260);
    canvas->setStyleSheet(QStringLiteral("QWidget#agentWorkCanvas { background:#ffffff; }"));
    canvas->setAutoFillBackground(true);
    QPalette canvasPalette = canvas->palette();
    canvasPalette.setColor(QPalette::Window, QColor(QStringLiteral("#ffffff")));
    canvas->setPalette(canvasPalette);
    auto *layout = new QVBoxLayout(canvas);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_workspaceTabs = new QTabWidget(canvas);
    m_workspaceTabs->setObjectName(QStringLiteral("agentWorkspaceTabs"));
    m_workspaceTabs->installEventFilter(this);
    m_workspaceTabs->setDocumentMode(true);
    m_workspaceTabs->setAutoFillBackground(true);
    m_workspaceTabs->setPalette(canvasPalette);
    m_workspaceTabs->tabBar()->setExpanding(true);
    m_workspaceTabs->setStyleSheet(QStringLiteral(
        "QTabWidget { background:#ffffff; }"
        "QTabWidget::pane { border:none; border-top:1px solid #e4e7ec; background:#ffffff; }"
        "QTabBar { background:#ffffff; spacing:0; }"
        "QTabBar::tab { background:#ffffff; color:#667085; border:none; padding:12px 14px;"
        "font-size:11px; min-width:38px; }"
        "QTabBar::tab:selected { color:#165DFF; font-weight:700; border-bottom:2px solid #4B7DFF; }"
        "QTabBar::tab:hover { color:#101828; background:#f9fafb; }"));
    auto *filesPage = new QWidget(m_workspaceTabs);
    auto *filesLayout = new QVBoxLayout(filesPage);
    filesLayout->setContentsMargins(10, 10, 10, 10);
    filesLayout->setSpacing(6);
    m_fileFilter = new QLineEdit(filesPage);
    m_fileFilter->setObjectName(QStringLiteral("agentFileFilter"));
    m_fileFilter->setPlaceholderText(QStringLiteral("筛选已加载的文件和目录"));
    m_fileFilter->setClearButtonEnabled(true);
    m_fileFilter->setStyleSheet(QStringLiteral(
        "QLineEdit { background:#ffffff; color:#344054; border:1px solid #d0d5dd;"
        "border-radius:6px; padding:0 9px; min-height:28px; max-height:28px; font-size:11px; }"
        "QLineEdit:focus { border-color:#84A8FF; }"));
    filesLayout->addWidget(m_fileFilter);
    m_fileTree = new QTreeWidget(filesPage);
    m_fileTree->setObjectName(QStringLiteral("agentFileTree"));
    m_fileTree->setHeaderLabels(
        {QStringLiteral("名称"), QStringLiteral("大小"), QStringLiteral("Git")});
    m_fileTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_fileTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_fileTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_fileTree->setRootIsDecorated(true);
    m_fileTree->setAlternatingRowColors(false);
    m_fileTree->setStyleSheet(QStringLiteral(
        "QTreeWidget { background:#ffffff; color:#344054; border:none;"
        "font-size:11px; outline:none; }"
        "QTreeWidget::item { min-height:26px; }"
        "QTreeWidget::item:selected { background:#EEF4FF; color:#165DFF; }"
        "QHeaderView::section { background:#f9fafb; color:#667085; border:none;"
        "border-bottom:1px solid #e4e7ec; padding:6px; font-size:10px; }"));
    m_fileStatus = new QLabel(QStringLiteral("打开文件夹后显示受授权项目内容"), filesPage);
    m_fileStatus->setWordWrap(true);
    m_fileStatus->setStyleSheet(QStringLiteral("color:#667085; font-size:10px; padding:2px;"));
    filesLayout->addWidget(m_fileTree, 1);
    filesLayout->addWidget(m_fileStatus);
    connect(m_fileFilter, &QLineEdit::textChanged,
            this, [this]() { applyFileFilter(); });
    connect(m_fileTree, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem *item) {
        if (item->data(0, kKindRole).toString() != QStringLiteral("directory")
                || item->data(0, kLoadedRole).toBool()) return;
        item->setData(0, kLoadedRole, true);
        qDeleteAll(item->takeChildren());
        requestDirectoryListing(item->data(0, kPathRole).toString());
    });
    connect(m_fileTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) { openWorkspaceFile(item); });
    connect(m_fileTree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) { openWorkspaceFile(item); });
    auto *fileContextAction = new QAction(QStringLiteral("添加到对话上下文"), m_fileTree);
    fileContextAction->setObjectName(QStringLiteral("agentFileTreeContextAction"));
    connect(fileContextAction, &QAction::triggered,
            this, &AgentWorkbenchWidget::addSelectedFileContext);
    m_fileTree->addAction(fileContextAction);
    if (m_pinFileContextAction) m_fileTree->addAction(m_pinFileContextAction);
    m_fileTree->setContextMenuPolicy(Qt::ActionsContextMenu);
    m_workspaceTabs->addTab(filesPage, QStringLiteral("文件"));

    auto *searchPage = new QWidget(m_workspaceTabs);
    auto *workspaceSearchLayout = new QVBoxLayout(searchPage);
    workspaceSearchLayout->setContentsMargins(10, 10, 10, 10);
    workspaceSearchLayout->setSpacing(8);
    auto *searchCommandRow = new QHBoxLayout;
    searchCommandRow->setSpacing(6);
    m_workspaceSearchInput = new QLineEdit(searchPage);
    m_workspaceSearchInput->setObjectName(QStringLiteral("agentWorkspaceSearchInput"));
    m_workspaceSearchInput->setPlaceholderText(QStringLiteral("搜索文件名和项目文本"));
    m_workspaceSearchInput->setClearButtonEnabled(true);
    searchCommandRow->addWidget(m_workspaceSearchInput, 1);
    m_workspaceSearchButton = new QPushButton(QStringLiteral("搜索"), searchPage);
    m_workspaceSearchButton->setObjectName(QStringLiteral("agentWorkspaceSearchButton"));
    m_workspaceSearchButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/search.svg")));
    m_workspaceSearchButton->setFixedHeight(30);
    searchCommandRow->addWidget(m_workspaceSearchButton);
    m_workspaceSearchCancelButton = new QPushButton(searchPage);
    m_workspaceSearchCancelButton->setObjectName(
        QStringLiteral("agentWorkspaceSearchCancelButton"));
    m_workspaceSearchCancelButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/x.svg")));
    m_workspaceSearchCancelButton->setToolTip(QStringLiteral("取消当前搜索"));
    m_workspaceSearchCancelButton->setFixedSize(30, 30);
    m_workspaceSearchCancelButton->setEnabled(false);
    m_workspaceSearchCancelButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#ffffff; border:1px solid #d0d5dd; border-radius:6px; }"
        "QPushButton:hover { background:#f2f4f7; }"));
    searchCommandRow->addWidget(m_workspaceSearchCancelButton);
    workspaceSearchLayout->addLayout(searchCommandRow);

    auto *searchOptionsRow = new QHBoxLayout;
    searchOptionsRow->setSpacing(8);
    m_workspaceSearchMode = new QComboBox(searchPage);
    m_workspaceSearchMode->setObjectName(QStringLiteral("agentWorkspaceSearchMode"));
    m_workspaceSearchMode->addItem(QStringLiteral("全部"), QStringLiteral("all"));
    m_workspaceSearchMode->addItem(QStringLiteral("文件名"), QStringLiteral("files"));
    m_workspaceSearchMode->addItem(QStringLiteral("文本"), QStringLiteral("text"));
    m_workspaceSearchMode->setFixedWidth(96);
    searchOptionsRow->addWidget(m_workspaceSearchMode);
    m_workspaceSearchCase = new QCheckBox(QStringLiteral("区分大小写"), searchPage);
    m_workspaceSearchCase->setObjectName(QStringLiteral("agentWorkspaceSearchCase"));
    searchOptionsRow->addWidget(m_workspaceSearchCase);
    searchOptionsRow->addStretch();
    workspaceSearchLayout->addLayout(searchOptionsRow);

    m_workspaceSearchResults = new QTreeWidget(searchPage);
    m_workspaceSearchResults->setObjectName(QStringLiteral("agentWorkspaceSearchResults"));
    m_workspaceSearchResults->setHeaderLabels(
        {QStringLiteral("文件"), QStringLiteral("位置"), QStringLiteral("匹配内容")});
    m_workspaceSearchResults->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_workspaceSearchResults->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_workspaceSearchResults->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_workspaceSearchResults->header()->resizeSection(0, 170);
    m_workspaceSearchResults->setRootIsDecorated(false);
    m_workspaceSearchResults->setAlternatingRowColors(true);
    m_workspaceSearchResults->setStyleSheet(QStringLiteral(
        "QTreeWidget { background:#ffffff; color:#344054; border:1px solid #e4e7ec;"
        "border-radius:6px; font-size:10px; outline:none; }"
        "QTreeWidget::item { min-height:28px; padding:1px 3px; }"
        "QTreeWidget::item:selected { background:#E8EFFF; color:#174EA6; }"));
    workspaceSearchLayout->addWidget(m_workspaceSearchResults, 1);

    auto *searchStatusRow = new QHBoxLayout;
    m_workspaceSearchStatus = new QLabel(QStringLiteral("输入关键词后搜索当前项目"), searchPage);
    m_workspaceSearchStatus->setObjectName(QStringLiteral("agentWorkspaceSearchStatus"));
    m_workspaceSearchStatus->setStyleSheet(
        QStringLiteral("color:#667085; font-size:10px;"));
    searchStatusRow->addWidget(m_workspaceSearchStatus, 1);
    m_workspaceSearchMoreButton = new QPushButton(QStringLiteral("加载更多"), searchPage);
    m_workspaceSearchMoreButton->setObjectName(
        QStringLiteral("agentWorkspaceSearchMoreButton"));
    m_workspaceSearchMoreButton->setEnabled(false);
    m_workspaceSearchMoreButton->setFixedHeight(28);
    m_workspaceSearchMoreButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#ffffff; color:#344054; border:1px solid #d0d5dd;"
        "border-radius:6px; padding:0 10px; font-size:10px; }"
        "QPushButton:hover { background:#f9fafb; }"
        "QPushButton:disabled { background:#f8fafc; color:#98a2b3; border-color:#e4e7ec; }"));
    searchStatusRow->addWidget(m_workspaceSearchMoreButton);
    workspaceSearchLayout->addLayout(searchStatusRow);
    connect(m_workspaceSearchButton, &QPushButton::clicked,
            this, [this]() { startWorkspaceSearch(false); });
    connect(m_workspaceSearchInput, &QLineEdit::returnPressed,
            this, [this]() { startWorkspaceSearch(false); });
    connect(m_workspaceSearchCancelButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::cancelWorkspaceSearch);
    connect(m_workspaceSearchMoreButton, &QPushButton::clicked,
            this, [this]() { startWorkspaceSearch(true); });
    connect(m_workspaceSearchResults, &QTreeWidget::itemActivated,
            this, [this](QTreeWidgetItem *item, int) {
        openWorkspaceSearchResult(item);
    });
    connect(m_workspaceSearchResults, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item, int) {
        openWorkspaceSearchResult(item);
    });
    auto *searchResultContextAction = new QAction(
        QStringLiteral("添加搜索结果到对话上下文"), m_workspaceSearchResults);
    searchResultContextAction->setObjectName(
        QStringLiteral("agentSearchResultContextAction"));
    connect(searchResultContextAction, &QAction::triggered,
            this, &AgentWorkbenchWidget::addSearchResultContext);
    m_workspaceSearchResults->addAction(searchResultContextAction);
    m_workspaceSearchResults->setContextMenuPolicy(Qt::ActionsContextMenu);
    m_workspaceTabs->addTab(searchPage, QStringLiteral("搜索"));

    auto *structurePage = new QWidget(m_workspaceTabs);
    auto *structureLayout = new QVBoxLayout(structurePage);
    structureLayout->setContentsMargins(10, 10, 10, 10);
    structureLayout->setSpacing(8);
    auto *structureToolbar = new QHBoxLayout;
    structureToolbar->setSpacing(7);
    m_repositoryRefreshButton = new QPushButton(QStringLiteral("刷新索引"), structurePage);
    m_repositoryRefreshButton->setObjectName(QStringLiteral("agentRepositoryRefreshButton"));
    m_repositoryRefreshButton->setIcon(
        QIcon(QStringLiteral(":/icons/lucide/rotate-ccw.svg")));
    m_repositoryRefreshButton->setToolTip(QStringLiteral("重新扫描变更文件并生成仓库地图"));
    m_repositoryRefreshButton->setFixedHeight(30);
    structureToolbar->addWidget(m_repositoryRefreshButton);
    m_repositoryCancelButton = new QPushButton(QStringLiteral("取消"), structurePage);
    m_repositoryCancelButton->setObjectName(QStringLiteral("agentRepositoryCancelButton"));
    m_repositoryCancelButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/x.svg")));
    m_repositoryCancelButton->setToolTip(QStringLiteral("取消当前索引并保留上一次完整快照"));
    m_repositoryCancelButton->setFixedHeight(30);
    m_repositoryCancelButton->setEnabled(false);
    m_repositoryCancelButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#ffffff; color:#344054; border:1px solid #d0d5dd;"
        "border-radius:6px; padding:0 10px; font-size:10px; }"
        "QPushButton:hover { background:#f9fafb; }"
        "QPushButton:disabled { background:#f8fafc; color:#98a2b3; border-color:#e4e7ec; }"));
    structureToolbar->addWidget(m_repositoryCancelButton);
    auto *budgetLabel = new QLabel(QStringLiteral("地图预算"), structurePage);
    budgetLabel->setStyleSheet(QStringLiteral("color:#667085; font-size:10px;"));
    structureToolbar->addWidget(budgetLabel);
    m_repositoryMapBudget = new QComboBox(structurePage);
    m_repositoryMapBudget->setObjectName(QStringLiteral("agentRepositoryMapBudget"));
    m_repositoryMapBudget->addItem(QStringLiteral("512 tokens"), 512);
    m_repositoryMapBudget->addItem(QStringLiteral("1,024 tokens"), 1024);
    m_repositoryMapBudget->addItem(QStringLiteral("2,048 tokens"), 2048);
    m_repositoryMapBudget->addItem(QStringLiteral("4,096 tokens"), 4096);
    m_repositoryMapBudget->addItem(QStringLiteral("8,192 tokens"), 8192);
    m_repositoryMapBudget->setCurrentIndex(2);
    m_repositoryMapBudget->setFixedWidth(132);
    structureToolbar->addWidget(m_repositoryMapBudget);
    structureToolbar->addStretch();
    structureLayout->addLayout(structureToolbar);

    auto *structureSplitter = new QSplitter(Qt::Vertical, structurePage);
    structureSplitter->setObjectName(QStringLiteral("agentRepositorySplitter"));
    structureSplitter->setChildrenCollapsible(false);
    m_repositoryViews = new QTabWidget(structureSplitter);
    m_repositoryViews->setDocumentMode(true);
    m_repositorySymbols = new QTreeWidget(m_repositoryViews);
    m_repositorySymbols->setObjectName(QStringLiteral("agentRepositorySymbols"));
    m_repositorySymbols->setHeaderLabels(
        {QStringLiteral("符号 / 文件"), QStringLiteral("类型"), QStringLiteral("位置")});
    m_repositorySymbols->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_repositorySymbols->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_repositorySymbols->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_repositorySymbols->setAlternatingRowColors(true);
    m_repositorySymbols->setStyleSheet(QStringLiteral(
        "QTreeWidget { background:#ffffff; color:#344054; border:none; font-size:10px; }"
        "QTreeWidget::item { min-height:25px; }"
        "QTreeWidget::item:selected { background:#E8EFFF; color:#174EA6; }"));
    m_repositoryViews->addTab(m_repositorySymbols, QStringLiteral("符号"));
    m_repositoryDependencies = new QTreeWidget(m_repositoryViews);
    m_repositoryDependencies->setObjectName(QStringLiteral("agentRepositoryDependencies"));
    m_repositoryDependencies->setHeaderLabels(
        {QStringLiteral("来源"), QStringLiteral("依赖"), QStringLiteral("类型 / 行")});
    m_repositoryDependencies->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_repositoryDependencies->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_repositoryDependencies->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_repositoryDependencies->header()->resizeSection(0, 160);
    m_repositoryDependencies->setRootIsDecorated(false);
    m_repositoryDependencies->setAlternatingRowColors(true);
    m_repositoryDependencies->setStyleSheet(m_repositorySymbols->styleSheet());
    m_repositoryViews->addTab(m_repositoryDependencies, QStringLiteral("依赖"));
    m_languageResults = new QTreeWidget(m_repositoryViews);
    m_languageResults->setObjectName(QStringLiteral("agentLanguageResults"));
    m_languageResults->setHeaderLabels(
        {QStringLiteral("类型"), QStringLiteral("文件"), QStringLiteral("位置")});
    m_languageResults->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_languageResults->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_languageResults->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_languageResults->setRootIsDecorated(false);
    m_languageResults->setAlternatingRowColors(true);
    m_languageResults->setStyleSheet(m_repositorySymbols->styleSheet());
    m_languageResultsView = m_repositoryViews->addTab(
        m_languageResults, QStringLiteral("定义 / 引用"));
    m_languageDiagnostics = new QTreeWidget(m_repositoryViews);
    m_languageDiagnostics->setObjectName(QStringLiteral("agentLanguageDiagnostics"));
    m_languageDiagnostics->setHeaderLabels(
        {QStringLiteral("级别"), QStringLiteral("文件"), QStringLiteral("位置"),
         QStringLiteral("状态"), QStringLiteral("消息")});
    m_languageDiagnostics->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_languageDiagnostics->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_languageDiagnostics->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_languageDiagnostics->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_languageDiagnostics->header()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_languageDiagnostics->header()->resizeSection(1, 140);
    m_languageDiagnostics->setRootIsDecorated(false);
    m_languageDiagnostics->setAlternatingRowColors(true);
    m_languageDiagnostics->setStyleSheet(m_repositorySymbols->styleSheet());
    m_languageDiagnosticsView = m_repositoryViews->addTab(
        m_languageDiagnostics, QStringLiteral("诊断"));
    m_diagnosticRawPreview = new QPlainTextEdit(m_repositoryViews);
    m_diagnosticRawPreview->setObjectName(QStringLiteral("agentDiagnosticRawPreview"));
    m_diagnosticRawPreview->setReadOnly(true);
    m_diagnosticRawPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_diagnosticRawPreview->setPlaceholderText(QStringLiteral("选择诊断原始记录后显示"));
    m_diagnosticRawPreview->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#101828; color:#d0d5dd; border:none; padding:9px;"
        "font-family:Menlo,Consolas,monospace; font-size:10px; }"));
    m_diagnosticRawView = m_repositoryViews->addTab(
        m_diagnosticRawPreview, QStringLiteral("原始记录"));
    structureSplitter->addWidget(m_repositoryViews);

    m_repositoryMapPreview = new QPlainTextEdit(structureSplitter);
    m_repositoryMapPreview->setObjectName(QStringLiteral("agentRepositoryMapPreview"));
    m_repositoryMapPreview->setReadOnly(true);
    m_repositoryMapPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_repositoryMapPreview->setPlaceholderText(QStringLiteral("仓库地图会在索引完成后生成"));
    m_repositoryMapPreview->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#101828; color:#d0d5dd; border:none; padding:9px;"
        "font-family:Menlo,Consolas,monospace; font-size:10px; }"));
    structureSplitter->addWidget(m_repositoryMapPreview);
    structureSplitter->setStretchFactor(0, 3);
    structureSplitter->setStretchFactor(1, 2);
    structureLayout->addWidget(structureSplitter, 1);
    m_repositoryStatus = new QLabel(QStringLiteral("进入结构页后建立项目索引"), structurePage);
    m_repositoryStatus->setObjectName(QStringLiteral("agentRepositoryStatus"));
    m_repositoryStatus->setWordWrap(true);
    m_repositoryStatus->setStyleSheet(QStringLiteral("color:#667085; font-size:10px;"));
    structureLayout->addWidget(m_repositoryStatus);
    connect(m_repositoryRefreshButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::refreshRepositoryIndex);
    connect(m_repositoryCancelButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::cancelRepositoryIndex);
    connect(m_repositoryMapBudget, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        if (m_repositoryIndexLoaded && m_repositoryIndexRequestId.isEmpty()) {
            requestRepositoryMap();
        }
    });
    connect(m_repositorySymbols, &QTreeWidget::itemActivated,
            this, [this](QTreeWidgetItem *item, int) { openRepositorySymbol(item); });
    connect(m_repositorySymbols, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item, int) { openRepositorySymbol(item); });
    connect(m_languageResults, &QTreeWidget::itemActivated,
            this, [this](QTreeWidgetItem *item, int) { openLanguageResult(item); });
    connect(m_languageResults, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item, int) { openLanguageResult(item); });
    connect(m_languageDiagnostics, &QTreeWidget::itemActivated,
            this, [this](QTreeWidgetItem *item, int) { openLanguageResult(item); });
    connect(m_languageDiagnostics, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem *item, int) { openLanguageResult(item); });
    auto *diagnosticContextAction = new QAction(
        QStringLiteral("添加诊断到对话上下文"), m_languageDiagnostics);
    diagnosticContextAction->setObjectName(
        QStringLiteral("agentDiagnosticContextAction"));
    connect(diagnosticContextAction, &QAction::triggered,
            this, &AgentWorkbenchWidget::addDiagnosticContext);
    m_languageDiagnostics->addAction(diagnosticContextAction);
    auto *pinDiagnosticContextAction = new QAction(
        QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")),
        QStringLiteral("固定选中诊断"), m_languageDiagnostics);
    pinDiagnosticContextAction->setObjectName(
        QStringLiteral("agentPinDiagnosticContextTableAction"));
    connect(pinDiagnosticContextAction, &QAction::triggered,
            this, &AgentWorkbenchWidget::pinSelectedDiagnosticContext);
    m_languageDiagnostics->addAction(pinDiagnosticContextAction);
    m_languageDiagnostics->setContextMenuPolicy(Qt::ActionsContextMenu);
    m_structureWorkspaceTab = m_workspaceTabs->addTab(structurePage, QStringLiteral("结构"));

    auto *editorPage = new QWidget(m_workspaceTabs);
    auto *editorLayout = new QVBoxLayout(editorPage);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    m_editorTabs = new QTabBar(editorPage);
    m_editorTabs->setObjectName(QStringLiteral("agentEditorTabs"));
    m_editorTabs->setDocumentMode(true);
    m_editorTabs->setTabsClosable(true);
    m_editorTabs->setMovable(true);
    m_editorTabs->setExpanding(false);
    m_editorTabs->setElideMode(Qt::ElideMiddle);
    m_editorTabs->setUsesScrollButtons(true);
    m_editorTabs->setStyleSheet(QStringLiteral(
        "QTabBar { background:#ffffff; border-bottom:1px solid #e4e7ec; }"
        "QTabBar::tab { background:#f9fafb; color:#667085; border:none;"
        "border-right:1px solid #e4e7ec; padding:8px 12px; min-width:92px; max-width:180px; }"
        "QTabBar::tab:selected { background:#ffffff; color:#101828; font-weight:600;"
        "border-top:2px solid #4B7DFF; }"));
    editorLayout->addWidget(m_editorTabs);
    connect(m_editorTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (m_switchingEditorTab || index < 0) return;
        if (m_editorLoading || !m_editorSaveRequestId.isEmpty()) {
            const int activeIndex = editorTabIndex(m_openEditorPath);
            if (activeIndex >= 0) {
                m_switchingEditorTab = true;
                m_editorTabs->setCurrentIndex(activeIndex);
                m_switchingEditorTab = false;
            }
            return;
        }
        const QString path = m_editorTabs->tabData(index).toString();
        if (!path.isEmpty() && path != m_openEditorPath) {
            storeActiveEditorState();
            activateEditorBuffer(path);
        }
    });
    connect(m_editorTabs, &QTabBar::tabCloseRequested,
            this, &AgentWorkbenchWidget::closeEditorTab);
    connect(m_editorTabs, &QTabBar::tabMoved,
            this, [this](int, int) { saveEditorViewState(); });

    auto *editorHeader = new QWidget(editorPage);
    editorHeader->setObjectName(QStringLiteral("agentEditorHeader"));
    editorHeader->setStyleSheet(QStringLiteral(
        "QWidget#agentEditorHeader { background:#f9fafb; border-bottom:1px solid #e4e7ec; }"));
    auto *editorHeaderLayout = new QHBoxLayout(editorHeader);
    editorHeaderLayout->setContentsMargins(12, 5, 8, 5);
    editorHeaderLayout->setSpacing(7);
    m_recentFilePicker = new QComboBox(editorHeader);
    m_recentFilePicker->setObjectName(QStringLiteral("agentRecentFiles"));
    m_recentFilePicker->setMinimumWidth(112);
    m_recentFilePicker->setMaximumWidth(160);
    m_recentFilePicker->setToolTip(QStringLiteral("最近打开的文件"));
    m_recentFilePicker->setStyleSheet(QStringLiteral(
        "QComboBox { background:#ffffff; color:#475467; border:1px solid #d0d5dd;"
        "border-radius:5px; padding:3px 7px; font-size:9px; }"));
    editorHeaderLayout->addWidget(m_recentFilePicker);
    connect(m_recentFilePicker, QOverload<int>::of(&QComboBox::activated),
            this, [this](int index) {
        const QString path = m_recentFilePicker->itemData(index).toString();
        if (path.isEmpty()) return;
        if (m_editorBuffers.contains(path)) activateEditorBuffer(path);
        else requestEditorFile(path);
    });
    m_editorPath = new QLabel(QStringLiteral("未打开文件"), editorHeader);
    m_editorPath->setObjectName(QStringLiteral("agentEditorPath"));
    m_editorPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_editorPath->setStyleSheet(QStringLiteral(
        "background:transparent; color:#475467; border:none; font-size:10px;"));
    editorHeaderLayout->addWidget(m_editorPath, 1);
    m_editorMeta = new QLabel(QStringLiteral("只读预览"), editorHeader);
    m_editorMeta->setObjectName(QStringLiteral("agentEditorMeta"));
    m_editorMeta->setStyleSheet(QStringLiteral(
        "background:transparent; color:#667085; border:none; font-size:9px;"));
    editorHeaderLayout->addWidget(m_editorMeta);
    m_editorContextButton = new QPushButton(editorHeader);
    m_editorContextButton->setObjectName(QStringLiteral("agentEditorContextButton"));
    m_editorContextButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")));
    m_editorContextButton->setToolTip(QStringLiteral("添加当前编辑器选区到对话上下文"));
    m_editorContextButton->setFixedSize(28, 28);
    m_editorContextButton->setEnabled(false);
    m_editorContextButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:transparent; border:none; border-radius:5px; padding:3px; }"
        "QPushButton:hover { background:#eaecf0; }"
        "QPushButton:disabled { background:transparent; }"));
    connect(m_editorContextButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::addEditorSelectionContext);
    editorHeaderLayout->addWidget(m_editorContextButton);
    m_editorSplitButton = new QPushButton(editorHeader);
    m_editorSplitButton->setObjectName(QStringLiteral("agentEditorSplitButton"));
    m_editorSplitButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/columns-2.svg")));
    m_editorSplitButton->setToolTip(QStringLiteral("拆分编辑器"));
    m_editorSplitButton->setCheckable(true);
    m_editorSplitButton->setEnabled(false);
    m_editorSplitButton->setFixedSize(28, 28);
    m_editorSplitButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:transparent; border:none; border-radius:5px; padding:3px; }"
        "QPushButton:hover { background:#eaecf0; }"
        "QPushButton:checked { background:#E8EFFF; border:1px solid #84A8FF; }"));
    editorHeaderLayout->addWidget(m_editorSplitButton);
    m_editorReloadButton = new QPushButton(editorHeader);
    m_editorReloadButton->setObjectName(QStringLiteral("agentEditorReloadButton"));
    m_editorReloadButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/rotate-ccw.svg")));
    m_editorReloadButton->setToolTip(QStringLiteral("重新载入文件"));
    m_editorReloadButton->setFixedSize(28, 28);
    m_editorReloadButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:transparent; border:none; border-radius:5px; padding:3px; }"
        "QPushButton:hover { background:#eaecf0; }"));
    editorHeaderLayout->addWidget(m_editorReloadButton);
    m_editorSaveButton = new QPushButton(editorHeader);
    m_editorSaveButton->setObjectName(QStringLiteral("agentEditorSaveButton"));
    m_editorSaveButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/save.svg")));
    m_editorSaveButton->setToolTip(QStringLiteral("保存文件"));
    m_editorSaveButton->setFixedSize(28, 28);
    m_editorSaveButton->setStyleSheet(QStringLiteral(
        "QPushButton { background:#165DFF; border:none; border-radius:5px; padding:3px; }"
        "QPushButton:hover { background:#0F46C6; }"
        "QPushButton:disabled { background:#e4e7ec; }"));
    editorHeaderLayout->addWidget(m_editorSaveButton);

    auto *languageBar = new QWidget(editorPage);
    languageBar->setObjectName(QStringLiteral("agentLanguageBar"));
    languageBar->setStyleSheet(QStringLiteral(
        "QWidget#agentLanguageBar { background:#f8fafc; border-bottom:1px solid #e4e7ec; }"));
    auto *languageLayout = new QHBoxLayout(languageBar);
    languageLayout->setContentsMargins(10, 4, 8, 4);
    languageLayout->setSpacing(5);
    m_languageStatus = new QLabel(QStringLiteral("LSP · 打开支持的源码文件后可用"), languageBar);
    m_languageStatus->setObjectName(QStringLiteral("agentLanguageStatus"));
    m_languageStatus->setStyleSheet(QStringLiteral("color:#667085; font-size:9px;"));
    languageLayout->addWidget(m_languageStatus, 1);
    m_languageDefinitionButton = new QPushButton(QStringLiteral("定义"), languageBar);
    m_languageDefinitionButton->setObjectName(QStringLiteral("agentLanguageDefinitionButton"));
    m_languageDefinitionButton->setToolTip(QStringLiteral("转到光标处符号的定义"));
    m_languageReferencesButton = new QPushButton(QStringLiteral("引用"), languageBar);
    m_languageReferencesButton->setObjectName(QStringLiteral("agentLanguageReferencesButton"));
    m_languageReferencesButton->setToolTip(QStringLiteral("查找光标处符号的引用"));
    m_languageDiagnosticsButton = new QPushButton(QStringLiteral("诊断"), languageBar);
    m_languageDiagnosticsButton->setObjectName(QStringLiteral("agentLanguageDiagnosticsButton"));
    m_languageDiagnosticsButton->setToolTip(
        QStringLiteral("重新计算当前文档的语言服务器诊断"));
    m_languageStopButton = new QPushButton(QStringLiteral("停止"), languageBar);
    m_languageStopButton->setObjectName(QStringLiteral("agentLanguageStopButton"));
    m_languageStopButton->setToolTip(QStringLiteral("停止当前语言的本地语言服务器"));
    m_languageRawButton = new QPushButton(QStringLiteral("原始"), languageBar);
    m_languageRawButton->setObjectName(QStringLiteral("agentLanguageRawButton"));
    m_languageRawButton->setToolTip(QStringLiteral("打开当前诊断的原始记录"));
    for (QPushButton *button : {m_languageDefinitionButton, m_languageReferencesButton,
                                m_languageDiagnosticsButton, m_languageRawButton,
                                m_languageStopButton}) {
        button->setFixedHeight(24);
        button->setEnabled(false);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background:#ffffff; color:#344054; border:1px solid #d0d5dd;"
            "border-radius:5px; padding:0 8px; font-size:9px; }"
            "QPushButton:hover { background:#f2f4f7; }"
            "QPushButton:disabled { color:#98a2b3; background:#f8fafc; }"));
        languageLayout->addWidget(button);
    }
    connect(m_languageDefinitionButton, &QPushButton::clicked,
            this, [this]() { requestLanguageAction(QStringLiteral("definition")); });
    connect(m_languageReferencesButton, &QPushButton::clicked,
            this, [this]() { requestLanguageAction(QStringLiteral("references")); });
    connect(m_languageDiagnosticsButton, &QPushButton::clicked,
            this, [this]() { requestLanguageAction(QStringLiteral("diagnostics")); });
    connect(m_languageRawButton, &QPushButton::clicked, this, [this]() {
        if (m_projectId.isEmpty() || m_diagnosticRawReference.isEmpty()
                || !m_diagnosticRawRequestId.isEmpty()) return;
        m_languageRawButton->setEnabled(false);
        m_languageStatus->setText(QStringLiteral("正在读取诊断原始记录…"));
        m_diagnosticRawRequestId = m_runtime->diagnosticRaw(
            m_projectId, m_diagnosticRawReference, m_workspaceRootId);
    });
    connect(m_languageStopButton, &QPushButton::clicked, this, [this]() {
        if (m_projectId.isEmpty() || m_openEditorPath.isEmpty()) return;
        m_languageStatus->setText(QStringLiteral("LSP · 正在停止…"));
        m_languageStopButton->setEnabled(false);
        m_runtime->stopLanguageServer(m_projectId, m_openEditorPath, m_workspaceRootId);
    });

    m_editorSearchBar = new QWidget(editorPage);
    m_editorSearchBar->setObjectName(QStringLiteral("agentEditorSearchBar"));
    m_editorSearchBar->setStyleSheet(QStringLiteral(
        "QWidget#agentEditorSearchBar { background:#ffffff; border-bottom:1px solid #e4e7ec; }"));
    auto *searchLayout = new QGridLayout(m_editorSearchBar);
    searchLayout->setContentsMargins(8, 5, 8, 5);
    searchLayout->setHorizontalSpacing(5);
    searchLayout->setVerticalSpacing(4);
    m_editorFind = new QLineEdit(m_editorSearchBar);
    m_editorFind->setObjectName(QStringLiteral("agentEditorFind"));
    m_editorFind->setPlaceholderText(QStringLiteral("查找"));
    m_editorFind->setClearButtonEnabled(true);
    m_editorFind->setMinimumWidth(0);
    m_editorReplace = new QLineEdit(m_editorSearchBar);
    m_editorReplace->setObjectName(QStringLiteral("agentEditorReplace"));
    m_editorReplace->setPlaceholderText(QStringLiteral("替换为"));
    m_editorReplace->setMinimumWidth(0);
    m_editorFind->setStyleSheet(QStringLiteral(
        "QLineEdit { border:1px solid #d0d5dd; border-radius:5px; padding:4px 7px; font-size:10px; }"));
    m_editorReplace->setStyleSheet(m_editorFind->styleSheet());
    searchLayout->addWidget(m_editorFind, 0, 0);
    m_editorCaseSensitive = new QCheckBox(QStringLiteral("区分大小写"), m_editorSearchBar);
    m_editorCaseSensitive->setStyleSheet(QStringLiteral("font-size:9px; color:#475467;"));
    auto *findPrevious = new QPushButton(m_editorSearchBar);
    findPrevious->setObjectName(QStringLiteral("agentEditorFindPrevious"));
    findPrevious->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    findPrevious->setToolTip(QStringLiteral("上一个匹配项"));
    auto *findNext = new QPushButton(m_editorSearchBar);
    findNext->setObjectName(QStringLiteral("agentEditorFindNext"));
    findNext->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    findNext->setToolTip(QStringLiteral("下一个匹配项"));
    auto *replaceOne = new QPushButton(QStringLiteral("替换"), m_editorSearchBar);
    replaceOne->setObjectName(QStringLiteral("agentEditorReplaceOne"));
    auto *replaceAll = new QPushButton(QStringLiteral("全部替换"), m_editorSearchBar);
    replaceAll->setObjectName(QStringLiteral("agentEditorReplaceAll"));
    auto *closeSearch = new QPushButton(m_editorSearchBar);
    closeSearch->setObjectName(QStringLiteral("agentEditorSearchClose"));
    closeSearch->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    closeSearch->setToolTip(QStringLiteral("关闭查找"));
    for (QPushButton *button : {findPrevious, findNext, replaceOne, replaceAll, closeSearch}) {
        button->setFixedHeight(26);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background:#ffffff; color:#344054; border:1px solid #d0d5dd;"
            "border-radius:5px; padding:2px 7px; font-size:9px; }"
            "QPushButton:hover { background:#f9fafb; }"));
    }
    findPrevious->setFixedWidth(26);
    findNext->setFixedWidth(26);
    closeSearch->setFixedWidth(26);
    searchLayout->addWidget(findPrevious, 0, 1);
    searchLayout->addWidget(findNext, 0, 2);
    m_editorSearchStatus = new QLabel(m_editorSearchBar);
    m_editorSearchStatus->setMinimumWidth(48);
    m_editorSearchStatus->setStyleSheet(QStringLiteral("color:#667085; font-size:9px;"));
    searchLayout->addWidget(m_editorSearchStatus, 0, 3);
    searchLayout->addWidget(closeSearch, 0, 4);
    searchLayout->addWidget(m_editorReplace, 1, 0);
    searchLayout->addWidget(m_editorCaseSensitive, 1, 1, 1, 2);
    searchLayout->addWidget(replaceOne, 1, 3);
    searchLayout->addWidget(replaceAll, 1, 4);
    searchLayout->setColumnStretch(0, 1);
    m_editorSearchBar->hide();

    m_editor = new QPlainTextEdit(editorPage);
    m_editor->setObjectName(QStringLiteral("agentReadOnlyEditor"));
    m_editor->setReadOnly(true);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setPlaceholderText(QStringLiteral("从文件树双击文本文件以只读方式打开"));
    m_editor->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#ffffff; color:#1d2939; border:none;"
        "padding:12px; font-family:Menlo,Consolas,monospace; font-size:11px; }"));
    editorLayout->addWidget(editorHeader);
    editorLayout->addWidget(languageBar);
    editorLayout->addWidget(m_editorSearchBar);
#ifdef AEGISY_HAS_MONACO
    m_editorStack = new QStackedWidget(editorPage);
    m_editorStack->setObjectName(QStringLiteral("agentEditorStack"));
    m_editorStack->addWidget(m_editor);
    editorLayout->addWidget(m_editorStack, 1);
    initializeMonacoEditor(editorPage);
#else
    editorLayout->addWidget(m_editor, 1);
#endif
    connect(m_editor->document(), &QTextDocument::modificationChanged,
            this, [this](bool) { updateEditorActions(); });
    connect(m_editor, &QPlainTextEdit::selectionChanged, this, [this]() {
        if (m_editorBuffers.contains(m_openEditorPath)) {
            EditorBuffer &buffer = m_editorBuffers[m_openEditorPath];
            buffer.cursorPosition = m_editor->textCursor().position();
            buffer.anchorPosition = m_editor->textCursor().anchor();
        }
        updateEditorActions();
    });
    connect(m_editorSaveButton, &QPushButton::clicked, this, [this]() {
#ifdef AEGISY_HAS_MONACO
        if (m_monacoReady && m_editorStack->currentWidget() == m_monacoView) {
            m_monacoBridge->requestContentForSave(m_activeEditorGroup);
            return;
        }
#endif
        saveOpenFile();
    });
    connect(m_editorReloadButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::reloadOpenFile);
    connect(m_editorSplitButton, &QPushButton::toggled,
            this, &AgentWorkbenchWidget::setEditorSplitEnabled);
    connect(m_editorFind, &QLineEdit::returnPressed,
            this, [this]() { findEditorText(false); });
    connect(findPrevious, &QPushButton::clicked,
            this, [this]() { findEditorText(true); });
    connect(findNext, &QPushButton::clicked,
            this, [this]() { findEditorText(false); });
    connect(replaceOne, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::replaceEditorSelection);
    connect(replaceAll, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::replaceAllEditorText);
    connect(closeSearch, &QPushButton::clicked,
            this, [this]() { setEditorSearchVisible(false); });
    auto *saveShortcut = new QShortcut(QKeySequence::Save, m_editor);
    connect(saveShortcut, &QShortcut::activated,
            this, &AgentWorkbenchWidget::saveOpenFile);
    auto *findShortcut = new QShortcut(QKeySequence::Find, m_editor);
    connect(findShortcut, &QShortcut::activated,
            this, [this]() { setEditorSearchVisible(true); });
    auto *replaceShortcut = new QShortcut(QKeySequence::Replace, m_editor);
    connect(replaceShortcut, &QShortcut::activated,
            this, [this]() { setEditorSearchVisible(true, true); });
    auto *definitionShortcut = new QShortcut(QKeySequence(QStringLiteral("F12")), editorPage);
    definitionShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(definitionShortcut, &QShortcut::activated, this, [this]() {
        if (m_languageDefinitionButton->isEnabled()) {
            requestLanguageAction(QStringLiteral("definition"));
        }
    });
    auto *referencesShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Shift+F12")), editorPage);
    referencesShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(referencesShortcut, &QShortcut::activated, this, [this]() {
        if (m_languageReferencesButton->isEnabled()) {
            requestLanguageAction(QStringLiteral("references"));
        }
    });
    auto *diagnosticsShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Shift+M")), editorPage);
    diagnosticsShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(diagnosticsShortcut, &QShortcut::activated, this, [this]() {
        if (m_languageDiagnosticsButton->isEnabled()) {
            requestLanguageAction(QStringLiteral("diagnostics"));
        }
    });
    m_editorSaveButton->setEnabled(false);
    m_editorReloadButton->setEnabled(false);
    m_editorWorkspaceTab = m_workspaceTabs->addTab(editorPage, QStringLiteral("编辑器"));
    m_workspaceEditTab = m_workspaceTabs->addTab(
        buildWorkspaceEditPage(), QStringLiteral("变更"));
    m_terminalWorkspaceTab = m_workspaceTabs->addTab(
        buildTerminalPage(), QStringLiteral("终端"));
    m_gitWorkspaceTab = m_workspaceTabs->addTab(buildGitPage(), QStringLiteral("Git"));
    connect(m_workspaceTabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == m_structureWorkspaceTab && !m_repositoryIndexLoaded
                && m_repositoryIndexRequestId.isEmpty()) {
            refreshRepositoryIndex();
        }
        if (index == m_terminalWorkspaceTab) requestTerminalList();
        if (index == m_gitWorkspaceTab) refreshGitWorkspace();
    });
    layout->addWidget(m_workspaceTabs);
    return canvas;
}

QWidget *AgentWorkbenchWidget::buildGitPage()
{
    auto *page = new QWidget(m_workspaceTabs);
    page->setObjectName(QStringLiteral("agentGitPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto *toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    m_gitSummary = new QLabel(QStringLiteral("未检测到仓库"), page);
    m_gitSummary->setObjectName(QStringLiteral("agentGitSummary"));
    m_gitSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    toolbar->addWidget(m_gitSummary, 1);
    m_gitDiffScope = new QComboBox(page);
    m_gitDiffScope->setObjectName(QStringLiteral("agentGitDiffScope"));
    m_gitDiffScope->addItem(QStringLiteral("工作区"), QStringLiteral("worktree"));
    m_gitDiffScope->addItem(QStringLiteral("暂存区"), QStringLiteral("staged"));
    m_gitDiffScope->addItem(QStringLiteral("选中提交"), QStringLiteral("commit"));
    toolbar->addWidget(m_gitDiffScope);
    m_gitRefreshButton = new QPushButton(page);
    m_gitRefreshButton->setObjectName(QStringLiteral("agentGitRefreshButton"));
    m_gitRefreshButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/rotate-ccw.svg")));
    m_gitRefreshButton->setToolTip(QStringLiteral("刷新 Git"));
    m_gitRefreshButton->setFixedSize(32, 32);
    toolbar->addWidget(m_gitRefreshButton);
    m_gitPinDiffButton = new QPushButton(QStringLiteral("固定差异"), page);
    m_gitPinDiffButton->setObjectName(QStringLiteral("agentGitPinDiffButton"));
    m_gitPinDiffButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")));
    m_gitPinDiffButton->setToolTip(QStringLiteral("固定当前 Git 差异"));
    toolbar->addWidget(m_gitPinDiffButton);
    m_gitPinCommitButton = new QPushButton(QStringLiteral("固定提交"), page);
    m_gitPinCommitButton->setObjectName(QStringLiteral("agentGitPinCommitButton"));
    m_gitPinCommitButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")));
    m_gitPinCommitButton->setToolTip(QStringLiteral("固定历史中选中的提交"));
    toolbar->addWidget(m_gitPinCommitButton);
    layout->addLayout(toolbar);

    auto *splitter = new QSplitter(Qt::Vertical, page);
    splitter->setChildrenCollapsible(false);
    m_gitHistory = new QTreeWidget(splitter);
    m_gitHistory->setObjectName(QStringLiteral("agentGitHistory"));
    m_gitHistory->setHeaderLabels({QStringLiteral("提交"), QStringLiteral("作者"),
                                   QStringLiteral("时间"), QStringLiteral("说明")});
    m_gitHistory->setRootIsDecorated(false);
    m_gitHistory->setAlternatingRowColors(true);
    m_gitHistory->setSelectionMode(QAbstractItemView::SingleSelection);
    m_gitHistory->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_gitHistory->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_gitHistory->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_gitHistory->header()->setSectionResizeMode(3, QHeaderView::Stretch);

    m_gitDiffPreview = new QPlainTextEdit(splitter);
    m_gitDiffPreview->setObjectName(QStringLiteral("agentGitDiffPreview"));
    m_gitDiffPreview->setReadOnly(true);
    m_gitDiffPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_gitDiffPreview->setPlaceholderText(QStringLiteral("无可显示差异"));
    m_gitDiffPreview->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#101828; color:#d0d5dd; border:none;"
        "border-radius:6px; padding:10px; font-family:Menlo,Consolas,monospace;"
        "font-size:10px; }"));
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    layout->addWidget(splitter, 1);

    auto *contextAction = new QAction(QStringLiteral("添加选中差异到对话上下文"),
                                      m_gitDiffPreview);
    contextAction->setObjectName(QStringLiteral("agentGitDiffContextAction"));
    connect(contextAction, &QAction::triggered, this, [this]() {
        addTextExcerptContext(QStringLiteral("git_diff"),
                              QStringLiteral("git-diff-selection"),
                              QStringLiteral("Git Diff"), m_gitDiffPreview);
    });
    m_gitDiffPreview->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_gitDiffPreview, &QPlainTextEdit::customContextMenuRequested, this,
            [this, contextAction](const QPoint &point) {
        QMenu *menu = m_gitDiffPreview->createStandardContextMenu();
        contextAction->setEnabled(m_gitDiffPreview->textCursor().hasSelection());
        menu->addSeparator();
        menu->addAction(contextAction);
        menu->exec(m_gitDiffPreview->mapToGlobal(point));
        delete menu;
    });
    connect(m_gitRefreshButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::refreshGitWorkspace);
    connect(m_gitPinDiffButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::pinCurrentGitDiffContext);
    connect(m_gitPinCommitButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::pinSelectedGitCommitContext);
    connect(m_gitDiffScope, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() {
        updateGitPinControls();
        requestGitDiff();
    });
    connect(m_gitHistory, &QTreeWidget::itemSelectionChanged, this, [this]() {
        QTreeWidgetItem *item = m_gitHistory->currentItem();
        if (!item) return;
        m_selectedGitOid = item->data(0, kGitOidRole).toString();
        const int index = m_gitDiffScope->findData(QStringLiteral("commit"));
        if (index >= 0) m_gitDiffScope->setCurrentIndex(index);
        requestGitDiff();
        updateGitPinControls();
    });
    updateGitPinControls();
    return page;
}

QWidget *AgentWorkbenchWidget::buildWorkspaceEditPage()
{
    auto *page = new QWidget(m_workspaceTabs);
    page->setObjectName(QStringLiteral("agentWorkspaceEditPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    m_workspaceEditSummary = new QLabel(
        QStringLiteral("暂无结构化变更提案"), page);
    m_workspaceEditSummary->setObjectName(QStringLiteral("agentWorkspaceEditSummary"));
    m_workspaceEditSummary->setStyleSheet(QStringLiteral(
        "color:#667085; font-size:11px; font-weight:600;"));
    layout->addWidget(m_workspaceEditSummary);

    auto *splitter = new QSplitter(Qt::Vertical, page);
    splitter->setChildrenCollapsible(false);
    m_workspaceEditFiles = new QTreeWidget(splitter);
    m_workspaceEditFiles->setObjectName(QStringLiteral("agentWorkspaceEditFiles"));
    m_workspaceEditFiles->setColumnCount(4);
    m_workspaceEditFiles->setHeaderLabels({QStringLiteral("文件"), QStringLiteral("操作"),
                                            QStringLiteral("变更"), QStringLiteral("检查")});
    m_workspaceEditFiles->setRootIsDecorated(false);
    m_workspaceEditFiles->setAlternatingRowColors(true);
    m_workspaceEditFiles->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_workspaceEditFiles->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_workspaceEditFiles->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_workspaceEditFiles->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    m_workspaceEditDiff = new QPlainTextEdit(splitter);
    m_workspaceEditDiff->setObjectName(QStringLiteral("agentWorkspaceEditDiff"));
    m_workspaceEditDiff->setReadOnly(true);
    m_workspaceEditDiff->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_workspaceEditDiff->setPlaceholderText(
        QStringLiteral("选择一个变更以审阅权威基线与提议内容之间的差异。"));
    m_workspaceEditDiff->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#101828; color:#d0d5dd; border:none;"
        "border-radius:6px; padding:10px; font-family:Menlo,Consolas,monospace;"
        "font-size:10px; }"));
    splitter->addWidget(m_workspaceEditFiles);
    splitter->addWidget(m_workspaceEditDiff);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(0, 0, 0, 0);
    footer->addStretch();
    m_workspaceEditMoreButton = new QPushButton(QStringLiteral("继续加载"), page);
    m_workspaceEditMoreButton->setObjectName(
        QStringLiteral("agentWorkspaceEditMoreButton"));
    m_workspaceEditMoreButton->setEnabled(false);
    m_workspaceEditMoreButton->setToolTip(QStringLiteral("读取下一页只读差异内容"));
    footer->addWidget(m_workspaceEditMoreButton);
    layout->addLayout(footer);

    connect(m_workspaceEditFiles, &QTreeWidget::itemSelectionChanged, this, [this]() {
        showWorkspaceEditFile(m_workspaceEditFiles->currentItem());
    });
    connect(m_workspaceEditMoreButton, &QPushButton::clicked,
            this, &AgentWorkbenchWidget::loadMoreWorkspaceEditDiff);
    auto *contextAction = new QAction(
        QStringLiteral("添加选中差异到对话上下文"), m_workspaceEditDiff);
    contextAction->setObjectName(QStringLiteral("agentWorkspaceEditContextAction"));
    connect(contextAction, &QAction::triggered, this, [this]() {
        addTextExcerptContext(QStringLiteral("git_diff"),
                              QStringLiteral("workspace-edit-preview"),
                              QStringLiteral("结构化变更"), m_workspaceEditDiff);
    });
    m_workspaceEditDiff->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_workspaceEditDiff, &QPlainTextEdit::customContextMenuRequested,
            this, [this, contextAction](const QPoint &point) {
        QMenu *menu = m_workspaceEditDiff->createStandardContextMenu();
        contextAction->setEnabled(m_workspaceEditDiff->textCursor().hasSelection());
        menu->addSeparator();
        menu->addAction(contextAction);
        menu->exec(m_workspaceEditDiff->mapToGlobal(point));
        delete menu;
    });
    return page;
}

void AgentWorkbenchWidget::populateWorkspaceEditPreview(const QJsonObject &preview)
{
    m_workspaceEditId = preview.value(QStringLiteral("edit_id")).toString();
    const int additions = preview.value(QStringLiteral("additions")).toInt();
    const int deletions = preview.value(QStringLiteral("deletions")).toInt();
    const int warningCount = preview.value(QStringLiteral("warning_count")).toInt();
    const bool applicable = preview.value(QStringLiteral("applicable")).toBool();
    const QJsonArray files = preview.value(QStringLiteral("files")).toArray();
    m_workspaceEditSummary->setText(
        QStringLiteral("%1 个文件 · +%2 -%3 · %4")
            .arg(files.size()).arg(additions).arg(deletions)
            .arg(applicable ? QStringLiteral("基线检查通过")
                            : QStringLiteral("%1 项阻塞警告").arg(warningCount)));
    m_workspaceEditSummary->setStyleSheet(applicable
        ? QStringLiteral("color:#027A48; font-size:11px; font-weight:600;")
        : QStringLiteral("color:#B54708; font-size:11px; font-weight:600;"));
    m_workspaceEditFiles->clear();

    auto addItem = [this](const QString &path, const QString &kind,
                          int itemAdditions, int itemDeletions,
                          const QString &check, const QJsonObject &diff,
                          const QString &toolTip) {
        auto *item = new QTreeWidgetItem(m_workspaceEditFiles);
        item->setText(0, path);
        item->setText(1, kind);
        item->setText(2, QStringLiteral("+%1 -%2").arg(itemAdditions).arg(itemDeletions));
        item->setText(3, check);
        item->setToolTip(3, toolTip);
        item->setData(0, kPatchDiffRole, diff.value(QStringLiteral("inline")).toString());
        item->setData(0, kPatchReferenceRole,
                      diff.value(QStringLiteral("reference")).toString());
        item->setData(0, kPatchInlineTruncatedRole,
                      diff.value(QStringLiteral("inline_truncated")).toBool());
        return item;
    };

    const QJsonObject aggregate = preview.value(QStringLiteral("aggregate_diff")).toObject();
    QTreeWidgetItem *aggregateItem = addItem(
        QStringLiteral("全部变更"), QStringLiteral("汇总"), additions, deletions,
        applicable ? QStringLiteral("通过") : QStringLiteral("需处理"), aggregate,
        QStringLiteral("当前提案的聚合只读差异"));
    for (const QJsonValue &value : files) {
        const QJsonObject file = value.toObject();
        const QString kind = file.value(QStringLiteral("kind")).toString();
        const QJsonArray warnings = file.value(QStringLiteral("warnings")).toArray();
        QStringList warningLabels;
        QStringList warningDetails;
        for (const QJsonValue &warningValue : warnings) {
            const QJsonObject warning = warningValue.toObject();
            warningLabels.append(warning.value(QStringLiteral("code")).toString());
            warningDetails.append(warning.value(QStringLiteral("message")).toString());
        }
        QString path = file.value(QStringLiteral("path")).toString();
        if (kind == QStringLiteral("rename")) {
            path = QStringLiteral("%1 → %2")
                .arg(file.value(QStringLiteral("from_path")).toString(), path);
        }
        addItem(path, kind,
                file.value(QStringLiteral("additions")).toInt(),
                file.value(QStringLiteral("deletions")).toInt(),
                warnings.isEmpty() ? QStringLiteral("通过") : warningLabels.join(", "),
                file.value(QStringLiteral("diff")).toObject(),
                warningDetails.join(QStringLiteral("\n")));
    }
    m_workspaceEditFiles->setCurrentItem(aggregateItem);
    m_workspaceTabs->setCurrentIndex(m_workspaceEditTab);
}

void AgentWorkbenchWidget::showWorkspaceEditFile(QTreeWidgetItem *item)
{
    m_workspaceEditArtifactRequestId.clear();
    if (!item) {
        m_workspaceEditReference.clear();
        m_workspaceEditOffset = 0;
        m_workspaceEditDiff->clear();
        m_workspaceEditMoreButton->setEnabled(false);
        return;
    }
    const QString text = item->data(0, kPatchDiffRole).toString();
    m_workspaceEditReference = item->data(0, kPatchReferenceRole).toString();
    m_workspaceEditOffset = text.toUtf8().size();
    m_workspaceEditDiff->setPlainText(text);
    m_workspaceEditMoreButton->setEnabled(
        item->data(0, kPatchInlineTruncatedRole).toBool()
        && !m_workspaceEditReference.isEmpty());
}

void AgentWorkbenchWidget::loadMoreWorkspaceEditDiff()
{
    if (m_workspaceEditReference.isEmpty() || m_workspaceEditId.isEmpty()
            || m_workSessionId.isEmpty() || m_projectId.isEmpty()
            || !m_workspaceEditArtifactRequestId.isEmpty()) {
        return;
    }
    m_workspaceEditMoreButton->setEnabled(false);
    m_workspaceEditArtifactRequestId = m_runtime->readWorkspaceEditArtifact(
        m_workSessionId, m_projectId, m_workspaceEditId,
        m_workspaceEditReference, m_workspaceEditOffset);
}

QWidget *AgentWorkbenchWidget::buildTerminalPage()
{
    auto *page = new QWidget(m_workspaceTabs);
    page->setObjectName(QStringLiteral("agentTerminalPage"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *toolbar = new QWidget(page);
    toolbar->setObjectName(QStringLiteral("agentTerminalToolbar"));
    toolbar->setStyleSheet(QStringLiteral(
        "QWidget#agentTerminalToolbar { background:#f8fafc; border-bottom:1px solid #e4e7ec; }"));
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(10, 6, 8, 6);
    toolbarLayout->setSpacing(6);
    m_terminalPicker = new QComboBox(toolbar);
    m_terminalPicker->setObjectName(QStringLiteral("agentTerminalPicker"));
    m_terminalPicker->setMinimumWidth(150);
    m_terminalPicker->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_terminalPicker->setPlaceholderText(QStringLiteral("尚未创建终端"));
    toolbarLayout->addWidget(m_terminalPicker, 1);

    m_terminalNewButton = new QPushButton(toolbar);
    m_terminalNewButton->setObjectName(QStringLiteral("agentTerminalNewButton"));
    m_terminalNewButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/plus.svg")));
    m_terminalNewButton->setToolTip(QStringLiteral("新建终端"));
    auto *newMenu = new QMenu(m_terminalNewButton);
    QAction *newForeground = newMenu->addAction(QStringLiteral("新建前台终端"));
    QAction *newBackground = newMenu->addAction(QStringLiteral("新建命名后台终端..."));
    newForeground->setObjectName(QStringLiteral("agentTerminalNewForegroundAction"));
    newBackground->setObjectName(QStringLiteral("agentTerminalNewBackgroundAction"));
    m_terminalNewButton->setMenu(newMenu);
    connect(newForeground, &QAction::triggered, this, [this]() {
        openTerminal(QStringLiteral("foreground"));
    });
    connect(newBackground, &QAction::triggered, this, [this]() {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, QStringLiteral("新建后台终端"), QStringLiteral("终端名称"),
            QLineEdit::Normal, QString(), &accepted).trimmed();
        if (accepted && !name.isEmpty()) openTerminal(QStringLiteral("background"), name);
    });
    m_terminalStopButton = new QPushButton(toolbar);
    m_terminalStopButton->setObjectName(QStringLiteral("agentTerminalStopButton"));
    m_terminalStopButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/x.svg")));
    m_terminalStopButton->setToolTip(QStringLiteral("停止终端进程"));
    m_terminalRestartButton = new QPushButton(toolbar);
    m_terminalRestartButton->setObjectName(QStringLiteral("agentTerminalRestartButton"));
    m_terminalRestartButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/rotate-ccw.svg")));
    m_terminalRestartButton->setToolTip(QStringLiteral("重启终端"));
    m_terminalRemoveButton = new QPushButton(QStringLiteral("移除"), toolbar);
    m_terminalRemoveButton->setObjectName(QStringLiteral("agentTerminalRemoveButton"));
    m_terminalRemoveButton->setToolTip(QStringLiteral("从当前会话移除已退出的终端"));
    m_terminalContextButton = new QPushButton(toolbar);
    m_terminalContextButton->setObjectName(QStringLiteral("agentTerminalContextButton"));
    m_terminalContextButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")));
    m_terminalContextButton->setToolTip(QStringLiteral("将终端选中内容添加到对话上下文"));
    for (QPushButton *button : {m_terminalNewButton, m_terminalStopButton,
                                m_terminalRestartButton, m_terminalContextButton}) {
        button->setFixedSize(28, 28);
    }
    m_terminalRemoveButton->setFixedHeight(28);
    toolbarLayout->addWidget(m_terminalNewButton);
    toolbarLayout->addWidget(m_terminalStopButton);
    toolbarLayout->addWidget(m_terminalRestartButton);
    toolbarLayout->addWidget(m_terminalRemoveButton);
    toolbarLayout->addWidget(m_terminalContextButton);
    layout->addWidget(toolbar);

    m_terminalStatus = new QLabel(QStringLiteral("打开项目并创建 Work 会话后可使用终端"), page);
    m_terminalStatus->setObjectName(QStringLiteral("agentTerminalStatus"));
    m_terminalStatus->setStyleSheet(QStringLiteral(
        "background:#ffffff; color:#667085; border-bottom:1px solid #e4e7ec; padding:5px 10px; font-size:9px;"));
    layout->addWidget(m_terminalStatus);

#ifdef AEGISY_HAS_MONACO
    m_terminalStack = new QStackedWidget(page);
    m_terminalStack->setObjectName(QStringLiteral("agentTerminalStack"));
    m_terminalExcerptPreview = new QPlainTextEdit(m_terminalStack);
#else
    m_terminalExcerptPreview = new QPlainTextEdit(page);
#endif
    m_terminalExcerptPreview->setObjectName(QStringLiteral("agentTerminalExcerptPreview"));
    m_terminalExcerptPreview->setReadOnly(true);
    m_terminalExcerptPreview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_terminalExcerptPreview->setMaximumBlockCount(10000);
    m_terminalExcerptPreview->setPlaceholderText(
        QStringLiteral("WebEngine 不可用时，终端输出会显示在这里。"));
    m_terminalExcerptPreview->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background:#101828; color:#d0d5dd; border:none; padding:10px;"
        "font-family:Menlo,Consolas,monospace; font-size:10px; }"));
    connect(m_terminalExcerptPreview, &QPlainTextEdit::selectionChanged,
            this, &AgentWorkbenchWidget::updateTerminalControls);
    m_terminalSelectionContextAction = new QAction(
        QStringLiteral("添加选中内容到对话上下文"), m_terminalExcerptPreview);
    m_terminalSelectionContextAction->setObjectName(
        QStringLiteral("agentTerminalExcerptContextAction"));
    connect(m_terminalSelectionContextAction, &QAction::triggered,
            this, &AgentWorkbenchWidget::addTerminalSelectionContext);
    m_pinTerminalExcerptAction = new QAction(
        QIcon(QStringLiteral(":/icons/lucide/paperclip.svg")),
        QStringLiteral("固定最近输出"), m_terminalExcerptPreview);
    m_pinTerminalExcerptAction->setObjectName(
        QStringLiteral("agentPinTerminalExcerptAction"));
    connect(m_pinTerminalExcerptAction, &QAction::triggered,
            this, &AgentWorkbenchWidget::pinRecentTerminalExcerpt);
    auto *terminalContextMenu = new QMenu(m_terminalContextButton);
    terminalContextMenu->addAction(m_terminalSelectionContextAction);
    terminalContextMenu->addAction(m_pinTerminalExcerptAction);
    m_terminalContextButton->setMenu(terminalContextMenu);
    m_terminalContextButton->setToolTip(QStringLiteral("添加或固定终端上下文"));
    m_terminalExcerptPreview->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_terminalExcerptPreview, &QPlainTextEdit::customContextMenuRequested,
            this, [this](const QPoint &point) {
        QMenu *menu = m_terminalExcerptPreview->createStandardContextMenu();
        updateTerminalControls();
        menu->addSeparator();
        menu->addAction(m_terminalSelectionContextAction);
        menu->addAction(m_pinTerminalExcerptAction);
        menu->exec(m_terminalExcerptPreview->mapToGlobal(point));
        delete menu;
    });
#ifdef AEGISY_HAS_MONACO
    m_terminalStack->addWidget(m_terminalExcerptPreview);
    initializeTerminalWeb(m_terminalStack);
    layout->addWidget(m_terminalStack, 1);
#else
    layout->addWidget(m_terminalExcerptPreview, 1);
#endif

    connect(m_terminalPicker, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index < 0) return;
        activateTerminal(m_terminalPicker->itemData(index).toString());
    });
    connect(m_terminalStopButton, &QPushButton::clicked, this, [this]() {
        if (!m_activeTerminalId.isEmpty() && !m_workSessionId.isEmpty()) {
            m_runtime->stopUserTerminal(m_workSessionId, m_activeTerminalId);
            m_terminalStatus->setText(QStringLiteral("正在停止终端..."));
        }
    });
    connect(m_terminalRestartButton, &QPushButton::clicked, this, [this]() {
        if (!m_activeTerminalId.isEmpty() && !m_workSessionId.isEmpty()) {
            m_runtime->restartUserTerminal(m_workSessionId, m_activeTerminalId);
            m_terminalStatus->setText(QStringLiteral("正在重启终端..."));
        }
    });
    connect(m_terminalRemoveButton, &QPushButton::clicked, this, [this]() {
        if (!m_activeTerminalId.isEmpty() && !m_workSessionId.isEmpty()) {
            m_runtime->removeUserTerminal(m_workSessionId, m_activeTerminalId);
        }
    });
    updateTerminalControls();
    return page;
}

void AgentWorkbenchWidget::requestTerminalList()
{
    if (m_workSessionId.isEmpty() || !m_runtime->isReady()
            || m_archivedSessionIds.contains(m_workSessionId)
            || m_recoverySessionIds.contains(m_workSessionId)
            || m_runtimeRecoveryMode
            || !m_terminalListRequestId.isEmpty()) return;
    m_terminalListRequestId = m_runtime->listTerminals(m_workSessionId);
}

void AgentWorkbenchWidget::openTerminal(const QString &kind, const QString &name)
{
    if (m_runtimeRecoveryMode
            || (!m_workSessionId.isEmpty()
                && m_recoverySessionIds.contains(m_workSessionId))) {
        addNotice(QStringLiteral("只读恢复会话不能启动或重启终端。"), true);
        return;
    }
    if (m_projectId.isEmpty()) {
        addNotice(QStringLiteral("请先打开项目，再创建终端。"), true);
        return;
    }
    if (!m_runtime->isReady()) {
        addNotice(QStringLiteral("本地运行时尚未就绪。"), true);
        return;
    }
    if (!m_workSessionId.isEmpty() && m_archivedSessionIds.contains(m_workSessionId)) {
        addNotice(QStringLiteral("该 Work 会话已归档，请先恢复或新建会话。"), true);
        return;
    }
    if (m_workSessionId.isEmpty()) {
        if (!m_pendingTerminalKind.isEmpty()) return;
        m_pendingTerminalKind = kind;
        m_pendingTerminalName = name;
        m_terminalStatus->setText(QStringLiteral("正在创建 Work 会话..."));
        m_runtime->startSession(QStringLiteral("work"), m_projectId);
        return;
    }
    m_terminalStatus->setText(QStringLiteral("正在创建终端..."));
    m_runtime->openUserTerminal(m_workSessionId, kind, name);
}

void AgentWorkbenchWidget::activateTerminal(const QString &terminalId)
{
    if (m_terminalPollTimer) m_terminalPollTimer->stop();
    m_activeTerminalId = terminalId;
    m_terminalAttachRequestId.clear();
    m_terminalOutputOffset = 0;
    m_terminalGeneration = 0;
    m_terminalRunning = false;
    m_terminalStopping = false;
    m_terminalSelection.clear();
    if (m_terminalExcerptPreview) m_terminalExcerptPreview->clear();
#ifdef AEGISY_HAS_MONACO
    if (m_terminalBridge && m_terminalWebReady) {
        m_terminalBridge->resetTerminal(0);
        m_terminalBridge->setInputEnabled(false);
    }
#endif
    updateTerminalControls();
    if (terminalId.isEmpty() || m_workSessionId.isEmpty() || !m_runtime->isReady()) {
        if (m_terminalStatus) {
            m_terminalStatus->setText(QStringLiteral("尚未选择终端"));
        }
        return;
    }
    m_terminalStatus->setText(QStringLiteral("正在连接终端..."));
    m_terminalAttachRequestId = m_runtime->attachTerminal(
        m_workSessionId, terminalId, 0);
}

void AgentWorkbenchWidget::applyTerminalSnapshot(const QJsonObject &terminal,
                                                 bool resetOutput)
{
    const QString terminalId = terminal.value(QStringLiteral("terminal_id")).toString();
    const QString sessionId = terminal.value(QStringLiteral("session_id")).toString();
    if (terminalId.isEmpty() || terminalId != m_activeTerminalId
            || sessionId != m_workSessionId) return;
    const quint64 generation = terminal.value(QStringLiteral("generation"))
        .toVariant().toULongLong();
    if (resetOutput || generation != m_terminalGeneration) {
        m_terminalGeneration = generation;
        m_terminalOutputOffset = 0;
        m_terminalSelection.clear();
        m_terminalExcerptPreview->clear();
#ifdef AEGISY_HAS_MONACO
        if (m_terminalBridge && m_terminalWebReady) {
            m_terminalBridge->resetTerminal(generation);
        }
#endif
    }
    const quint64 outputStart = terminal.value(QStringLiteral("output_start"))
        .toVariant().toULongLong();
    const quint64 outputEnd = terminal.value(QStringLiteral("output_end"))
        .toVariant().toULongLong();
    if (outputStart > m_terminalOutputOffset) {
        const QString marker = QStringLiteral("\r\n[Aegisy: earlier terminal output was omitted]\r\n");
        m_terminalExcerptPreview->moveCursor(QTextCursor::End);
        m_terminalExcerptPreview->insertPlainText(marker);
#ifdef AEGISY_HAS_MONACO
        if (m_terminalBridge && m_terminalWebReady) {
            m_terminalBridge->writeOutput(
                QString::fromLatin1(marker.toUtf8().toBase64()));
        }
#endif
    }
    const QString outputBase64 = terminal.value(QStringLiteral("output_base64")).toString();
    if (!outputBase64.isEmpty()) {
        const QByteArray bytes = QByteArray::fromBase64(outputBase64.toLatin1());
        QString fallback = QString::fromUtf8(bytes);
        fallback.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        fallback.remove(QLatin1Char('\r'));
        fallback.remove(QRegularExpression(
            QStringLiteral("\\x1B\\[[0-?]*[ -/]*[@-~]")));
        m_terminalExcerptPreview->moveCursor(QTextCursor::End);
        m_terminalExcerptPreview->insertPlainText(fallback);
        m_terminalExcerptPreview->moveCursor(QTextCursor::End);
#ifdef AEGISY_HAS_MONACO
        if (m_terminalBridge && m_terminalWebReady) {
            m_terminalBridge->writeOutput(outputBase64);
        }
#endif
    }
    m_terminalOutputOffset = outputEnd;
    m_terminalRunning = terminal.value(QStringLiteral("running")).toBool();
    const QString name = terminal.value(QStringLiteral("name")).toString();
    const QString kind = terminal.value(QStringLiteral("kind")).toString();
    const QString state = terminal.value(QStringLiteral("state")).toString();
    m_terminalStopping = state == QStringLiteral("stopping");
    const QString shell = terminal.value(QStringLiteral("shell")).toString();
    m_terminalStatus->setText(QStringLiteral("%1 · %2 · %3 · %4 · 第 %5 代")
        .arg(name,
             kind == QStringLiteral("background") ? QStringLiteral("后台")
                                                    : QStringLiteral("前台"),
             shell, state)
        .arg(generation));
#ifdef AEGISY_HAS_MONACO
    if (m_terminalBridge && m_terminalWebReady) {
        m_terminalBridge->setInputEnabled(m_terminalRunning && !m_terminalStopping);
    }
#endif
    updateTerminalControls();
    if (m_terminalRunning && m_terminalPollTimer) m_terminalPollTimer->start();
}

void AgentWorkbenchWidget::pollActiveTerminal()
{
    if (m_activeTerminalId.isEmpty() || m_workSessionId.isEmpty()
            || !m_terminalRunning || !m_runtime->isReady()
            || !m_terminalAttachRequestId.isEmpty()) return;
    m_terminalAttachRequestId = m_runtime->attachTerminal(
        m_workSessionId, m_activeTerminalId, m_terminalOutputOffset);
}

void AgentWorkbenchWidget::updateTerminalControls()
{
    const bool protocolReady = m_runtime && m_runtime->isReady() && !m_runtimeRecoveryMode
        && !m_projectId.isEmpty();
    const bool ready = protocolReady
        && (m_workSessionId.isEmpty()
            || (!m_archivedSessionIds.contains(m_workSessionId)
                && !m_recoverySessionIds.contains(m_workSessionId)
                && !m_pendingDeletionSessionIds.contains(m_workSessionId)));
    const bool selected = !m_activeTerminalId.isEmpty();
    if (m_terminalNewButton) m_terminalNewButton->setEnabled(ready);
    if (m_terminalStopButton) {
        m_terminalStopButton->setEnabled(
            protocolReady && selected && m_terminalRunning && !m_terminalStopping);
    }
    if (m_terminalRestartButton) m_terminalRestartButton->setEnabled(ready && selected);
    if (m_terminalRemoveButton) {
        m_terminalRemoveButton->setEnabled(protocolReady && selected && !m_terminalRunning);
    }
    if (m_terminalContextButton) {
        const bool nativeSelection = m_terminalExcerptPreview
            && m_terminalExcerptPreview->textCursor().hasSelection();
        const bool hasSelection = !m_terminalSelection.isEmpty() || nativeSelection;
        const bool canPin = ready && selected && m_terminalOutputOffset > 0
            && !m_workSessionId.isEmpty() && m_pinnedContextAvailable
            && !currentOperationStatusBlocked()
            && m_pinnedContextMutationRequestId.isEmpty()
            && m_terminalExcerptRequestId.isEmpty();
        m_terminalContextButton->setEnabled(hasSelection || canPin);
        if (m_terminalSelectionContextAction) {
            m_terminalSelectionContextAction->setEnabled(hasSelection);
        }
        if (m_pinTerminalExcerptAction) {
            m_pinTerminalExcerptAction->setEnabled(canPin);
        }
    }
}

void AgentWorkbenchWidget::addTerminalSelectionContext()
{
    QString selected = m_terminalSelection;
    if (selected.isEmpty() && m_terminalExcerptPreview
            && m_terminalExcerptPreview->textCursor().hasSelection()) {
        selected = m_terminalExcerptPreview->textCursor().selectedText();
        selected.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    }
    if (selected.isEmpty()) {
        addNotice(QStringLiteral("请先在终端中选择要添加的输出。"), true);
        return;
    }
    bool truncated = false;
    selected = boundedContextText(selected, &truncated);
    const QString name = m_terminalPicker && m_terminalPicker->currentIndex() >= 0
        ? m_terminalPicker->currentText() : QStringLiteral("终端");
    addContextItem({
        {QStringLiteral("kind"), QStringLiteral("terminal_excerpt")},
        {QStringLiteral("label"), name},
        {QStringLiteral("origin"), QStringLiteral("terminal-selection")},
        {QStringLiteral("content"), selected},
        {QStringLiteral("truncated"), truncated},
        {QStringLiteral("size"), selected.toUtf8().size()},
    });
}

void AgentWorkbenchWidget::pinRecentTerminalExcerpt()
{
    if (!m_pinnedContextAvailable || m_runtimeRecoveryMode || m_projectId.isEmpty()
            || m_workSessionId.isEmpty() || m_activeTerminalId.isEmpty()) {
        addNotice(QStringLiteral("当前终端不能固定输出。"), true);
        return;
    }
    if (currentSessionRecoveryRequired() || currentSessionDeletionPending()
            || currentOperationStatusBlocked()) {
        addNotice(QStringLiteral("当前会话状态不允许更新固定上下文。"), true);
        return;
    }
    if (!m_pinnedContextMutationRequestId.isEmpty()
            || !m_terminalExcerptRequestId.isEmpty()) {
        addNotice(QStringLiteral("固定上下文或终端摘录正在更新，请稍候。"));
        return;
    }
    if (m_terminalOutputOffset == 0) {
        addNotice(QStringLiteral("当前终端还没有可固定的输出。"), true);
        return;
    }
    m_terminalExcerptRequestId = m_runtime->readTerminalExcerpt(
        m_workSessionId, m_activeTerminalId, 16 * 1024);
    if (m_terminalExcerptRequestId.isEmpty()) {
        addNotice(QStringLiteral("无法读取待固定的终端摘录。"), true);
    }
    updateTerminalControls();
}

void AgentWorkbenchWidget::finishPinnedTerminalExcerpt(const QJsonObject &excerpt)
{
    const QString sessionId = excerpt.value(QStringLiteral("session_id")).toString();
    const QString projectId = excerpt.value(QStringLiteral("project_id")).toString();
    const QString rootId = excerpt.value(QStringLiteral("root_id")).toString();
    const QString terminalId = excerpt.value(QStringLiteral("terminal_id")).toString();
    const QString reference = excerpt.value(QStringLiteral("reference")).toString();
    const QString contentType = excerpt.value(QStringLiteral("content_type")).toString();
    const QString contentHash = excerpt.value(QStringLiteral("content_hash")).toString();
    const QByteArray content = excerpt.value(QStringLiteral("content")).toString().toUtf8();
    const quint64 generation = excerpt.value(QStringLiteral("generation"))
        .toVariant().toULongLong();
    const quint64 outputStart = excerpt.value(QStringLiteral("output_start"))
        .toVariant().toULongLong();
    const quint64 outputEnd = excerpt.value(QStringLiteral("output_end"))
        .toVariant().toULongLong();
    const quint64 sourceOutputEnd = excerpt.value(QStringLiteral("source_output_end"))
        .toVariant().toULongLong();
    static const QRegularExpression referencePattern(QStringLiteral(
        "^terminal-excerpt:(terminal-[A-Za-z0-9-]+):([1-9][0-9]*):(0|[1-9][0-9]*):([1-9][0-9]*)$"));
    const QRegularExpressionMatch match = referencePattern.match(reference);
    const QString digest = contentHash.startsWith(QStringLiteral("sha256:"))
        ? contentHash.mid(7) : QString();
    const auto isLowerHex = [](const QString &value) {
        return value.size() == 64
            && std::all_of(value.cbegin(), value.cend(), [](QChar character) {
                return (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
                    || (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
            });
    };
    if (excerpt.value(QStringLiteral("schema_version")).toString()
                != QStringLiteral("terminal-excerpt/0.1")
            || sessionId != m_workSessionId || projectId != m_projectId
            || rootId != QStringLiteral("root-1") || terminalId != m_activeTerminalId
            || contentType != QStringLiteral("text/plain; charset=utf-8")
            || excerpt.value(QStringLiteral("content_bodies_persisted")).toBool(true)
            || content.isEmpty() || content.size() > 16 * 1024
            || excerpt.value(QStringLiteral("bytes")).toVariant().toLongLong() != content.size()
            || !isLowerHex(digest)
            || QString::fromLatin1(QCryptographicHash::hash(
                   content, QCryptographicHash::Sha256).toHex()) != digest
            || !match.hasMatch() || match.captured(1) != terminalId
            || match.captured(2).toULongLong() != generation
            || match.captured(3).toULongLong() != outputStart
            || match.captured(4).toULongLong() != outputEnd
            || generation == 0 || outputStart >= outputEnd
            || outputEnd - outputStart > 16 * 1024 || sourceOutputEnd < outputEnd
            || !m_pinnedContextMutationRequestId.isEmpty()) {
        addNotice(QStringLiteral("终端摘录身份校验失败，未写入固定上下文。"), true);
        return;
    }
    const QByteArray idDigest = QCryptographicHash::hash(
        QStringLiteral("%1\n%2\n%3\n%4")
            .arg(projectId, sessionId, terminalId, reference).toUtf8(),
        QCryptographicHash::Sha256).toHex();
    const QString id = QStringLiteral("pin-terminal-%1")
        .arg(QString::fromLatin1(idDigest));
    const QJsonObject descriptor{
        {QStringLiteral("id"), id},
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("root_id"), rootId},
        {QStringLiteral("kind"), QStringLiteral("terminal_excerpt")},
        {QStringLiteral("source"), QStringLiteral("terminal-output")},
        {QStringLiteral("label"), QStringLiteral("终端摘录")},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("content_hash"), contentHash},
        {QStringLiteral("bytes"), static_cast<double>(content.size())},
        {QStringLiteral("revision"), QStringLiteral("terminal-generation:%1")
            .arg(generation)},
        {QStringLiteral("freshness"), QStringLiteral("fresh")},
        {QStringLiteral("priority"), 680},
        {QStringLiteral("metadata"), QJsonObject{
            {QStringLiteral("terminal_id"), terminalId},
            {QStringLiteral("generation"), QString::number(generation)},
            {QStringLiteral("output_start"), QString::number(outputStart)},
            {QStringLiteral("output_end"), QString::number(outputEnd)},
        }},
    };
    QJsonArray items;
    bool replaced = false;
    for (QJsonObject existing : m_pinnedContextItems) {
        existing.remove(QStringLiteral("included"));
        if (existing.value(QStringLiteral("id")).toString() == id) {
            items.append(descriptor);
            replaced = true;
        } else {
            items.append(existing);
        }
    }
    if (!replaced) {
        if (items.size() >= 128) {
            addNotice(QStringLiteral("固定上下文已达到 128 项上限。"), true);
            return;
        }
        items.append(descriptor);
    }
    m_pendingPinnedIncludeId = id;
    m_pinnedContextMutationRequestId = m_runtime->savePinnedContext(
        projectId, items, m_pinnedContextSetIdentity);
    if (m_pinnedContextMutationRequestId.isEmpty()) {
        m_pendingPinnedIncludeId.clear();
        addNotice(QStringLiteral("无法提交终端固定上下文更新。"), true);
    }
}

void AgentWorkbenchWidget::populateDirectory(const QJsonObject &listing)
{
    const QString parentPath = listing.value(QStringLiteral("path")).toString();
    QTreeWidgetItem *parent = parentPath.isEmpty() ? nullptr : m_treeItems.value(parentPath, nullptr);
    if (!parentPath.isEmpty() && !parent) return;
    QList<QTreeWidgetItem *> previousItems;
    if (parent) {
        previousItems = parent->takeChildren();
    } else {
        while (m_fileTree->topLevelItemCount() > 0) {
            previousItems.append(m_fileTree->takeTopLevelItem(0));
        }
    }
    QHash<QString, QTreeWidgetItem *> reusableItems;
    for (QTreeWidgetItem *item : previousItems) {
        reusableItems.insert(item->data(0, kPathRole).toString(), item);
    }

    const QJsonArray entries = listing.value(QStringLiteral("entries")).toArray();
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const QString path = entry.value(QStringLiteral("path")).toString();
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        QTreeWidgetItem *item = reusableItems.take(path);
        if (item && item->data(0, kKindRole).toString() != kind) {
            removeTreeItemMappings(item);
            delete item;
            item = nullptr;
        }
        const bool created = !item;
        if (!item) item = new QTreeWidgetItem();
        item->setText(0, entry.value(QStringLiteral("name")).toString());
        item->setText(1, QString());
        item->setText(2, QString());
        item->setForeground(0, QBrush());
        if (kind == QStringLiteral("file")) {
            item->setText(1, QString::number(
                entry.value(QStringLiteral("size")).toVariant().toLongLong()));
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        } else if (kind == QStringLiteral("directory")) {
            item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            if (created) new QTreeWidgetItem(item, QStringList{QStringLiteral("加载中…")});
        } else {
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileLinkIcon));
            item->setForeground(0, QColor(QStringLiteral("#98a2b3")));
        }
        item->setData(0, kPathRole, path);
        item->setData(0, kKindRole, kind);
        if (created) item->setData(0, kLoadedRole, false);
        item->setData(0, kRevisionRole,
                      entry.value(QStringLiteral("revision")).toString());
        if (parent) parent->addChild(item);
        else m_fileTree->addTopLevelItem(item);
        m_treeItems.insert(path, item);
    }
    for (QTreeWidgetItem *item : reusableItems) {
        removeTreeItemMappings(item);
        delete item;
    }
    const int ignored = listing.value(QStringLiteral("ignored_count")).toInt();
    const bool truncated = listing.value(QStringLiteral("truncated")).toBool();
    m_directoryStatus = QStringLiteral("%1 个项目%2%3 · 懒加载 · 只读")
        .arg(entries.size())
        .arg(ignored ? QStringLiteral(" · %1 个已按策略隐藏").arg(ignored) : QString())
        .arg(truncated ? QStringLiteral(" · 结果已截断") : QString());
    m_fileStatus->setText(m_directoryStatus);
    applyGitDecorations();
    applyFileFilter();
}

void AgentWorkbenchWidget::removeTreeItemMappings(QTreeWidgetItem *item)
{
    if (!item) return;
    for (int index = 0; index < item->childCount(); ++index) {
        removeTreeItemMappings(item->child(index));
    }
    const QString path = item->data(0, kPathRole).toString();
    if (!path.isEmpty()) m_treeItems.remove(path);
}

void AgentWorkbenchWidget::requestDirectoryListing(const QString &directory)
{
    if (m_projectId.isEmpty() || m_workspaceListRequests.values().contains(directory)) return;
    const QString requestId = m_runtime->listWorkspace(m_projectId, directory,
                                                       m_workspaceRootId);
    if (!requestId.isEmpty()) m_workspaceListRequests.insert(requestId, directory);
}

void AgentWorkbenchWidget::markDirectoryUnavailable(const QString &directory,
                                                    const QString &message)
{
    if (directory.isEmpty()) {
        m_fileStatus->setText(QStringLiteral("项目根目录不可用：%1").arg(message));
        return;
    }
    QTreeWidgetItem *item = m_treeItems.value(directory, nullptr);
    if (!item) return;
    qDeleteAll(item->takeChildren());
    item->setData(0, kKindRole, QStringLiteral("unavailable"));
    item->setData(0, kLoadedRole, false);
    item->setText(1, QStringLiteral("—"));
    item->setText(2, QStringLiteral("不可用"));
    item->setForeground(0, QColor(QStringLiteral("#b42318")));
    item->setForeground(2, QColor(QStringLiteral("#b42318")));
    item->setToolTip(0, message);
}

void AgentWorkbenchWidget::updateWorkspaceWatch(const QString &directory)
{
    if (m_projectId.isEmpty() || m_watchedDirectories.contains(directory)) return;
    m_watchedDirectories.insert(directory);
    QStringList paths = m_watchedDirectories.values();
    paths.sort(Qt::CaseInsensitive);
    m_workspaceWatchTimer->stop();
    m_runtime->watchWorkspace(m_projectId, paths, m_workspaceWatchId,
                              m_workspaceRootId);
}

void AgentWorkbenchWidget::applyFileFilter()
{
    if (!m_fileTree || !m_fileFilter) return;
    const QString query = m_fileFilter->text().trimmed();
    for (int index = 0; index < m_fileTree->topLevelItemCount(); ++index) {
        filterTreeItem(m_fileTree->topLevelItem(index), query);
    }
    if (!query.isEmpty()) {
        m_fileStatus->setText(QStringLiteral("正在筛选已加载目录 · 清空筛选可恢复完整树"));
    } else if (!m_directoryStatus.isEmpty()) {
        m_fileStatus->setText(m_directoryStatus);
    }
}

bool AgentWorkbenchWidget::filterTreeItem(QTreeWidgetItem *item, const QString &query)
{
    if (!item) return false;
    bool childMatches = false;
    for (int index = 0; index < item->childCount(); ++index) {
        QTreeWidgetItem *child = item->child(index);
        if (child->data(0, kPathRole).toString().isEmpty()) continue;
        childMatches = filterTreeItem(child, query) || childMatches;
    }
    const bool selfMatches = query.isEmpty()
        || item->text(0).contains(query, Qt::CaseInsensitive)
        || item->data(0, kPathRole).toString().contains(query, Qt::CaseInsensitive);
    const bool visible = selfMatches || childMatches;
    item->setHidden(!visible);
    if (!query.isEmpty() && childMatches) item->setExpanded(true);
    return visible;
}

void AgentWorkbenchWidget::refreshGitStatus()
{
    if (m_projectId.isEmpty() || !m_runtime->isReady() || m_gitStatusPending) return;
    m_gitStatusPending = true;
    m_runtime->workspaceGitStatus(m_projectId);
}

void AgentWorkbenchWidget::refreshGitWorkspace()
{
    if (m_projectId.isEmpty() || !m_runtime->isReady()) return;
    if (m_gitOverviewRequestId.isEmpty()) {
        m_gitOverviewRequestId = m_runtime->gitOverview(m_projectId);
    }
    if (m_gitLogRequestId.isEmpty()) {
        m_gitLogRequestId = m_runtime->gitLog(m_projectId, 100);
    }
    requestGitDiff();
}

void AgentWorkbenchWidget::requestGitDiff()
{
    if (m_projectId.isEmpty() || !m_runtime->isReady() || !m_gitDiffScope
            || !m_gitDiffRequestId.isEmpty()) return;
    const QString scope = m_gitDiffScope->currentData().toString();
    if (scope == QStringLiteral("commit") && m_selectedGitOid.isEmpty()) {
        if (m_gitDiffPreview) m_gitDiffPreview->clear();
        updateGitPinControls();
        return;
    }
    m_gitDiffRequestId = m_runtime->gitDiff(
        m_projectId, scope, scope == QStringLiteral("commit") ? m_selectedGitOid : QString());
    if (!m_gitDiffRequestId.isEmpty()) {
        m_gitDiffRequestedScope = scope;
        m_gitDiffRequestedOid = scope == QStringLiteral("commit") ? m_selectedGitOid : QString();
    }
    updateGitPinControls();
}

void AgentWorkbenchWidget::populateGitOverview(const QJsonObject &overview)
{
    const QJsonArray branches = overview.value(QStringLiteral("branches")).toArray();
    QString currentBranch = QStringLiteral("detached");
    for (const QJsonValue &value : branches) {
        const QJsonObject branch = value.toObject();
        if (branch.value(QStringLiteral("current")).toBool()) {
            m_gitCurrentBranch = branch.value(QStringLiteral("name")).toString();
            currentBranch = m_gitCurrentBranch;
            const QString upstream = branch.value(QStringLiteral("upstream")).toString();
            if (!upstream.isEmpty()) currentBranch += QStringLiteral(" → %1").arg(upstream);
            break;
        }
    }
    if (m_gitCurrentBranch.isEmpty()) m_gitCurrentBranch = QStringLiteral("detached");
    const int tags = overview.value(QStringLiteral("tags")).toArray().size();
    const int remotes = overview.value(QStringLiteral("remotes")).toArray().size();
    const int worktrees = overview.value(QStringLiteral("worktrees")).toArray().size();
    m_gitSummary->setText(QStringLiteral("%1 · %2 tags · %3 remotes · %4 worktrees")
        .arg(currentBranch).arg(tags).arg(remotes).arg(worktrees));
    updateContextStrip();
}

void AgentWorkbenchWidget::populateGitLog(const QJsonObject &log)
{
    m_gitHistory->clear();
    m_selectedGitOid.clear();
    for (const QJsonValue &value : log.value(QStringLiteral("commits")).toArray()) {
        const QJsonObject commit = value.toObject();
        const QString oid = commit.value(QStringLiteral("oid")).toString();
        const qint64 authoredAt = commit.value(QStringLiteral("author_time"))
            .toVariant().toLongLong();
        auto *item = new QTreeWidgetItem(m_gitHistory, {
            oid.left(8),
            commit.value(QStringLiteral("author_name")).toString(),
            QDateTime::fromSecsSinceEpoch(authoredAt).toLocalTime()
                .toString(QStringLiteral("yyyy-MM-dd HH:mm")),
            commit.value(QStringLiteral("subject")).toString(),
        });
        item->setData(0, kGitOidRole, oid);
        item->setToolTip(0, oid);
    }
    updateGitPinControls();
}

void AgentWorkbenchWidget::populateGitDiff(const QJsonObject &diff)
{
    const QString patch = diff.value(QStringLiteral("patch")).toString();
    m_gitDiffPreview->setPlainText(patch);
    const int additions = diff.value(QStringLiteral("additions")).toInt();
    const int deletions = diff.value(QStringLiteral("deletions")).toInt();
    m_gitDiffPreview->setToolTip(QStringLiteral("+%1 / -%2").arg(additions).arg(deletions));
    updateGitPinControls();
}

void AgentWorkbenchWidget::updateGitPinControls()
{
    if (!m_gitPinDiffButton || !m_gitPinCommitButton) return;
    const bool blocked = !m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode
        || currentSessionRecoveryRequired() || currentSessionDeletionPending()
        || currentOperationStatusBlocked() || m_projectId.isEmpty()
        || !m_pinnedContextAvailable || !m_gitContextAvailable
        || !m_gitContextRequestId.isEmpty()
        || !m_pinnedContextMutationRequestId.isEmpty();
    const QString scope = m_gitDiffScope ? m_gitDiffScope->currentData().toString() : QString();
    const bool validSelectedOid = isLowerHex(m_selectedGitOid, 40);
    const bool diffReady = m_gitDiffPreview && !m_gitDiffPreview->toPlainText().isEmpty()
        && m_gitDiffRequestId.isEmpty()
        && (scope == QStringLiteral("worktree") || scope == QStringLiteral("staged")
            || scope == QStringLiteral("commit"))
        && (scope != QStringLiteral("commit") || validSelectedOid);
    m_gitPinDiffButton->setEnabled(!blocked && diffReady);
    m_gitPinCommitButton->setEnabled(!blocked && validSelectedOid);
}

void AgentWorkbenchWidget::pinCurrentGitDiffContext()
{
    updateGitPinControls();
    if (!m_gitPinDiffButton || !m_gitPinDiffButton->isEnabled()) {
        addNotice(QStringLiteral("当前 Git 差异不能固定，请等待查询完成或解除只读阻塞。"), true);
        return;
    }
    const QString scope = m_gitDiffScope->currentData().toString();
    const QString oid = scope == QStringLiteral("commit") ? m_selectedGitOid : QString();
    const QString label = scope == QStringLiteral("worktree")
        ? QStringLiteral("工作区差异")
        : scope == QStringLiteral("staged")
            ? QStringLiteral("暂存区差异")
            : QStringLiteral("提交差异 · %1").arg(oid.left(8));
    m_gitContextRequestedKind = QStringLiteral("git_diff");
    m_gitContextRequestedScope = scope;
    m_gitContextRequestedOid = oid;
    m_gitContextRequestedLabel = label;
    m_gitContextRequestId = m_runtime->gitContext(
        m_projectId, m_gitContextRequestedKind, scope, oid);
    if (m_gitContextRequestId.isEmpty()) {
        m_gitContextRequestedKind.clear();
        m_gitContextRequestedScope.clear();
        m_gitContextRequestedOid.clear();
        m_gitContextRequestedLabel.clear();
        addNotice(QStringLiteral("无法读取待固定 Git 差异。"), true);
    }
    updateGitPinControls();
}

void AgentWorkbenchWidget::pinSelectedGitCommitContext()
{
    updateGitPinControls();
    if (!m_gitPinCommitButton || !m_gitPinCommitButton->isEnabled()) {
        addNotice(QStringLiteral("请先选择可固定的完整 Git 提交。"), true);
        return;
    }
    QTreeWidgetItem *item = m_gitHistory ? m_gitHistory->currentItem() : nullptr;
    const QString subject = item ? item->text(3) : QString();
    m_gitContextRequestedKind = QStringLiteral("git_commit");
    m_gitContextRequestedScope.clear();
    m_gitContextRequestedOid = m_selectedGitOid;
    m_gitContextRequestedLabel = subject.isEmpty()
        ? QStringLiteral("Git 提交 · %1").arg(m_selectedGitOid.left(8))
        : QStringLiteral("%1 · %2").arg(m_selectedGitOid.left(8), subject);
    m_gitContextRequestId = m_runtime->gitContext(
        m_projectId, m_gitContextRequestedKind, {}, m_gitContextRequestedOid);
    if (m_gitContextRequestId.isEmpty()) {
        m_gitContextRequestedKind.clear();
        m_gitContextRequestedOid.clear();
        m_gitContextRequestedLabel.clear();
        addNotice(QStringLiteral("无法读取待固定 Git 提交。"), true);
    }
    updateGitPinControls();
}

void AgentWorkbenchWidget::finishPinnedGitContext(const QJsonObject &context)
{
    const QString kind = m_gitContextRequestedKind;
    const QString scope = m_gitContextRequestedScope;
    const QString oid = m_gitContextRequestedOid;
    const QString responseKind = context.value(QStringLiteral("kind")).toString();
    const QString responseScope = context.value(QStringLiteral("scope")).toString();
    const QString responseOid = context.value(QStringLiteral("oid")).toString();
    const QString reference = context.value(QStringLiteral("reference")).toString();
    const QString contentHash = context.value(QStringLiteral("content_hash")).toString();
    const QString digest = contentHash.startsWith(QStringLiteral("sha256:"))
        ? contentHash.mid(7) : QString();
    const QByteArray content = context.value(QStringLiteral("content")).toString().toUtf8();
    const qint64 bytes = context.value(QStringLiteral("bytes")).toVariant().toLongLong();
    const qint64 sourceBytes = context.value(QStringLiteral("source_bytes"))
        .toVariant().toLongLong();
    const QString sourceHash = context.value(QStringLiteral("source_hash")).toString();
    const QString sourceDigest = sourceHash.startsWith(QStringLiteral("sha256:"))
        ? sourceHash.mid(7) : QString();
    const bool truncated = context.value(QStringLiteral("truncated")).toBool();
    const QString expectedReference = kind == QStringLiteral("git_commit")
        ? QStringLiteral("git-commit:%1").arg(oid)
        : scope == QStringLiteral("commit")
            ? QStringLiteral("git-diff:commit:%1").arg(oid)
            : QStringLiteral("git-diff:%1").arg(scope);
    const bool selectionUnchanged = kind == QStringLiteral("git_commit")
        ? m_selectedGitOid == oid
        : m_gitDiffScope && m_gitDiffScope->currentData().toString() == scope
            && (scope != QStringLiteral("commit") || m_selectedGitOid == oid);
    const bool truncationValid = context.value(QStringLiteral("truncated")).isBool()
        && ((truncated && sourceBytes > bytes
             && content.endsWith("\n[Aegisy Git context truncated]\n"))
            || (!truncated && sourceBytes == bytes));
    const bool identityValid = context.value(QStringLiteral("schema_version")).toString()
            == QStringLiteral("git-context/0.1")
        && context.value(QStringLiteral("project_id")).toString() == m_projectId
        && context.value(QStringLiteral("root_id")).toString() == QStringLiteral("root-1")
        && responseKind == kind && responseScope == scope && responseOid == oid
        && reference == expectedReference
        && context.value(QStringLiteral("content_type")).toString()
            == QStringLiteral("text/plain; charset=utf-8")
        && !context.value(QStringLiteral("content_bodies_persisted")).toBool(true)
        && !content.isEmpty() && content.size() <= kMaxInlineContextBytes
        && bytes == content.size() && sourceBytes >= bytes
        && isLowerHex(digest, 64)
        && isLowerHex(sourceDigest, 64)
        && (truncated || sourceHash == contentHash)
        && QString::fromLatin1(QCryptographicHash::hash(
               content, QCryptographicHash::Sha256).toHex()) == digest
        && truncationValid && selectionUnchanged
        && ((kind == QStringLiteral("git_commit") && scope.isEmpty() && isLowerHex(oid, 40))
            || (kind == QStringLiteral("git_diff")
                && (scope == QStringLiteral("worktree")
                    || scope == QStringLiteral("staged")
                    || (scope == QStringLiteral("commit") && isLowerHex(oid, 40)))));
    if (!identityValid || !m_pinnedContextMutationRequestId.isEmpty()) {
        addNotice(QStringLiteral("Git 上下文身份、范围或截断状态校验失败，未写入固定上下文。"), true);
        return;
    }
    const QByteArray idDigest = QCryptographicHash::hash(
        QStringLiteral("%1\n%2\n%3").arg(m_projectId, kind, reference).toUtf8(),
        QCryptographicHash::Sha256).toHex();
    const QString id = QStringLiteral("pin-%1-%2").arg(
        kind, QString::fromLatin1(idDigest));
    QJsonObject metadata{
        {QStringLiteral("truncated"), truncated ? QStringLiteral("true")
                                                : QStringLiteral("false")},
        {QStringLiteral("source_bytes"), QString::number(sourceBytes)},
        {QStringLiteral("source_hash"), sourceHash},
    };
    if (!scope.isEmpty()) metadata.insert(QStringLiteral("scope"), scope);
    if (!oid.isEmpty()) metadata.insert(QStringLiteral("oid"), oid);
    const QJsonObject descriptor{
        {QStringLiteral("id"), id},
        {QStringLiteral("project_id"), m_projectId},
        {QStringLiteral("root_id"), QStringLiteral("root-1")},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("source"), QStringLiteral("git-query")},
        {QStringLiteral("label"), m_gitContextRequestedLabel},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("content_hash"), contentHash},
        {QStringLiteral("bytes"), static_cast<double>(bytes)},
        {QStringLiteral("revision"), kind == QStringLiteral("git_commit") ? oid : contentHash},
        {QStringLiteral("freshness"), QStringLiteral("fresh")},
        {QStringLiteral("priority"), kind == QStringLiteral("git_commit") ? 710 : 720},
        {QStringLiteral("metadata"), metadata},
    };
    QJsonArray items;
    bool replaced = false;
    for (QJsonObject existing : m_pinnedContextItems) {
        existing.remove(QStringLiteral("included"));
        if (existing.value(QStringLiteral("id")).toString() == id) {
            items.append(descriptor);
            replaced = true;
        } else {
            items.append(existing);
        }
    }
    if (!replaced) {
        if (items.size() >= 128) {
            addNotice(QStringLiteral("固定上下文已达到 128 项上限。"), true);
            return;
        }
        items.append(descriptor);
    }
    m_pendingPinnedIncludeId = id;
    m_pinnedContextMutationRequestId = m_runtime->savePinnedContext(
        m_projectId, items, m_pinnedContextSetIdentity);
    if (m_pinnedContextMutationRequestId.isEmpty()) {
        m_pendingPinnedIncludeId.clear();
        addNotice(QStringLiteral("无法提交 Git 固定上下文更新。"), true);
    }
}

void AgentWorkbenchWidget::applyGitDecorations()
{
    for (auto item = m_treeItems.cbegin(); item != m_treeItems.cend(); ++item) {
        const QString kind = item.value()->data(0, kKindRole).toString();
        if (kind == QStringLiteral("unavailable")) continue;
        const bool directory = kind == QStringLiteral("directory");
        const QString status = gitStatusForPath(item.key(), directory);
        item.value()->setText(2, status);
        item.value()->setToolTip(2, status.isEmpty()
            ? QString() : QStringLiteral("Git 状态：%1").arg(status));
        QColor color(QStringLiteral("#667085"));
        if (status.contains(QLatin1Char('U'))) color = QColor(QStringLiteral("#b42318"));
        else if (status.contains(QLatin1Char('D'))) color = QColor(QStringLiteral("#d92d20"));
        else if (status == QStringLiteral("??") || status.contains(QLatin1Char('A'))) {
            color = QColor(QStringLiteral("#067647"));
        } else if (!status.isEmpty()) {
            color = QColor(QStringLiteral("#b54708"));
        }
        item.value()->setForeground(2, color);
    }
}

QString AgentWorkbenchWidget::gitStatusForPath(const QString &path, bool directory) const
{
    const auto exact = m_gitStatuses.constFind(path);
    if (exact != m_gitStatuses.cend()) return exact.value().trimmed();
    if (!directory) return {};
    const QString prefix = path + QLatin1Char('/');
    int changedChildren = 0;
    for (auto status = m_gitStatuses.cbegin(); status != m_gitStatuses.cend(); ++status) {
        if (status.key().startsWith(prefix)) ++changedChildren;
    }
    return changedChildren > 0 ? QStringLiteral("•%1").arg(changedChildren) : QString();
}

void AgentWorkbenchWidget::startWorkspaceSearch(bool nextPage)
{
    const QString query = m_workspaceSearchInput->text().trimmed();
    if (m_projectId.isEmpty()) {
        m_workspaceSearchStatus->setText(QStringLiteral("请先打开项目文件夹"));
        return;
    }
    if (query.isEmpty()) {
        m_workspaceSearchStatus->setText(QStringLiteral("请输入搜索关键词"));
        return;
    }
    if (!m_workspaceSearchRequestId.isEmpty()) return;
    if (nextPage && m_workspaceSearchCursor.isEmpty()) return;
    if (!nextPage) {
        if (!m_workspaceSearchId.isEmpty()) {
            m_runtime->cancelWorkspaceSearch(m_workspaceSearchId);
        }
        ++m_workspaceSearchSequence;
        m_workspaceSearchId = QStringLiteral("qt-search-%1").arg(m_workspaceSearchSequence);
        m_workspaceSearchCursor.clear();
        m_workspaceSearchResults->clear();
        m_workspaceSearchStale = false;
    }
    m_workspaceSearchAppending = nextPage;
    const QString mode = m_workspaceSearchMode->currentData().toString();
    m_workspaceSearchRequestId = m_runtime->searchWorkspace(
        m_projectId, m_workspaceSearchId, query, mode,
        m_workspaceSearchCase->isChecked(),
        nextPage ? m_workspaceSearchCursor : QString(), 50, m_workspaceRootId);
    if (m_workspaceSearchRequestId.isEmpty()) return;
    m_workspaceSearchStatus->setText(
        nextPage ? QStringLiteral("正在加载下一页…") : QStringLiteral("正在扫描项目…"));
    m_workspaceSearchButton->setEnabled(false);
    m_workspaceSearchCancelButton->setEnabled(true);
    m_workspaceSearchMoreButton->setEnabled(false);
}

void AgentWorkbenchWidget::cancelWorkspaceSearch()
{
    if (m_workspaceSearchId.isEmpty()) return;
    m_runtime->cancelWorkspaceSearch(m_workspaceSearchId, m_projectId, m_workspaceRootId);
    m_workspaceSearchRequestId.clear();
    m_workspaceSearchCursor.clear();
    m_workspaceSearchAppending = false;
    m_workspaceSearchButton->setEnabled(true);
    m_workspaceSearchCancelButton->setEnabled(false);
    m_workspaceSearchMoreButton->setEnabled(false);
    m_workspaceSearchStatus->setText(QStringLiteral("搜索已取消"));
}

void AgentWorkbenchWidget::appendWorkspaceSearchResults(const QJsonObject &result)
{
    if (result.value(QStringLiteral("search_id")).toString() != m_workspaceSearchId) return;
    const bool cancelled = result.value(QStringLiteral("cancelled")).toBool();
    const bool stale = result.value(QStringLiteral("stale")).toBool();
    if (cancelled) {
        m_workspaceSearchStatus->setText(QStringLiteral("搜索已取消"));
        return;
    }
    if (!m_workspaceSearchAppending || stale) m_workspaceSearchResults->clear();
    const QJsonArray matches = result.value(QStringLiteral("matches")).toArray();
    for (const QJsonValue &value : matches) {
        const QJsonObject match = value.toObject();
        const QString path = match.value(QStringLiteral("path")).toString();
        const QString type = match.value(QStringLiteral("match_type")).toString();
        const int line = match.value(QStringLiteral("line")).toInt();
        const int column = match.value(QStringLiteral("column")).toInt();
        const QString position = type == QStringLiteral("filename")
            ? QStringLiteral("文件名") : QStringLiteral("%1:%2").arg(line).arg(column);
        auto *item = new QTreeWidgetItem(m_workspaceSearchResults);
        item->setText(0, path);
        item->setText(1, position);
        item->setText(2, match.value(QStringLiteral("preview")).toString());
        item->setData(0, kPathRole, path);
        item->setData(0, kSearchLineRole, qMax(1, line));
        item->setData(0, kSearchColumnRole, qMax(1, column));
        item->setData(0, kContextRole, match.toVariantMap());
        item->setToolTip(0, path);
        item->setToolTip(2, item->text(2));
    }
    m_workspaceSearchCursor = result.value(QStringLiteral("next_cursor")).toString();
    m_workspaceSearchStale = stale;
    const int scanned = result.value(QStringLiteral("scanned_files")).toInt();
    const int skipped = result.value(QStringLiteral("skipped_files")).toInt();
    const bool truncated = result.value(QStringLiteral("truncated")).toBool();
    QStringList statusParts;
    statusParts << QStringLiteral("%1 条结果").arg(m_workspaceSearchResults->topLevelItemCount());
    statusParts << QStringLiteral("扫描 %1 个文本文件").arg(scanned);
    if (skipped > 0) statusParts << QStringLiteral("跳过 %1 个文件").arg(skipped);
    if (stale) statusParts << QStringLiteral("工作区已变化，已从新快照重载");
    else if (m_workspaceSearchStale) statusParts << QStringLiteral("结果可能已过期");
    if (truncated) statusParts << QStringLiteral("结果有上限");
    m_workspaceSearchStatus->setText(statusParts.join(QStringLiteral(" · ")));
    m_workspaceSearchButton->setEnabled(true);
    m_workspaceSearchCancelButton->setEnabled(false);
    m_workspaceSearchMoreButton->setEnabled(!m_workspaceSearchCursor.isEmpty());
    m_workspaceSearchAppending = false;
}

void AgentWorkbenchWidget::refreshRepositoryIndex()
{
    if (m_projectId.isEmpty()) {
        m_repositoryStatus->setText(QStringLiteral("请先打开项目文件夹"));
        return;
    }
    if (!m_runtime->isReady()) {
        m_repositoryStatus->setText(QStringLiteral("本地运行时尚未就绪"));
        return;
    }
    if (!m_repositoryIndexRequestId.isEmpty()) return;
    m_repositoryRefreshButton->setEnabled(false);
    m_repositoryCancelButton->setEnabled(true);
    m_repositoryStatus->setText(QStringLiteral("正在扫描变更文件并提取符号…"));
    m_repositoryIndexId = QStringLiteral("qt-index-%1").arg(++m_repositoryIndexSequence);
    m_repositoryIndexRequestId = m_runtime->indexWorkspace(
        m_projectId, m_repositoryIndexId, m_workspaceRootId);
    if (m_repositoryIndexRequestId.isEmpty()) {
        m_repositoryIndexId.clear();
        m_repositoryRefreshButton->setEnabled(true);
        m_repositoryCancelButton->setEnabled(false);
    }
}

void AgentWorkbenchWidget::cancelRepositoryIndex()
{
    if (m_repositoryIndexId.isEmpty() || m_repositoryIndexRequestId.isEmpty()
            || !m_repositoryIndexCancelRequestId.isEmpty()) {
        return;
    }
    m_repositoryIndexCancelRequestId = m_runtime->cancelWorkspaceIndex(
        m_projectId, m_repositoryIndexId, m_workspaceRootId);
    if (m_repositoryIndexCancelRequestId.isEmpty()) return;
    m_repositoryIndexRequestId.clear();
    m_repositoryCancelButton->setEnabled(false);
    m_repositoryRefreshButton->setEnabled(true);
    m_repositoryStatus->setText(QStringLiteral("正在取消索引…"));
}

void AgentWorkbenchWidget::populateRepositoryIndex(const QJsonObject &result)
{
    m_repositorySymbols->clear();
    m_repositoryDependencies->clear();
    QHash<QString, QTreeWidgetItem *> fileItems;
    const QJsonArray symbols = result.value(QStringLiteral("symbols")).toArray();
    for (const QJsonValue &value : symbols) {
        const QJsonObject symbol = value.toObject();
        const QString path = symbol.value(QStringLiteral("path")).toString();
        QTreeWidgetItem *fileItem = fileItems.value(path, nullptr);
        if (!fileItem) {
            fileItem = new QTreeWidgetItem(m_repositorySymbols);
            fileItem->setText(0, path);
            fileItem->setText(1, symbol.value(QStringLiteral("language")).toString());
            fileItem->setData(0, kPathRole, path);
            fileItem->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
            fileItem->setToolTip(0, path);
            fileItems.insert(path, fileItem);
        }
        const int line = symbol.value(QStringLiteral("line")).toInt();
        const int column = symbol.value(QStringLiteral("column")).toInt();
        auto *item = new QTreeWidgetItem(fileItem);
        item->setText(0, symbol.value(QStringLiteral("name")).toString());
        item->setText(1, symbol.value(QStringLiteral("kind")).toString());
        item->setText(2, QStringLiteral("%1:%2").arg(line).arg(column));
        item->setData(0, kPathRole, path);
        item->setData(0, kSymbolLineRole, line);
        item->setData(0, kSymbolColumnRole, column);
        item->setToolTip(0, QStringLiteral("%1 · Tree-sitter").arg(path));
    }
    for (auto item = fileItems.cbegin(); item != fileItems.cend(); ++item) {
        item.value()->setExpanded(fileItems.size() <= 12);
    }

    const QJsonArray dependencies = result.value(QStringLiteral("dependencies")).toArray();
    for (const QJsonValue &value : dependencies) {
        const QJsonObject edge = value.toObject();
        auto *item = new QTreeWidgetItem(m_repositoryDependencies);
        item->setText(0, edge.value(QStringLiteral("from")).toString());
        item->setText(1, edge.value(QStringLiteral("target")).toString());
        item->setText(2, QStringLiteral("%1 · %2")
            .arg(edge.value(QStringLiteral("kind")).toString())
            .arg(edge.value(QStringLiteral("line")).toInt()));
        item->setToolTip(0, item->text(0));
        item->setToolTip(1, item->text(1));
    }

    const int indexed = result.value(QStringLiteral("indexed_files")).toInt();
    const int parsed = result.value(QStringLiteral("parsed_files")).toInt();
    const int reused = result.value(QStringLiteral("reused_files")).toInt();
    const int skipped = result.value(QStringLiteral("skipped_files")).toInt();
    QStringList summary;
    summary << QStringLiteral("%1 个文件").arg(indexed)
            << QStringLiteral("%1 个符号").arg(symbols.size())
            << QStringLiteral("%1 条依赖").arg(dependencies.size())
            << QStringLiteral("解析 %1 / 复用 %2").arg(parsed).arg(reused);
    if (skipped > 0) summary << QStringLiteral("跳过 %1").arg(skipped);
    if (result.value(QStringLiteral("truncated")).toBool()) {
        summary << QStringLiteral("达到安全上限");
    }
    m_repositoryIndexSummary = summary.join(QStringLiteral(" · "));
    m_repositoryStatus->setText(m_repositoryIndexSummary);
    m_repositoryIndexLoaded = true;
    m_repositoryIndexStale = false;
}

void AgentWorkbenchWidget::requestRepositoryMap()
{
    if (!m_repositoryIndexLoaded || m_projectId.isEmpty()
            || !m_repositoryMapRequestId.isEmpty()) {
        return;
    }
    QStringList focusPaths;
    if (!m_openEditorPath.isEmpty()) focusPaths.append(m_openEditorPath);
    const int tokenBudget = m_repositoryMapBudget->currentData().toInt();
    m_repositoryRefreshButton->setEnabled(false);
    m_repositoryStatus->setText(
        QStringLiteral("%1 · 正在生成仓库地图…").arg(m_repositoryIndexSummary));
    m_repositoryMapRequestId = m_runtime->repositoryMap(
        m_projectId, tokenBudget, focusPaths, m_workspaceRootId);
    if (m_repositoryMapRequestId.isEmpty()) {
        m_repositoryRefreshButton->setEnabled(true);
    }
}

void AgentWorkbenchWidget::openRepositorySymbol(QTreeWidgetItem *item)
{
    if (!item) return;
    const QString path = item->data(0, kPathRole).toString();
    const int line = item->data(0, kSymbolLineRole).toInt();
    const int column = item->data(0, kSymbolColumnRole).toInt();
    if (path.isEmpty() || line <= 0) return;
    m_pendingSearchPath = path;
    m_pendingSearchLine = line;
    m_pendingSearchColumn = qMax(1, column);
    if (m_editorBuffers.contains(path)) {
        EditorBuffer &buffer = m_editorBuffers[path];
        const int offset = editorOffsetForLineColumn(buffer.content, line, column);
        buffer.cursorPosition = offset;
        buffer.anchorPosition = offset;
        m_pendingSearchPath.clear();
        activateEditorBuffer(path);
        return;
    }
    requestEditorFile(path);
}

void AgentWorkbenchWidget::markRepositoryIndexStale()
{
    m_repositoryIndexStale = true;
    if (!m_repositoryIndexLoaded || !m_repositoryStatus) return;
    m_repositoryStatus->setText(QStringLiteral("%1 · 工作区已变化，需刷新")
        .arg(m_repositoryIndexSummary));
}

void AgentWorkbenchWidget::requestLanguageServers()
{
    if (m_projectId.isEmpty() || !m_runtime->isReady()
            || !m_languageServersRequestId.isEmpty()) {
        return;
    }
    m_languageServersRequestId = m_runtime->languageServers(m_projectId, m_workspaceRootId);
}

void AgentWorkbenchWidget::requestLanguageAction(const QString &action)
{
    if (m_projectId.isEmpty() || m_openEditorPath.isEmpty()
            || !m_editorBuffers.contains(m_openEditorPath)
            || !m_languageRequestId.isEmpty()) {
        return;
    }
    storeActiveEditorState();
    const EditorBuffer &buffer = m_editorBuffers[m_openEditorPath];
    const int position = qBound(0, buffer.cursorPosition, buffer.content.size());
    const QString preceding = buffer.content.left(position);
    const int line = preceding.count(QLatin1Char('\n')) + 1;
    const int previousNewline = preceding.lastIndexOf(QLatin1Char('\n'));
    const int column = position - previousNewline;
    if (action == QStringLiteral("definition")) {
        m_languageRequestId = m_runtime->workspaceDefinition(
            m_projectId, m_openEditorPath, buffer.content, buffer.revision, line, column,
            m_workspaceRootId);
        m_languageStatus->setText(QStringLiteral("LSP · 正在查找定义…"));
    } else if (action == QStringLiteral("references")) {
        m_languageRequestId = m_runtime->workspaceReferences(
            m_projectId, m_openEditorPath, buffer.content, buffer.revision, line, column,
            m_workspaceRootId);
        m_languageStatus->setText(QStringLiteral("LSP · 正在查找引用…"));
    } else if (action == QStringLiteral("diagnostics")) {
        m_languageRequestId = m_runtime->workspaceDiagnostics(
            m_projectId, m_openEditorPath, buffer.content, buffer.revision,
            m_workspaceRootId);
        m_languageStatus->setText(QStringLiteral("LSP · 正在计算诊断…"));
    }
    updateEditorActions();
}

void AgentWorkbenchWidget::populateLanguageLocations(const QJsonObject &result,
                                                     const QString &type)
{
    m_languageResults->clear();
    const QJsonArray locations = result.value(QStringLiteral("locations")).toArray();
    for (const QJsonValue &value : locations) {
        const QJsonObject location = value.toObject();
        const QString path = location.value(QStringLiteral("path")).toString();
        const int line = location.value(QStringLiteral("line")).toInt();
        const int column = location.value(QStringLiteral("column")).toInt();
        auto *item = new QTreeWidgetItem(m_languageResults);
        item->setText(0, type);
        item->setText(1, path);
        item->setText(2, QStringLiteral("%1:%2").arg(line).arg(column));
        item->setData(0, kPathRole, path);
        item->setData(0, kSymbolLineRole, line);
        item->setData(0, kSymbolColumnRole, column);
        item->setToolTip(1, QStringLiteral("%1\n%2")
            .arg(path, location.value(QStringLiteral("provenance")).toString()));
    }
    m_languageResultsStale = false;
    const QString server = result.value(QStringLiteral("server_id")).toString();
    const int denied = result.value(QStringLiteral("denied_locations")).toInt();
    const bool truncated = result.value(QStringLiteral("truncated")).toBool();
    m_languageStatus->setText(QStringLiteral("LSP · %1 · %2 %3%4%5")
        .arg(server, type).arg(locations.size())
        .arg(denied > 0 ? QStringLiteral(" · 已过滤 %1 个越界结果").arg(denied) : QString())
        .arg(truncated ? QStringLiteral(" · 已截断") : QString()));
    m_languageStopButton->setEnabled(true);
    if (type == QStringLiteral("定义") && locations.size() == 1) {
        openLanguageResult(m_languageResults->topLevelItem(0));
    } else {
        m_workspaceTabs->setCurrentIndex(m_structureWorkspaceTab);
        m_repositoryViews->setCurrentIndex(m_languageResultsView);
    }
}

void AgentWorkbenchWidget::populateLanguageDiagnostics(const QJsonObject &result,
                                                        bool activateView)
{
    m_languageDiagnostics->clear();
    const QJsonArray diagnostics = result.contains(QStringLiteral("observed_diagnostics"))
        ? result.value(QStringLiteral("observed_diagnostics")).toArray()
        : result.value(QStringLiteral("diagnostics")).toArray();
    m_diagnosticRawReference = result.value(QStringLiteral("raw_output_ref")).toString();
    m_languageRawButton->setEnabled(!m_diagnosticRawReference.isEmpty());
    for (const QJsonValue &value : diagnostics) {
        const QJsonObject diagnostic = value.toObject();
        const QString severity = diagnostic.value(QStringLiteral("severity")).toString();
        const QString path = diagnostic.value(QStringLiteral("path")).toString();
        const int line = diagnostic.value(QStringLiteral("line")).toInt();
        const int column = diagnostic.value(QStringLiteral("column")).toInt();
        const QString freshness = diagnostic.value(QStringLiteral("freshness"))
            .toString(QStringLiteral("fresh"));
        auto *item = new QTreeWidgetItem(m_languageDiagnostics);
        const QString severityLabel = severity == QStringLiteral("error")
            ? QStringLiteral("错误")
            : severity == QStringLiteral("warning") ? QStringLiteral("警告")
            : severity == QStringLiteral("hint") ? QStringLiteral("提示")
                                                   : QStringLiteral("信息");
        item->setText(0, severityLabel);
        item->setText(1, path);
        item->setText(2, QStringLiteral("%1:%2").arg(line).arg(column));
        item->setText(3, freshness == QStringLiteral("stale")
            ? QStringLiteral("已过期") : QStringLiteral("新鲜"));
        item->setText(4, diagnostic.value(QStringLiteral("message")).toString());
        item->setData(0, kPathRole, path);
        item->setData(0, kSymbolLineRole, line);
        item->setData(0, kSymbolColumnRole, column);
        item->setData(0, kContextRole, diagnostic.toVariantMap());
        const QColor color = severity == QStringLiteral("error")
            ? QColor(QStringLiteral("#b42318"))
            : severity == QStringLiteral("warning")
                ? QColor(QStringLiteral("#b54708")) : QColor(QStringLiteral("#175cd3"));
        item->setForeground(0, color);
        const QString sourceIdentity = diagnostic.value(QStringLiteral("source_identity"))
            .toString(diagnostic.value(QStringLiteral("provenance")).toString());
        const QString sourceServer = diagnostic.value(QStringLiteral("source_server")).toString();
        const QString sourceCommand = diagnostic.value(QStringLiteral("source_command")).toString();
        const QString fileHash = diagnostic.value(QStringLiteral("file_hash")).toString();
        QStringList provenance;
        if (!sourceIdentity.isEmpty()) provenance.append(
            QStringLiteral("来源：%1").arg(sourceIdentity));
        if (!sourceServer.isEmpty() && sourceServer != sourceIdentity) provenance.append(
            QStringLiteral("服务器：%1").arg(sourceServer));
        if (!sourceCommand.isEmpty()) provenance.append(
            QStringLiteral("命令：%1").arg(sourceCommand));
        if (!fileHash.isEmpty()) provenance.append(
            QStringLiteral("文件 SHA-256：%1").arg(fileHash));
        provenance.prepend(item->text(4));
        const QString toolTip = provenance.join(QLatin1Char('\n'));
        item->setToolTip(3, toolTip);
        item->setToolTip(4, toolTip);
    }
    m_languageResultsStale = false;
    const bool pending = result.value(QStringLiteral("pending")).toBool();
    const bool truncated = result.value(QStringLiteral("truncated")).toBool();
    const bool commandSource = result.value(QStringLiteral("source_kind")).toString()
        == QStringLiteral("command");
    const QString identity = commandSource
        ? result.value(QStringLiteral("source_identity")).toString()
        : result.value(QStringLiteral("server_id")).toString();
    m_languageStatus->setText(QStringLiteral("%1 · %2 · %3 条诊断%4%5")
        .arg(commandSource ? QStringLiteral("命令诊断") : QStringLiteral("LSP"), identity)
        .arg(diagnostics.size())
        .arg(pending ? QStringLiteral(" · 服务器仍在计算") : QString())
        .arg(truncated ? QStringLiteral(" · 已截断") : QString()));
    m_languageStopButton->setEnabled(!commandSource);
    if (activateView) {
        m_workspaceTabs->setCurrentIndex(m_structureWorkspaceTab);
        m_repositoryViews->setCurrentIndex(m_languageDiagnosticsView);
    }
}

void AgentWorkbenchWidget::openLanguageResult(QTreeWidgetItem *item)
{
    if (!item) return;
    const QString path = item->data(0, kPathRole).toString();
    const int line = item->data(0, kSymbolLineRole).toInt();
    const int column = item->data(0, kSymbolColumnRole).toInt();
    if (path.isEmpty() || line <= 0) return;
    m_pendingSearchPath = path;
    m_pendingSearchLine = line;
    m_pendingSearchColumn = qMax(1, column);
    if (m_editorBuffers.contains(path)) {
        EditorBuffer &buffer = m_editorBuffers[path];
        const int offset = editorOffsetForLineColumn(buffer.content, line, column);
        buffer.cursorPosition = offset;
        buffer.anchorPosition = offset;
        m_pendingSearchPath.clear();
        activateEditorBuffer(path);
        return;
    }
    requestEditorFile(path);
}

void AgentWorkbenchWidget::markLanguageResultsStale()
{
    if (m_languageResults->topLevelItemCount() == 0
            && m_languageDiagnostics->topLevelItemCount() == 0) {
        return;
    }
    m_languageResultsStale = true;
    const QColor staleColor(QStringLiteral("#98a2b3"));
    for (int row = 0; row < m_languageDiagnostics->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_languageDiagnostics->topLevelItem(row);
        item->setText(3, QStringLiteral("已过期"));
        for (int column = 0; column < m_languageDiagnostics->columnCount(); ++column) {
            item->setForeground(column, staleColor);
        }
    }
    m_languageStatus->setText(QStringLiteral("LSP · 工作区已变化，当前结果已过期"));
}

void AgentWorkbenchWidget::openWorkspaceSearchResult(QTreeWidgetItem *item)
{
    if (!item) return;
    const QString path = item->data(0, kPathRole).toString();
    if (path.isEmpty()) return;
    m_pendingSearchPath = path;
    m_pendingSearchLine = item->data(0, kSearchLineRole).toInt();
    m_pendingSearchColumn = item->data(0, kSearchColumnRole).toInt();
    if (m_editorBuffers.contains(path)) {
        EditorBuffer &buffer = m_editorBuffers[path];
        const int offset = editorOffsetForLineColumn(
            buffer.content, m_pendingSearchLine, m_pendingSearchColumn);
        buffer.cursorPosition = offset;
        buffer.anchorPosition = offset;
        m_pendingSearchPath.clear();
        activateEditorBuffer(path);
        return;
    }
    requestEditorFile(path);
}

void AgentWorkbenchWidget::openWorkspaceFile(QTreeWidgetItem *item)
{
    if (!item || item->data(0, kKindRole).toString() != QStringLiteral("file")) return;
    const QString path = item->data(0, kPathRole).toString();
    requestEditorFile(path);
}

void AgentWorkbenchWidget::requestEditorFile(const QString &path, bool restoring)
{
    if (path.isEmpty() || m_projectId.isEmpty() || !m_editorSaveRequestId.isEmpty()) return;
    if (m_editorBuffers.contains(path)) {
        if (!restoring) activateEditorBuffer(path);
        return;
    }
    if (!restoring && m_editorLoading) return;
    storeActiveEditorState();
    if (!restoring) {
        m_fileStatus->setText(QStringLiteral("正在读取 %1…").arg(QFileInfo(path).fileName()));
        m_editorLoading = true;
        updateEditorActions();
    }
    const QString requestId = m_runtime->readWorkspaceFile(m_projectId, path,
                                                            m_workspaceRootId);
    if (requestId.isEmpty()) {
        if (!restoring) m_editorLoading = false;
        return;
    }
    m_workspaceReadRequests.insert(requestId, path);
    if (restoring) m_editorRestoreRequests.insert(requestId);
}

void AgentWorkbenchWidget::activateEditorBuffer(const QString &path)
{
    if (!m_editorBuffers.contains(path)) return;
    if (!m_editorSaveRequestId.isEmpty() && path != m_openEditorPath) return;
    if (!m_openEditorPath.isEmpty() && path != m_openEditorPath) storeActiveEditorState();
    const EditorBuffer buffer = m_editorBuffers.value(path);
    m_openEditorPath = path;
    m_activeEditorGroup = qBound(0, m_activeEditorGroup, 1);
    m_editorGroupPaths[m_activeEditorGroup] = path;
    m_editorRevision = buffer.revision;
    m_editorEncoding = buffer.encoding;
    m_editorNewline = buffer.newline;
    m_editorSaveSupported = buffer.saveSupported;
    m_editorConflict = buffer.conflict;
    m_editorLoading = false;
#ifdef AEGISY_HAS_MONACO
    m_monacoContentWithinLimit = buffer.content.toUtf8().size() <= 512 * 1024;
#endif
    {
        const QSignalBlocker blocker(m_editor->document());
        m_editor->setPlainText(buffer.content);
        QTextCursor cursor = m_editor->textCursor();
        cursor.setPosition(qBound(0, buffer.anchorPosition, buffer.content.size()));
        cursor.setPosition(qBound(0, buffer.cursorPosition, buffer.content.size()),
                           QTextCursor::KeepAnchor);
        m_editor->setTextCursor(cursor);
        m_editor->document()->setModified(buffer.modified);
    }
    m_editor->verticalScrollBar()->setValue(buffer.verticalScroll);
    m_editor->horizontalScrollBar()->setValue(buffer.horizontalScroll);
    m_editorMeta->setText(buffer.metadata);
    m_editorPath->setToolTip(path);
    if (buffer.conflict) {
        m_fileStatus->setText(buffer.modified
            ? QStringLiteral("保存已阻止：本地编辑与外部修改发生冲突")
            : QStringLiteral("文件已在工作区变化，请重新载入后继续编辑"));
    } else if (buffer.fallback || !buffer.saveSupported) {
        m_fileStatus->setText(QStringLiteral("此文件不满足安全保存条件，已使用只读预览"));
    } else {
        m_fileStatus->setText(QStringLiteral("用户编辑可保存 · Agent 仍保持只读"));
    }
    const int index = editorTabIndex(path);
    if (index >= 0) {
        m_switchingEditorTab = true;
        m_editorTabs->setCurrentIndex(index);
        m_switchingEditorTab = false;
    }
    updateEditorActions();
    updateEditorTab(path);
    if (m_editorWorkspaceTab >= 0) m_workspaceTabs->setCurrentIndex(m_editorWorkspaceTab);
#ifdef AEGISY_HAS_MONACO
    syncMonacoModel();
#endif
    if (m_editorRestoreRequests.isEmpty()) saveEditorViewState();
}

void AgentWorkbenchWidget::storeActiveEditorState()
{
    if (m_openEditorPath.isEmpty() || !m_editorBuffers.contains(m_openEditorPath) || !m_editor) {
        return;
    }
    EditorBuffer &buffer = m_editorBuffers[m_openEditorPath];
    buffer.content = m_editor->toPlainText();
    buffer.revision = m_editorRevision;
    buffer.encoding = m_editorEncoding;
    buffer.newline = m_editorNewline;
    buffer.metadata = m_editorMeta->text();
#ifdef AEGISY_HAS_MONACO
    if (!m_monacoReady || m_editorStack->currentWidget() != m_monacoView) {
#endif
        buffer.cursorPosition = m_editor->textCursor().position();
        buffer.anchorPosition = m_editor->textCursor().anchor();
        buffer.verticalScroll = m_editor->verticalScrollBar()->value();
        buffer.horizontalScroll = m_editor->horizontalScrollBar()->value();
#ifdef AEGISY_HAS_MONACO
    }
#endif
    buffer.saveSupported = m_editorSaveSupported;
    buffer.modified = m_editor->document()->isModified();
    buffer.conflict = m_editorConflict;
    updateEditorTab(m_openEditorPath);
}

void AgentWorkbenchWidget::closeEditorTab(int index)
{
    if (index < 0 || index >= m_editorTabs->count() || m_editorLoading
            || !m_editorSaveRequestId.isEmpty()) return;
    const QString path = m_editorTabs->tabData(index).toString();
    if (path == m_openEditorPath) storeActiveEditorState();
    const EditorBuffer buffer = m_editorBuffers.value(path);
    if (buffer.modified) {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("关闭未保存文件？"),
            QStringLiteral("%1 中的未保存修改将被放弃。").arg(QFileInfo(path).fileName()),
            QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) return;
    }
    const bool wasActive = path == m_openEditorPath;
    const bool displayedInGroup[2] = {
        m_editorGroupPaths[0] == path,
        m_editorGroupPaths[1] == path
    };
#ifdef AEGISY_HAS_MONACO
    if (m_monacoBridge) m_monacoBridge->closeModel(path);
#endif
    m_editorBuffers.remove(path);
    {
        const QSignalBlocker blocker(m_editorTabs);
        m_editorTabs->removeTab(index);
    }
    if (m_editorTabs->count() == 0) {
        resetEditorModel();
        saveEditorViewState();
        return;
    }
    const int next = qMin(index, m_editorTabs->count() - 1);
    const QString replacement = m_editorTabs->tabData(next).toString();
    for (int group = 0; group < 2; ++group) {
        if (displayedInGroup[group]) m_editorGroupPaths[group] = replacement;
    }
    if (wasActive) {
        m_openEditorPath.clear();
        activateEditorBuffer(m_editorGroupPaths[m_activeEditorGroup]);
    } else {
#ifdef AEGISY_HAS_MONACO
        for (int group = 0; group < 2; ++group) {
            if (displayedInGroup[group]) syncMonacoModel(group, replacement);
        }
#endif
    }
    saveEditorViewState();
}

int AgentWorkbenchWidget::editorTabIndex(const QString &path) const
{
    if (!m_editorTabs) return -1;
    for (int index = 0; index < m_editorTabs->count(); ++index) {
        if (m_editorTabs->tabData(index).toString() == path) return index;
    }
    return -1;
}

void AgentWorkbenchWidget::updateEditorTab(const QString &path)
{
    const int index = editorTabIndex(path);
    if (index < 0 || !m_editorBuffers.contains(path)) return;
    const EditorBuffer &buffer = m_editorBuffers[path];
    QString suffix;
    if (buffer.conflict) suffix = QStringLiteral(" !");
    else if (buffer.modified) suffix = QStringLiteral(" *");
    m_editorTabs->setTabText(index, QFileInfo(path).fileName() + suffix);
    m_editorTabs->setTabToolTip(index, path);
}

void AgentWorkbenchWidget::addRecentFile(const QString &path)
{
    m_recentFiles.removeAll(path);
    m_recentFiles.prepend(path);
    while (m_recentFiles.size() > 12) m_recentFiles.removeLast();
    updateRecentFilePicker();
}

void AgentWorkbenchWidget::updateRecentFilePicker()
{
    if (!m_recentFilePicker) return;
    const QSignalBlocker blocker(m_recentFilePicker);
    m_recentFilePicker->clear();
    m_recentFilePicker->addItem(QStringLiteral("最近文件"));
    for (const QString &path : m_recentFiles) {
        m_recentFilePicker->addItem(QFileInfo(path).fileName(), path);
        m_recentFilePicker->setItemData(m_recentFilePicker->count() - 1, path, Qt::ToolTipRole);
    }
    m_recentFilePicker->setCurrentIndex(0);
}

QString AgentWorkbenchWidget::editorSettingsKey() const
{
    if (m_projectRoot.isEmpty()) return {};
    const QByteArray digest = QCryptographicHash::hash(
        QFileInfo(m_projectRoot).canonicalFilePath().toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("agent_workbench/editor/%1").arg(QString::fromLatin1(digest.left(20)));
}

void AgentWorkbenchWidget::loadEditorViewState()
{
    const QString key = editorSettingsKey();
    if (key.isEmpty()) return;
    QSettings settings;
    m_recentFiles = settings.value(key + QStringLiteral("/recent_files")).toStringList();
    updateRecentFilePicker();
    QStringList openPaths = settings.value(key + QStringLiteral("/open_tabs")).toStringList();
    m_editorRestoreActivePath = settings.value(key + QStringLiteral("/active_file")).toString();
    m_editorRestoreSplitEnabled = settings.value(
        key + QStringLiteral("/split_enabled"), false).toBool();
    m_editorRestoreActiveGroup = qBound(
        0, settings.value(key + QStringLiteral("/active_group"), 0).toInt(), 1);
    m_editorRestoreGroupPaths[0] = settings.value(
        key + QStringLiteral("/group_0_file"), m_editorRestoreActivePath).toString();
    m_editorRestoreGroupPaths[1] = settings.value(
        key + QStringLiteral("/group_1_file")).toString();
    for (const QString &path : m_editorRestoreGroupPaths) {
        if (!path.isEmpty() && !openPaths.contains(path)) openPaths.append(path);
    }
    const int viewCount = settings.beginReadArray(key + QStringLiteral("/views"));
    for (int index = 0; index < viewCount; ++index) {
        settings.setArrayIndex(index);
        const QString path = settings.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) continue;
        EditorViewState view;
        view.cursorPosition = settings.value(QStringLiteral("cursor")).toInt();
        view.anchorPosition = settings.value(QStringLiteral("anchor"), view.cursorPosition).toInt();
        view.verticalScroll = settings.value(QStringLiteral("vertical_scroll")).toInt();
        view.horizontalScroll = settings.value(QStringLiteral("horizontal_scroll")).toInt();
        m_restoredEditorViews.insert(path, view);
    }
    settings.endArray();
    QSet<QString> requested;
    for (const QString &path : openPaths.mid(0, 8)) {
        const QString clean = QDir::cleanPath(path);
        if (clean.isEmpty() || QDir::isAbsolutePath(clean) || clean == QStringLiteral("..")
                || clean.startsWith(QStringLiteral("../")) || requested.contains(clean)) {
            continue;
        }
        requested.insert(clean);
        requestEditorFile(clean, true);
    }
}

void AgentWorkbenchWidget::saveEditorViewState()
{
    const QString key = editorSettingsKey();
    if (key.isEmpty() || !m_editorTabs) return;
    storeActiveEditorState();
    QStringList paths;
    for (int index = 0; index < m_editorTabs->count(); ++index) {
        const QString path = m_editorTabs->tabData(index).toString();
        if (!path.isEmpty()) paths.append(path);
    }
    QSettings settings;
    settings.setValue(key + QStringLiteral("/recent_files"), m_recentFiles);
    settings.setValue(key + QStringLiteral("/open_tabs"), paths);
    settings.setValue(key + QStringLiteral("/active_file"), m_openEditorPath);
    settings.setValue(key + QStringLiteral("/split_enabled"), m_editorSplitEnabled);
    settings.setValue(key + QStringLiteral("/active_group"), m_activeEditorGroup);
    settings.setValue(key + QStringLiteral("/group_0_file"), m_editorGroupPaths[0]);
    settings.setValue(key + QStringLiteral("/group_1_file"), m_editorGroupPaths[1]);
    settings.beginWriteArray(key + QStringLiteral("/views"), paths.size());
    for (int index = 0; index < paths.size(); ++index) {
        settings.setArrayIndex(index);
        const QString path = paths.at(index);
        const EditorBuffer buffer = m_editorBuffers.value(path);
        settings.setValue(QStringLiteral("path"), path);
        settings.setValue(QStringLiteral("cursor"), buffer.cursorPosition);
        settings.setValue(QStringLiteral("anchor"), buffer.anchorPosition);
        settings.setValue(QStringLiteral("vertical_scroll"), buffer.verticalScroll);
        settings.setValue(QStringLiteral("horizontal_scroll"), buffer.horizontalScroll);
    }
    settings.endArray();
}

void AgentWorkbenchWidget::setEditorSplitEnabled(bool enabled)
{
#ifdef AEGISY_HAS_MONACO
    if (enabled && (!m_monacoReady || m_editorBuffers.isEmpty())) {
        m_editorSplitEnabled = enabled && !m_editorBuffers.isEmpty();
        const QSignalBlocker blocker(m_editorSplitButton);
        m_editorSplitButton->setChecked(false);
        return;
    }
    if (enabled && m_editorBuffers.value(m_openEditorPath).fallback) {
        const QSignalBlocker blocker(m_editorSplitButton);
        m_editorSplitButton->setChecked(false);
        m_fileStatus->setText(QStringLiteral("只读降级文件不支持编辑器分栏"));
        return;
    }
    storeActiveEditorState();
    m_editorSplitEnabled = enabled;
    if (!m_editorBuffers.contains(m_editorGroupPaths[0])) {
        m_editorGroupPaths[0] = m_openEditorPath;
    }
    if (enabled && !m_editorBuffers.contains(m_editorGroupPaths[1])) {
        QString secondary;
        for (int index = 0; index < m_editorTabs->count(); ++index) {
            const QString candidate = m_editorTabs->tabData(index).toString();
            if (candidate != m_editorGroupPaths[0]
                    && !m_editorBuffers.value(candidate).fallback) {
                secondary = candidate;
                break;
            }
        }
        m_editorGroupPaths[1] = secondary.isEmpty()
            ? m_editorGroupPaths[0] : secondary;
    }
    if (!enabled && m_activeEditorGroup == 1) {
        m_activeEditorGroup = 0;
        if (!m_editorBuffers.contains(m_editorGroupPaths[0])) {
            m_editorGroupPaths[0] = m_editorGroupPaths[1];
        }
    }
    if (m_monacoBridge) {
        m_monacoBridge->setSplitEnabled(enabled);
        if (enabled) {
            syncMonacoModel(0, m_editorGroupPaths[0]);
            syncMonacoModel(1, m_editorGroupPaths[1]);
        }
    }
    const QSignalBlocker blocker(m_editorSplitButton);
    m_editorSplitButton->setChecked(enabled);
    m_editorSplitButton->setToolTip(
        enabled ? QStringLiteral("关闭编辑器分栏") : QStringLiteral("拆分编辑器"));
    const QString activePath = m_editorGroupPaths[m_activeEditorGroup];
    if (m_editorBuffers.contains(activePath)) activateEditorBuffer(activePath);
    saveEditorViewState();
#else
    Q_UNUSED(enabled);
#endif
}

void AgentWorkbenchWidget::activateEditorGroup(int group, const QString &path)
{
    if (group < 0 || group > 1 || (group == 1 && !m_editorSplitEnabled)
            || !m_editorBuffers.contains(path)) {
        return;
    }
    if (group == m_activeEditorGroup && path == m_openEditorPath) return;
    storeActiveEditorState();
    m_activeEditorGroup = group;
    m_editorGroupPaths[group] = path;
    activateEditorBuffer(path);
}

void AgentWorkbenchWidget::restoreEditorGroups()
{
    QString fallback = m_editorRestoreActivePath;
    if (!m_editorBuffers.contains(fallback) && !m_editorBuffers.isEmpty()) {
        fallback = m_editorBuffers.constBegin().key();
    }
    for (int group = 0; group < 2; ++group) {
        const QString restored = m_editorRestoreGroupPaths[group];
        m_editorGroupPaths[group] = m_editorBuffers.contains(restored) ? restored : fallback;
    }
    m_activeEditorGroup = m_editorRestoreSplitEnabled
        ? m_editorRestoreActiveGroup : 0;
    if (!m_editorBuffers.contains(m_editorGroupPaths[m_activeEditorGroup])) {
        m_activeEditorGroup = 0;
    }
    const bool restoreSplit = m_editorRestoreSplitEnabled
        && !m_editorGroupPaths[1].isEmpty();
    m_editorRestoreSplitEnabled = false;
    m_editorRestoreActiveGroup = 0;
    m_editorRestoreGroupPaths[0].clear();
    m_editorRestoreGroupPaths[1].clear();
#ifdef AEGISY_HAS_MONACO
    if (m_monacoReady) setEditorSplitEnabled(restoreSplit);
    else m_editorSplitEnabled = restoreSplit;
#else
    m_editorSplitEnabled = false;
#endif
    if (!m_openEditorPath.isEmpty() && !m_editorSplitEnabled) {
        activateEditorBuffer(m_editorGroupPaths[m_activeEditorGroup]);
    }
    saveEditorViewState();
}

void AgentWorkbenchWidget::setEditorSearchVisible(bool visible, bool replace)
{
    m_editorSearchBar->setVisible(visible);
    if (!visible) {
        m_editorSearchStatus->clear();
        m_editor->setFocus();
        return;
    }
    Q_UNUSED(replace);
    m_editorFind->setFocus();
    m_editorFind->selectAll();
}

void AgentWorkbenchWidget::findEditorText(bool backwards)
{
    const QString term = m_editorFind->text();
    if (term.isEmpty() || m_openEditorPath.isEmpty()) {
        m_editorSearchStatus->setText(QStringLiteral("输入关键词"));
        return;
    }
    QTextDocument::FindFlags flags;
    if (backwards) flags |= QTextDocument::FindBackward;
    if (m_editorCaseSensitive->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    bool found = m_editor->find(term, flags);
    if (!found) {
        QTextCursor cursor = m_editor->textCursor();
        cursor.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
        m_editor->setTextCursor(cursor);
        found = m_editor->find(term, flags);
    }
    m_editorSearchStatus->setText(found ? QStringLiteral("已定位") : QStringLiteral("未找到"));
}

void AgentWorkbenchWidget::replaceEditorSelection()
{
    if (m_editor->isReadOnly()) return;
    QTextCursor cursor = m_editor->textCursor();
    const Qt::CaseSensitivity sensitivity = m_editorCaseSensitive->isChecked()
        ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (!cursor.hasSelection()
            || cursor.selectedText().compare(m_editorFind->text(), sensitivity) != 0) {
        findEditorText(false);
        return;
    }
    cursor.insertText(m_editorReplace->text());
    m_editor->setTextCursor(cursor);
    m_editorSearchStatus->setText(QStringLiteral("已替换"));
    findEditorText(false);
}

void AgentWorkbenchWidget::replaceAllEditorText()
{
    const QString term = m_editorFind->text();
    if (term.isEmpty() || m_editor->isReadOnly()) return;
    QTextDocument::FindFlags flags;
    if (m_editorCaseSensitive->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    QTextCursor editCursor(m_editor->document());
    editCursor.beginEditBlock();
    QTextCursor match(m_editor->document());
    match.movePosition(QTextCursor::Start);
    int count = 0;
    while (true) {
        match = m_editor->document()->find(term, match, flags);
        if (match.isNull()) break;
        match.insertText(m_editorReplace->text());
        ++count;
    }
    editCursor.endEditBlock();
    m_editorSearchStatus->setText(QStringLiteral("已替换 %1 处").arg(count));
}

void AgentWorkbenchWidget::updateResponsiveEditorChrome()
{
    if (!m_workspaceTabs || !m_recentFilePicker || !m_editorMeta) return;
    const bool compact = m_workspaceTabs->width() < 390;
    m_recentFilePicker->setVisible(!compact);
    m_editorMeta->setVisible(!compact);
    if (compact && !m_openEditorPath.isEmpty()) {
        m_editorPath->setToolTip(QStringLiteral("%1\n%2")
            .arg(m_openEditorPath, m_editorMeta->text()));
    }
}

#ifdef AEGISY_HAS_MONACO
void AgentWorkbenchWidget::initializeTerminalWeb(QWidget *parent)
{
    const QString entryPath = terminalEntryPath();
    if (!QFileInfo::exists(entryPath) || !m_terminalStack) {
        m_terminalStatus->setText(QStringLiteral("xterm.js 本地资源缺失，已使用原生输出视图"));
        return;
    }
    m_terminalView = new QWebEngineView(parent);
    m_terminalView->setObjectName(QStringLiteral("agentXtermTerminal"));
    m_terminalView->setContextMenuPolicy(Qt::NoContextMenu);
    m_terminalProfile = new QWebEngineProfile(m_terminalView);
    m_terminalProfile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    m_terminalProfile->setHttpCacheMaximumSize(4 * 1024 * 1024);
    m_terminalProfile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    m_terminalProfile->setSpellCheckEnabled(false);
    m_terminalProfile->setUrlRequestInterceptor(
        new LocalOnlyRequestInterceptor(m_terminalProfile));

    const QUrl entry = QUrl::fromLocalFile(entryPath);
    m_terminalPage = new TrustedWorkbenchPage(
        m_terminalProfile, entry, m_terminalView);
    QWebEnginePage *page = m_terminalPage;
    page->setBackgroundColor(QColor(QStringLiteral("#101828")));
    page->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    page->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    page->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    m_terminalView->setPage(page);

    m_terminalBridge = new TerminalWebBridge(this);
    auto *channel = new QWebChannel(page);
    channel->registerObject(QStringLiteral("aegisyTerminal"), m_terminalBridge);
    page->setWebChannel(channel);
    m_terminalStack->addWidget(m_terminalView);
    connect(m_terminalBridge, &TerminalWebBridge::terminalReady, this, [this]() {
        m_terminalWebReady = true;
        m_terminalStack->setCurrentWidget(m_terminalView);
        if (!m_activeTerminalId.isEmpty()) activateTerminal(m_activeTerminalId);
    });
    connect(m_terminalBridge, &TerminalWebBridge::inputRequested,
            this, [this](const QString &data) {
        if (m_terminalRunning && !m_terminalStopping && !m_activeTerminalId.isEmpty()
                && !m_workSessionId.isEmpty() && !m_runtimeRecoveryMode
                && !m_recoverySessionIds.contains(m_workSessionId)) {
            m_runtime->inputUserTerminal(
                m_workSessionId, m_activeTerminalId, data.toUtf8());
        }
    });
    connect(m_terminalBridge, &TerminalWebBridge::sizeRequested,
            this, [this](int rows, int cols) {
        if (m_terminalRunning && !m_terminalStopping && !m_activeTerminalId.isEmpty()
                && !m_workSessionId.isEmpty() && !m_runtimeRecoveryMode
                && !m_recoverySessionIds.contains(m_workSessionId)
                && rows > 0 && cols > 0) {
            m_runtime->resizeTerminal(m_workSessionId, m_activeTerminalId, rows, cols);
        }
    });
    connect(m_terminalBridge, &TerminalWebBridge::selectionUpdated,
            this, [this](const QString &selection) {
        m_terminalSelection = selection;
        updateTerminalControls();
    });
    connect(m_terminalBridge, &TerminalWebBridge::clipboardCopyRequested,
            this, [](const QString &text) {
        QApplication::clipboard()->setText(text, QClipboard::Clipboard);
    });
    connect(m_terminalBridge, &TerminalWebBridge::clipboardPasteRequested,
            this, [this]() {
        if (!m_terminalBridge || !m_terminalRunning || m_terminalStopping
                || m_runtimeRecoveryMode
                || m_recoverySessionIds.contains(m_workSessionId)) return;
        const QString text = boundedUtf8Text(
            QApplication::clipboard()->text(QClipboard::Clipboard), 64 * 1024);
        m_terminalBridge->pasteText(text);
    });
    connect(page, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        if (ok) return;
        m_terminalWebReady = false;
        m_terminalStack->setCurrentWidget(m_terminalExcerptPreview);
        m_terminalStatus->setText(QStringLiteral("xterm.js 加载失败，已切换到原生输出视图"));
    });
    connect(page, &QWebEnginePage::renderProcessTerminated,
            this, [this, entry, page](QWebEnginePage::RenderProcessTerminationStatus, int exitCode) {
        m_terminalWebReady = false;
        m_terminalStack->setCurrentWidget(m_terminalExcerptPreview);
        if (m_terminalRenderRestartAttempts >= 2) {
            m_terminalStatus->setText(
                QStringLiteral("终端渲染进程反复退出（%1），已保留原生输出").arg(exitCode));
            return;
        }
        ++m_terminalRenderRestartAttempts;
        m_terminalStatus->setText(
            QStringLiteral("终端渲染进程退出（%1），正在恢复...").arg(exitCode));
        QTimer::singleShot(250, page, [this, entry]() {
            if (m_terminalPage) m_terminalPage->load(entry);
        });
    });
    page->load(entry);
}

void AgentWorkbenchWidget::initializeMonacoEditor(QWidget *parent)
{
    const QString entryPath = monacoEntryPath();
    if (!QFileInfo::exists(entryPath)) {
        m_fileStatus->setText(QStringLiteral("Monaco 本地资源缺失，已使用 Qt 编辑器"));
        return;
    }

    m_monacoView = new QWebEngineView(m_editorStack);
    m_monacoView->setObjectName(QStringLiteral("agentMonacoEditor"));
    m_monacoView->setContextMenuPolicy(Qt::DefaultContextMenu);
    m_monacoProfile = new QWebEngineProfile(m_monacoView);
    m_monacoProfile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    m_monacoProfile->setHttpCacheMaximumSize(8 * 1024 * 1024);
    m_monacoProfile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    m_monacoProfile->setSpellCheckEnabled(false);
    m_monacoProfile->setUrlRequestInterceptor(
        new LocalOnlyRequestInterceptor(m_monacoProfile));

    const QUrl entry = QUrl::fromLocalFile(entryPath);
    m_monacoPage = new TrustedWorkbenchPage(m_monacoProfile, entry, m_monacoView);
    QWebEnginePage *page = m_monacoPage;
    page->setBackgroundColor(Qt::white);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    page->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    page->settings()->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    page->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    m_monacoView->setPage(page);

    m_monacoBridge = new MonacoEditorBridge(this);
    auto *channel = new QWebChannel(page);
    channel->registerObject(QStringLiteral("aegisyEditor"), m_monacoBridge);
    page->setWebChannel(channel);
    m_editorStack->addWidget(m_monacoView);

    connect(m_monacoBridge, &MonacoEditorBridge::editorReady, this, [this]() {
        m_monacoReady = true;
        m_editorSplitButton->setEnabled(!m_editorBuffers.isEmpty());
        if (m_editorSplitEnabled) setEditorSplitEnabled(true);
        else syncMonacoModel();
    });
    connect(m_monacoBridge, &MonacoEditorBridge::contentEdited,
            this, &AgentWorkbenchWidget::handleMonacoContent);
    connect(m_monacoBridge, &MonacoEditorBridge::editorViewChanged,
            this, &AgentWorkbenchWidget::handleMonacoView);
    connect(m_monacoBridge, &MonacoEditorBridge::editorGroupActivated,
            this, &AgentWorkbenchWidget::activateEditorGroup);
    connect(m_monacoBridge, &MonacoEditorBridge::saveInvoked,
            this, [this](int group, const QString &path) {
        activateEditorGroup(group, path);
        saveOpenFile();
    });
    connect(page, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        if (ok) return;
        m_monacoReady = false;
        m_editorSplitEnabled = false;
        m_editorSplitButton->setEnabled(false);
        m_editorSplitButton->setChecked(false);
        m_editorStack->setCurrentWidget(m_editor);
        m_fileStatus->setText(QStringLiteral("Monaco 加载失败，已切换到 Qt 编辑器"));
    });
    connect(page, &QWebEnginePage::renderProcessTerminated,
            this, [this](QWebEnginePage::RenderProcessTerminationStatus, int exitCode) {
        m_monacoReady = false;
        m_editorSplitEnabled = false;
        m_editorSplitButton->setEnabled(false);
        m_editorSplitButton->setChecked(false);
        m_editorStack->setCurrentWidget(m_editor);
        m_fileStatus->setText(QStringLiteral("Monaco 渲染进程退出（%1），已保留 Qt 编辑器内容")
                                  .arg(exitCode));
    });
    page->load(entry);
}

void AgentWorkbenchWidget::syncMonacoModel()
{
    syncMonacoModel(m_activeEditorGroup, m_openEditorPath);
    if (m_monacoReady && m_monacoBridge && !m_openEditorPath.isEmpty()) {
        m_monacoBridge->focusGroup(m_activeEditorGroup);
    }
}

void AgentWorkbenchWidget::syncMonacoModel(int group, const QString &path)
{
    if (!m_monacoReady || !m_monacoBridge || path.isEmpty()
            || !m_editorBuffers.contains(path)) {
        return;
    }
    const EditorBuffer &buffer = m_editorBuffers[path];
    if (buffer.fallback) {
        if (group == m_activeEditorGroup) m_editorStack->setCurrentWidget(m_editor);
        return;
    }
    const bool busy = m_editorLoading || !m_editorSaveRequestId.isEmpty();
    const bool withinLimit = buffer.content.toUtf8().size() <= 512 * 1024;
    const bool readOnly = !buffer.saveSupported || buffer.conflict || busy
        || !withinLimit;
    m_editorStack->setCurrentWidget(m_monacoView);
    m_monacoBridge->activateModel(
        group, path, buffer.content, readOnly,
        buffer.cursorPosition, buffer.anchorPosition,
        buffer.verticalScroll, buffer.horizontalScroll);
}

void AgentWorkbenchWidget::handleMonacoContent(const QString &path, const QString &content,
                                                int cursorPosition, int anchorPosition)
{
    if (!m_editorBuffers.contains(path)) return;
    EditorBuffer &buffer = m_editorBuffers[path];
    const bool changed = buffer.content != content;
    buffer.content = content;
    buffer.cursorPosition = qBound(0, cursorPosition, content.size());
    buffer.anchorPosition = qBound(0, anchorPosition, content.size());
    buffer.modified = buffer.modified || changed;
    const bool withinLimit = content.toUtf8().size() <= 512 * 1024;
    if (path != m_openEditorPath) {
        updateEditorTab(path);
        return;
    }

    m_monacoContentWithinLimit = withinLimit;
    {
        const QSignalBlocker blocker(m_editor->document());
        if (m_editor->toPlainText() != content) m_editor->setPlainText(content);
        QTextCursor cursor = m_editor->textCursor();
        cursor.setPosition(buffer.anchorPosition);
        cursor.setPosition(buffer.cursorPosition, QTextCursor::KeepAnchor);
        m_editor->setTextCursor(cursor);
        m_editor->document()->setModified(buffer.modified);
    }
    if (!withinLimit) {
        m_fileStatus->setText(QStringLiteral("编辑内容超过 512 KiB 安全保存上限"));
    } else if (!m_editorConflict) {
        m_fileStatus->setText(QStringLiteral("用户编辑可保存 · Agent 仍保持只读"));
    }
    updateEditorActions();
}

void AgentWorkbenchWidget::handleMonacoView(int group, const QString &path,
                                             int cursorPosition, int anchorPosition,
                                             int verticalScroll, int horizontalScroll)
{
    Q_UNUSED(group);
    if (!m_editorBuffers.contains(path)) return;
    EditorBuffer &buffer = m_editorBuffers[path];
    buffer.cursorPosition = qMax(0, cursorPosition);
    buffer.anchorPosition = qMax(0, anchorPosition);
    buffer.verticalScroll = qMax(0, verticalScroll);
    buffer.horizontalScroll = qMax(0, horizontalScroll);
    if (path == m_openEditorPath) updateEditorActions();
}
#endif

void AgentWorkbenchWidget::saveOpenFile()
{
    if (m_openEditorPath.isEmpty() || !m_editorSaveSupported || m_editorConflict
            || m_editorLoading || !m_editorSaveRequestId.isEmpty()
            || !m_editor->document()->isModified()) {
        return;
    }
#ifdef AEGISY_HAS_MONACO
    if (!m_monacoContentWithinLimit) {
        m_fileStatus->setText(QStringLiteral("编辑内容超过 512 KiB，无法保存"));
        return;
    }
#endif
    m_editorSaveRequestId = m_runtime->saveWorkspaceFile(
        m_projectId, m_openEditorPath, m_editor->toPlainText(), m_editorRevision,
        m_editorEncoding, m_editorNewline, m_workspaceRootId);
    if (m_editorSaveRequestId.isEmpty()) return;
    m_fileStatus->setText(QStringLiteral("正在验证版本并原子保存…"));
    updateEditorActions();
}

void AgentWorkbenchWidget::reloadOpenFile()
{
    if (m_openEditorPath.isEmpty() || m_editorLoading || !m_editorSaveRequestId.isEmpty()) return;
    if (m_editor->document()->isModified()) {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("放弃未保存修改？"),
            QStringLiteral("重新载入会放弃当前文件尚未保存的修改。"),
            QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) return;
    }
    m_editorLoading = true;
    m_editor->setReadOnly(true);
    m_fileStatus->setText(QStringLiteral("正在重新载入 %1…").arg(m_openEditorPath));
    updateEditorActions();
    const QString requestId = m_runtime->readWorkspaceFile(
        m_projectId, m_openEditorPath, m_workspaceRootId);
    if (!requestId.isEmpty()) m_workspaceReadRequests.insert(requestId, m_openEditorPath);
}

void AgentWorkbenchWidget::resetEditorModel()
{
    m_editorBuffers.clear();
    m_restoredEditorViews.clear();
#ifdef AEGISY_HAS_MONACO
    if (m_monacoBridge) {
        m_monacoBridge->setSplitEnabled(false);
        m_monacoBridge->closeAllModels();
    }
    m_monacoContentWithinLimit = true;
#endif
    m_editorRestoreRequests.clear();
    m_editorRestoreActivePath.clear();
    m_editorRestoreGroupPaths[0].clear();
    m_editorRestoreGroupPaths[1].clear();
    m_editorRestoreSplitEnabled = false;
    m_editorRestoreActiveGroup = 0;
    if (m_editorTabs) {
        const QSignalBlocker blocker(m_editorTabs);
        while (m_editorTabs->count() > 0) m_editorTabs->removeTab(0);
    }
    m_openEditorPath.clear();
    m_editorGroupPaths[0].clear();
    m_editorGroupPaths[1].clear();
    m_activeEditorGroup = 0;
    m_editorSplitEnabled = false;
    m_editorRevision.clear();
    m_editorEncoding.clear();
    m_editorNewline.clear();
    m_editorSaveRequestId.clear();
    m_editorSaveSupported = false;
    m_editorConflict = false;
    m_editorLoading = false;
    if (m_editor) {
        const QSignalBlocker blocker(m_editor->document());
        m_editor->clear();
        m_editor->document()->setModified(false);
        m_editor->setReadOnly(true);
    }
    if (m_editorPath) m_editorPath->setText(QStringLiteral("未打开文件"));
    if (m_editorMeta) m_editorMeta->setText(QStringLiteral("只读预览"));
    if (m_editorSplitButton) {
        const QSignalBlocker blocker(m_editorSplitButton);
        m_editorSplitButton->setChecked(false);
        m_editorSplitButton->setEnabled(false);
        m_editorSplitButton->setToolTip(QStringLiteral("拆分编辑器"));
    }
    updateEditorActions();
}

void AgentWorkbenchWidget::updateEditorActions()
{
    if (!m_editor || !m_editorSaveButton || !m_editorReloadButton) return;
    const bool dirty = m_editor->document()->isModified();
    const bool busy = m_editorLoading || !m_editorSaveRequestId.isEmpty();
    const bool storeRecovery = m_runtimeRecoveryMode;
    m_editor->setReadOnly(!m_editorSaveSupported || busy || storeRecovery);
    m_editorSaveButton->setEnabled(
        m_editorSaveSupported && dirty && !m_editorConflict && !busy && !storeRecovery
#ifdef AEGISY_HAS_MONACO
        && m_monacoContentWithinLimit
#endif
    );
    m_editorReloadButton->setEnabled(!m_openEditorPath.isEmpty() && !busy && !storeRecovery);
    const QString suffix = QFileInfo(m_openEditorPath).suffix().toLower();
    const bool languageSupported = QStringList{
        QStringLiteral("rs"), QStringLiteral("py"), QStringLiteral("pyi"),
        QStringLiteral("js"), QStringLiteral("jsx"), QStringLiteral("mjs"),
        QStringLiteral("cjs"), QStringLiteral("ts"), QStringLiteral("tsx"),
        QStringLiteral("mts"), QStringLiteral("cts"), QStringLiteral("c"),
        QStringLiteral("cc"), QStringLiteral("cpp"), QStringLiteral("cxx"),
        QStringLiteral("h"), QStringLiteral("hh"), QStringLiteral("hpp"),
        QStringLiteral("hxx")
    }.contains(suffix);
    const bool languageReady = languageSupported && !m_openEditorPath.isEmpty()
        && !m_editorBuffers.value(m_openEditorPath).fallback
        && m_languageRequestId.isEmpty() && !busy && !storeRecovery && m_runtime->isReady();
    if (m_languageDefinitionButton) m_languageDefinitionButton->setEnabled(languageReady);
    if (m_languageReferencesButton) m_languageReferencesButton->setEnabled(languageReady);
    if (m_languageDiagnosticsButton) m_languageDiagnosticsButton->setEnabled(languageReady);
    if (m_editorContextButton) {
        const EditorBuffer buffer = m_editorBuffers.value(m_openEditorPath);
        m_editorContextButton->setEnabled(!m_openEditorPath.isEmpty()
            && buffer.cursorPosition != buffer.anchorPosition);
    }
    if (m_pinSelectionContextAction) {
        const EditorBuffer buffer = m_editorBuffers.value(m_openEditorPath);
        m_pinSelectionContextAction->setEnabled(m_pinnedContextAvailable
            && !m_runtimeRecoveryMode && !m_projectId.isEmpty()
            && !currentSessionRecoveryRequired() && !currentSessionDeletionPending()
            && !currentOperationStatusBlocked()
            && m_pinnedContextMutationRequestId.isEmpty()
            && m_pinnedFileReadRequestId.isEmpty()
            && !m_openEditorPath.isEmpty()
            && buffer.cursorPosition != buffer.anchorPosition
            && !buffer.modified && !buffer.conflict);
    }
    if (m_languageStopButton
            && (!languageSupported || m_openEditorPath.isEmpty()
                || m_editorBuffers.value(m_openEditorPath).fallback
                || !m_runtime->isReady())) {
        m_languageStopButton->setEnabled(false);
    }
    if (!m_openEditorPath.isEmpty()) {
        if (m_editorBuffers.contains(m_openEditorPath)) {
            EditorBuffer &buffer = m_editorBuffers[m_openEditorPath];
            buffer.modified = dirty;
            buffer.conflict = m_editorConflict;
            buffer.saveSupported = m_editorSaveSupported;
        }
        const QString suffix = m_editorConflict ? QStringLiteral(" · 冲突")
            : dirty ? QStringLiteral(" · 未保存") : QString();
        m_editorPath->setText(breadcrumbForPath(m_openEditorPath) + suffix);
        m_editorPath->setToolTip(m_openEditorPath);
        updateEditorTab(m_openEditorPath);
#ifdef AEGISY_HAS_MONACO
        if (m_monacoReady && m_monacoBridge) {
            QSet<QString> updatedPaths;
            for (int group = 0; group < (m_editorSplitEnabled ? 2 : 1); ++group) {
                const QString path = m_editorGroupPaths[group];
                if (!m_editorBuffers.contains(path) || updatedPaths.contains(path)) continue;
                updatedPaths.insert(path);
                const EditorBuffer &groupBuffer = m_editorBuffers[path];
                m_monacoBridge->setModelReadOnly(
                    path,
                    !groupBuffer.saveSupported || groupBuffer.conflict || busy
                        || storeRecovery
                        || groupBuffer.content.toUtf8().size() > 512 * 1024);
            }
        }
#endif
    }
#ifdef AEGISY_HAS_MONACO
    if (m_editorSplitButton) {
        m_editorSplitButton->setEnabled(
            m_monacoReady && !m_editorBuffers.isEmpty()
                && !m_editorBuffers.value(m_openEditorPath).fallback);
    }
#endif
}

void AgentWorkbenchWidget::showEditorFallback(const QString &path,
                                              const QJsonObject &metadata,
                                              const QString &message)
{
    EditorBuffer buffer;
    buffer.saveSupported = false;
    buffer.fallback = true;
    const qint64 size = metadata.value(QStringLiteral("size")).toVariant().toLongLong();
    buffer.metadata = size > 0
        ? QStringLiteral("%1 字节 · 只读降级").arg(size)
        : QStringLiteral("只读降级");
    m_editorBuffers.insert(path, buffer);
    if (editorTabIndex(path) < 0) {
        m_editorTabs->addTab(QFileInfo(path).fileName());
        m_editorTabs->setTabData(m_editorTabs->count() - 1, path);
    }
    addRecentFile(path);
    m_editorLoading = false;
    activateEditorBuffer(path);
    m_fileStatus->setText(message.isEmpty()
        ? QStringLiteral("文件不支持安全文本编辑") : message);
    saveEditorViewState();
}

void AgentWorkbenchWidget::setMode(const QString &mode)
{
    if (mode == m_mode) return;
    m_mode = mode;
    updateContextStrip();
    updateTurnAction();
    updateTerminalControls();
    m_composer->setPlaceholderText(mode == QStringLiteral("work")
        ? QStringLiteral("描述要在当前项目中完成的工作…")
        : QStringLiteral("向 Aegisy Agent 发送消息…"));
    if (mode == QStringLiteral("work") && m_projectId.isEmpty()) {
        addNotice(QStringLiteral("Work 需要先打开一个项目文件夹。"));
    }
    m_composer->setFocus();
    requestOperationStatus();
}

void AgentWorkbenchWidget::requestOperationStatus()
{
    m_operationProbeRequestId.clear();
    m_operationReconcileRequestId.clear();
    m_operationReview = {};
    if (m_operationStatusBanner) m_operationStatusBanner->hide();
    if (m_operationStatusRow) m_operationStatusRow->hide();
    if (m_operationStatusReviewButton) m_operationStatusReviewButton->hide();
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    m_operationStatusSessionId = sessionId;
    m_operationStatusKnown = sessionId.isEmpty();
    m_operationStatusBlocked = false;
    if (!m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode || sessionId.isEmpty()) {
        updateRecoveryUi();
        return;
    }
    m_operationStatusRequestId = m_runtime->operationStatus(sessionId);
    if (m_operationStatusRequestId.isEmpty()) {
        m_operationStatusKnown = false;
        m_operationStatusBlocked = true;
        showOperationReviewFailure(
            QStringLiteral("运行时未接受 operation/status 请求，当前会话保持只读门控。"));
    }
    updateRecoveryUi();
}

void AgentWorkbenchWidget::updateOperationStatusUi(const QJsonObject &status)
{
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    const bool validSchema = status.value(QStringLiteral("schema_version")).toString()
        == QStringLiteral("operation-reconciliation-status/0.1");
    const bool validIdentity = status.value(QStringLiteral("session_id")).toString() == sessionId;
    const bool validFlags = status.value(QStringLiteral("blocked")).isBool()
        && status.value(QStringLiteral("recovery_action_available")).isBool()
        && !status.value(QStringLiteral("recovery_action_available")).toBool();
    const bool blocked = status.value(QStringLiteral("blocked")).toBool();
    const bool validOperation = !blocked || status.value(QStringLiteral("operation")).isObject();
    const bool valid = validSchema && validIdentity && validFlags && validOperation;
    if (!valid) {
        m_operationStatusKnown = false;
        m_operationStatusBlocked = true;
        m_operationReview = {};
        showOperationReviewFailure(
            QStringLiteral("运行时返回的 operation/status 版本或会话标识无效。"));
        updateRecoveryUi();
        return;
    }
    m_operationStatusKnown = true;
    m_operationStatusBlocked = status.value(QStringLiteral("blocked")).toBool();
    if (!m_operationStatusBlocked) {
        m_operationReview = {};
        if (m_operationStatusBanner) m_operationStatusBanner->hide();
        if (m_operationStatusRow) m_operationStatusRow->hide();
        if (m_operationStatusReviewButton) m_operationStatusReviewButton->hide();
        updateRecoveryUi();
        startPendingTurnIfReady();
        return;
    }
    const QJsonObject operation = status.value(QStringLiteral("operation")).toObject();
    m_operationReview = operation;
    const QString state = operation.value(QStringLiteral("state")).toString(
        QStringLiteral("unknown"));
    const QString decision = operation.value(QStringLiteral("decision")).toString(
        QStringLiteral("explicit-review-required"));
    QStringList blockers;
    for (const QJsonValue &value : operation.value(QStringLiteral("blockers")).toArray()) {
        const QString blocker = value.toString();
        if (!blocker.isEmpty()) blockers.append(blocker);
    }
    if (m_operationStatusBanner) {
        m_operationStatusBanner->setText(
            QStringLiteral("检测到未完成操作：当前会话写入已暂停（%1）。恢复动作暂不可用。")
                .arg(state));
    }
    QString tooltip = QStringLiteral("决策：%1\n恢复动作：不可用").arg(decision);
    if (!blockers.isEmpty()) tooltip += QStringLiteral("\n阻断原因：%1").arg(blockers.join(
        QStringLiteral("、")));
    const QString reviewId = operation.value(QStringLiteral("review_id")).toString();
    if (!reviewId.isEmpty()) tooltip += QStringLiteral("\n审核标识：%1").arg(reviewId);
    if (m_operationStatusRow) {
        m_operationStatusBanner->show();
        m_operationStatusBanner->setToolTip(tooltip);
        m_operationStatusRow->show();
    }
    updateOperationReviewButton();
    updateRecoveryUi();
}

void AgentWorkbenchWidget::reviewOperationStatus()
{
    if (!m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode
            || !m_operationStatusKnown || !m_operationStatusBlocked
            || !m_operationProbeRequestId.isEmpty()
            || !m_operationReconcileRequestId.isEmpty()) {
        return;
    }
    const QString sessionId = m_operationStatusSessionId;
    const QString operationId = m_operationReview.value(QStringLiteral("operation_id"))
        .toString();
    const QString kind = m_operationReview.value(QStringLiteral("kind")).toString();
    if (sessionId.isEmpty() || operationId.isEmpty() || kind != QStringLiteral("turn")) {
        addNotice(QStringLiteral(
            "当前操作类型暂不支持自动采集复核证据；未执行任何恢复动作。"), true);
        return;
    }
    m_operationProbeRequestId = m_runtime->operationProbe({
        {QStringLiteral("operation_id"), operationId},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("kind"), kind},
    });
    if (m_operationProbeRequestId.isEmpty()) {
        showOperationReviewFailure(
            QStringLiteral("无法启动 operation/probe，当前会话继续保持只读。"));
        return;
    }
    if (m_operationStatusBanner) {
        m_operationStatusBanner->setText(QStringLiteral(
            "正在采集受限运行时证据；在复核完成前不会恢复会话写入。"));
    }
    updateOperationReviewButton();
}

void AgentWorkbenchWidget::updateOperationReviewButton()
{
    if (!m_operationStatusReviewButton) return;
    const QString kind = m_operationReview.value(QStringLiteral("kind")).toString();
    const bool pending = !m_operationProbeRequestId.isEmpty()
        || !m_operationReconcileRequestId.isEmpty();
    const bool supported = m_operationStatusKnown && m_operationStatusBlocked
        && kind == QStringLiteral("turn")
        && !m_operationReview.value(QStringLiteral("operation_id")).toString().isEmpty();
    if (m_operationStatusRow && !m_operationStatusRow->isHidden()) {
        m_operationStatusReviewButton->show();
    } else {
        m_operationStatusReviewButton->hide();
    }
    m_operationStatusReviewButton->setText(pending
        ? QStringLiteral("正在检查…") : QStringLiteral("重新检查"));
    m_operationStatusReviewButton->setEnabled(supported && !pending
        && m_runtime && m_runtime->isReady() && !m_runtimeRecoveryMode);
    if (kind != QStringLiteral("turn") && !kind.isEmpty()) {
        m_operationStatusReviewButton->setToolTip(QStringLiteral(
            "当前仅支持对 turn 进行受限证据复核；不会执行恢复或 Git mutation"));
    }
}

void AgentWorkbenchWidget::beginBackgroundNotificationInspection(const QString &sessionId)
{
    if (sessionId.isEmpty() || !m_runtime || !m_runtime->isReady()
            || m_runtimeRecoveryMode || !m_backgroundNotificationInspectionAvailable
            || !m_backgroundNotificationRequestId.isEmpty()) {
        return;
    }
    if (m_backgroundNotificationDialog) delete m_backgroundNotificationDialog;
    m_backgroundNotificationSessionId = sessionId;
    m_backgroundNotificationCursor = {};
    m_backgroundNotificationRequestId = m_runtime->backgroundNotifications(sessionId, {}, 100);
    if (m_backgroundNotificationRequestId.isEmpty()) {
        m_backgroundNotificationSessionId.clear();
        addNotice(QStringLiteral("无法发起后台通知记录查询。"), true);
        return;
    }
    addNotice(QStringLiteral("正在读取后台通知记录…"));
}

void AgentWorkbenchWidget::showBackgroundNotificationPage(const QJsonObject &result)
{
    const QJsonValue nextCursorValue = result.value(QStringLiteral("next_cursor"));
    const QJsonArray notifications = result.value(QStringLiteral("notifications")).toArray();
    const QRegularExpression intentIdentityPattern(QStringLiteral(
        "^background-job-notification-intent:sha256:[0-9a-f]{64}$"));
    bool valid = result.value(QStringLiteral("schema_version")).toString()
            == QStringLiteral("background-notification-page/0.1")
        && result.value(QStringLiteral("session_id")).toString()
            == m_backgroundNotificationSessionId
        && result.value(QStringLiteral("notifications")).isArray()
        && result.value(QStringLiteral("content_included")).isBool()
        && !result.value(QStringLiteral("content_included")).toBool()
        && result.value(QStringLiteral("delivery_mutation_available")).isBool()
        && !result.value(QStringLiteral("delivery_mutation_available")).toBool()
        && result.value(QStringLiteral("platform_delivery_authority")).isBool()
        && !result.value(QStringLiteral("platform_delivery_authority")).toBool()
        && (nextCursorValue.isNull() || nextCursorValue.isObject());
    const QStringList allowedKinds{
        QStringLiteral("completed"),
        QStringLiteral("failed"),
        QStringLiteral("approval_needed"),
        QStringLiteral("budget_exhausted"),
    };
    const QStringList allowedStatuses{
        QStringLiteral("queued"), QStringLiteral("running"),
        QStringLiteral("pause_requested"), QStringLiteral("paused"),
        QStringLiteral("waiting_approval"), QStringLiteral("cancelling"),
        QStringLiteral("completed"), QStringLiteral("failed"),
        QStringLiteral("cancelled"), QStringLiteral("interrupted"),
    };
    if (nextCursorValue.isObject()) {
        const QJsonObject cursor = nextCursorValue.toObject();
        valid = valid
            && cursor.value(QStringLiteral("recorded_at_ms")).isDouble()
            && cursor.value(QStringLiteral("recorded_at_ms")).toVariant().toLongLong() > 0
            && intentIdentityPattern.match(
                cursor.value(QStringLiteral("intent_identity")).toString()).hasMatch();
    }
    if (notifications.isEmpty() && nextCursorValue.isObject()) valid = false;
    for (const QJsonValue &value : notifications) {
        const QJsonObject notification = value.toObject();
        const QJsonObject intent = notification.value(QStringLiteral("intent")).toObject();
        valid = valid && value.isObject()
            && notification.value(QStringLiteral("schema_version")).toString()
                == QStringLiteral("stored-background-notification/0.1")
            && intent.value(QStringLiteral("schema_version")).toString()
                == QStringLiteral("background-job-notification-intent/0.1")
            && intent.value(QStringLiteral("session_id")).toString()
                == m_backgroundNotificationSessionId
            && allowedKinds.contains(intent.value(QStringLiteral("kind")).toString())
            && allowedStatuses.contains(intent.value(QStringLiteral("job_status")).toString())
            && intent.value(QStringLiteral("job_generation")).isDouble()
            && intent.value(QStringLiteral("job_generation")).toVariant().toLongLong() >= 0
            && notification.value(QStringLiteral("event_sequence")).isDouble()
            && notification.value(QStringLiteral("event_sequence")).toVariant().toLongLong() > 0
            && intentIdentityPattern.match(
                intent.value(QStringLiteral("intent_identity")).toString()).hasMatch()
            && notification.value(QStringLiteral("delivery_state")).toString()
                == QStringLiteral("recorded")
            && notification.value(QStringLiteral("delivery_attempt_count")).toInt(-1) == 0
            && notification.value(QStringLiteral("recorded_at_ms")).isDouble()
            && notification.value(QStringLiteral("recorded_at_ms")).toVariant().toLongLong() > 0
            && intent.value(QStringLiteral("content_included")).isBool()
            && !intent.value(QStringLiteral("content_included")).toBool()
            && intent.value(QStringLiteral("delivery_available")).isBool()
            && !intent.value(QStringLiteral("delivery_available")).toBool()
            && intent.value(QStringLiteral("delivery_attempted")).isBool()
            && !intent.value(QStringLiteral("delivery_attempted")).toBool()
            && intent.value(QStringLiteral("platform_delivery_authority")).isBool()
            && !intent.value(QStringLiteral("platform_delivery_authority")).toBool()
            && !intent.contains(QStringLiteral("title"))
            && !intent.contains(QStringLiteral("body"))
            && !intent.contains(QStringLiteral("prompt"))
            && !intent.contains(QStringLiteral("result_content"))
            && !intent.contains(QStringLiteral("platform_payload"))
            && !notification.contains(QStringLiteral("title"))
            && !notification.contains(QStringLiteral("body"))
            && !notification.contains(QStringLiteral("prompt"))
            && !notification.contains(QStringLiteral("result_content"))
            && !notification.contains(QStringLiteral("platform_payload"));
    }
    if (!valid) {
        if (m_backgroundNotificationMoreButton) {
            m_backgroundNotificationMoreButton->setEnabled(
                !m_backgroundNotificationCursor.isEmpty());
        }
        addNotice(QStringLiteral("后台通知记录未通过版本、会话或只读权限校验。"), true);
        return;
    }

    if (!m_backgroundNotificationDialog) {
        auto *dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("agentBackgroundNotificationDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("后台通知记录"));
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);
        auto *table = new QTableWidget(0, 5, dialog);
        table->setObjectName(QStringLiteral("agentBackgroundNotificationTable"));
        table->setHorizontalHeaderLabels({
            QStringLiteral("类型"), QStringLiteral("作业状态"), QStringLiteral("记录时间"),
            QStringLiteral("代次"), QStringLiteral("投递状态"),
        });
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        layout->addWidget(table, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        auto *more = buttons->addButton(QStringLiteral("加载更多"), QDialogButtonBox::ActionRole);
        more->setObjectName(QStringLiteral("agentBackgroundNotificationMoreButton"));
        connect(more, &QPushButton::clicked, dialog, [this]() {
            if (!m_runtime || !m_runtime->isReady() || m_backgroundNotificationCursor.isEmpty()
                    || !m_backgroundNotificationRequestId.isEmpty()) return;
            m_backgroundNotificationRequestId = m_runtime->backgroundNotifications(
                m_backgroundNotificationSessionId, m_backgroundNotificationCursor, 100);
            if (m_backgroundNotificationRequestId.isEmpty()) {
                addNotice(QStringLiteral("无法加载更多后台通知记录。"), true);
                return;
            }
            if (m_backgroundNotificationMoreButton) {
                m_backgroundNotificationMoreButton->setEnabled(false);
            }
        });
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        connect(dialog, &QObject::destroyed, this, [this]() {
            m_backgroundNotificationDialog = nullptr;
            m_backgroundNotificationTable = nullptr;
            m_backgroundNotificationMoreButton = nullptr;
            m_backgroundNotificationRequestId.clear();
            m_backgroundNotificationSessionId.clear();
            m_backgroundNotificationCursor = {};
        });
        m_backgroundNotificationDialog = dialog;
        m_backgroundNotificationTable = table;
        m_backgroundNotificationMoreButton = more;
        dialog->resize(760, 420);
    }

    if (m_backgroundNotificationTable->rowCount() == 1
            && m_backgroundNotificationTable->item(0, 0)
            && m_backgroundNotificationTable->item(0, 0)->data(Qt::UserRole).toBool()) {
        m_backgroundNotificationTable->removeRow(0);
    }
    const QHash<QString, QString> kindLabels{
        {QStringLiteral("completed"), QStringLiteral("已完成")},
        {QStringLiteral("failed"), QStringLiteral("失败")},
        {QStringLiteral("approval_needed"), QStringLiteral("等待审批")},
        {QStringLiteral("budget_exhausted"), QStringLiteral("预算耗尽")},
    };
    const QHash<QString, QString> statusLabels{
        {QStringLiteral("queued"), QStringLiteral("排队")},
        {QStringLiteral("running"), QStringLiteral("运行中")},
        {QStringLiteral("pause_requested"), QStringLiteral("请求暂停")},
        {QStringLiteral("paused"), QStringLiteral("已暂停")},
        {QStringLiteral("waiting_approval"), QStringLiteral("等待审批")},
        {QStringLiteral("cancelling"), QStringLiteral("取消中")},
        {QStringLiteral("completed"), QStringLiteral("已完成")},
        {QStringLiteral("failed"), QStringLiteral("失败")},
        {QStringLiteral("cancelled"), QStringLiteral("已取消")},
        {QStringLiteral("interrupted"), QStringLiteral("已中断")},
    };
    for (const QJsonValue &value : notifications) {
        const QJsonObject notification = value.toObject();
        const QJsonObject intent = notification.value(QStringLiteral("intent")).toObject();
        const int row = m_backgroundNotificationTable->rowCount();
        m_backgroundNotificationTable->insertRow(row);
        const qint64 recordedAt = notification.value(
            QStringLiteral("recorded_at_ms")).toVariant().toLongLong();
        const QString tooltip = QStringLiteral("作业：%1\n事件序号：%2\nIntent：%3")
            .arg(intent.value(QStringLiteral("job_id")).toString())
            .arg(notification.value(QStringLiteral("event_sequence")).toVariant().toULongLong())
            .arg(intent.value(QStringLiteral("intent_identity")).toString());
        const QStringList cells{
            kindLabels.value(intent.value(QStringLiteral("kind")).toString()),
            statusLabels.value(intent.value(QStringLiteral("job_status")).toString()),
            QDateTime::fromMSecsSinceEpoch(recordedAt).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
            QString::number(intent.value(QStringLiteral("job_generation")).toVariant().toULongLong()),
            QStringLiteral("已记录"),
        };
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(tooltip);
            m_backgroundNotificationTable->setItem(row, column, item);
        }
    }
    if (m_backgroundNotificationTable->rowCount() == 0) {
        m_backgroundNotificationTable->insertRow(0);
        auto *empty = new QTableWidgetItem(QStringLiteral("暂无后台通知记录"));
        empty->setData(Qt::UserRole, true);
        empty->setTextAlignment(Qt::AlignCenter);
        m_backgroundNotificationTable->setItem(0, 0, empty);
        m_backgroundNotificationTable->setSpan(0, 0, 1, 5);
    }
    m_backgroundNotificationCursor = nextCursorValue.toObject();
    m_backgroundNotificationMoreButton->setVisible(!m_backgroundNotificationCursor.isEmpty());
    m_backgroundNotificationMoreButton->setEnabled(!m_backgroundNotificationCursor.isEmpty());
    m_backgroundNotificationDialog->show();
    m_backgroundNotificationDialog->raise();
    m_backgroundNotificationDialog->activateWindow();
}

void AgentWorkbenchWidget::beginBackgroundRecoveryInspection(const QString &sessionId)
{
    if (sessionId.isEmpty() || !m_runtime || !m_runtime->isReady()
            || m_runtimeRecoveryMode || !m_backgroundRecoveryInspectionAvailable
            || !m_backgroundRecoveryRequestId.isEmpty()) {
        return;
    }
    if (m_backgroundRecoveryDialog) delete m_backgroundRecoveryDialog;
    m_backgroundRecoverySessionId = sessionId;
    m_backgroundRecoveryCursor = {};
    m_backgroundRecoveryRequestId = m_runtime->backgroundRecovery(sessionId, {}, 100);
    if (m_backgroundRecoveryRequestId.isEmpty()) {
        m_backgroundRecoverySessionId.clear();
        addNotice(QStringLiteral("无法发起后台恢复状态查询。"), true);
        return;
    }
    addNotice(QStringLiteral("正在读取后台恢复状态…"));
}

void AgentWorkbenchWidget::showBackgroundRecoveryPage(const QJsonObject &result)
{
    const QJsonValue nextCursorValue = result.value(QStringLiteral("next_cursor"));
    const QJsonArray entries = result.value(QStringLiteral("entries")).toArray();
    const QRegularExpression snapshotPattern(QStringLiteral(
        "^background-job-scheduler-snapshot:sha256:[0-9a-f]{64}$"));
    const QRegularExpression entryPattern(QStringLiteral(
        "^background-job-scheduler-entry:sha256:[0-9a-f]{64}$"));
    const QRegularExpression decisionPattern(QStringLiteral(
        "^background-job-recovery-decision:sha256:[0-9a-f]{64}$"));
    const QStringList statuses{
        QStringLiteral("queued"), QStringLiteral("running"),
        QStringLiteral("pause_requested"), QStringLiteral("paused"),
        QStringLiteral("waiting_approval"), QStringLiteral("cancelling"),
        QStringLiteral("completed"), QStringLiteral("failed"),
        QStringLiteral("cancelled"), QStringLiteral("interrupted"),
    };
    const QStringList actions{
        QStringLiteral("await_schedule"), QStringLiteral("admission_review"),
        QStringLiteral("keep_paused"), QStringLiteral("keep_waiting_approval"),
        QStringLiteral("retry_review_required"), QStringLiteral("monitor_owned_process"),
        QStringLiteral("manual_reconciliation"), QStringLiteral("terminal_review"),
    };
    const QStringList leaseStates{
        QStringLiteral("missing"), QStringLiteral("current"),
        QStringLiteral("expired"), QStringLiteral("released"),
        QStringLiteral("state_stale"), QStringLiteral("owner_mismatch"),
    };
    const QStringList processStates{
        QStringLiteral("not_required"), QStringLiteral("missing_lease"),
        QStringLiteral("missing_registration"), QStringLiteral("observation_unavailable"),
        QStringLiteral("observed_not_running"), QStringLiteral("mismatched"),
        QStringLiteral("current"),
    };
    bool valid = result.value(QStringLiteral("schema_version")).toString()
            == QStringLiteral("background-recovery-page/0.1")
        && result.value(QStringLiteral("session_id")).toString()
            == m_backgroundRecoverySessionId
        && snapshotPattern.match(result.value(QStringLiteral("snapshot_identity")).toString())
            .hasMatch()
        && result.value(QStringLiteral("scheduler_generation")).toVariant().toULongLong() > 0
        && result.value(QStringLiteral("observed_at_ms")).toVariant().toULongLong() > 0
        && result.value(QStringLiteral("entries")).isArray()
        && result.value(QStringLiteral("content_included")).isBool()
        && !result.value(QStringLiteral("content_included")).toBool()
        && result.value(QStringLiteral("dispatch_available")).isBool()
        && !result.value(QStringLiteral("dispatch_available")).toBool()
        && result.value(QStringLiteral("automatic_retry")).isBool()
        && !result.value(QStringLiteral("automatic_retry")).toBool()
        && result.value(QStringLiteral("automatic_approval")).isBool()
        && !result.value(QStringLiteral("automatic_approval")).toBool()
        && result.value(QStringLiteral("automatic_takeover")).isBool()
        && !result.value(QStringLiteral("automatic_takeover")).toBool()
        && result.value(QStringLiteral("mutation_authority")).isBool()
        && !result.value(QStringLiteral("mutation_authority")).toBool()
        && (nextCursorValue.isNull() || nextCursorValue.isObject());
    if (nextCursorValue.isObject()) {
        const QJsonObject cursor = nextCursorValue.toObject();
        valid = valid && !cursor.value(QStringLiteral("job_id")).toString().isEmpty()
            && entryPattern.match(cursor.value(QStringLiteral("entry_identity")).toString())
                .hasMatch();
    }
    for (const QJsonValue &value : entries) {
        const QJsonObject wrapper = value.toObject();
        const QJsonObject entry = wrapper.value(QStringLiteral("entry")).toObject();
        const QJsonValue reviewValue = wrapper.value(QStringLiteral("review"));
        const QJsonObject review = reviewValue.toObject();
        valid = valid && value.isObject()
            && entryPattern.match(wrapper.value(QStringLiteral("entry_identity")).toString())
                .hasMatch()
            && !entry.value(QStringLiteral("job_id")).toString().isEmpty()
            && entry.value(QStringLiteral("session_id")).toString()
                == m_backgroundRecoverySessionId
            && !entry.value(QStringLiteral("project_id")).toString().isEmpty()
            && !entry.value(QStringLiteral("root_id")).toString().isEmpty()
            && statuses.contains(entry.value(QStringLiteral("status")).toString())
            && actions.contains(entry.value(QStringLiteral("action")).toString())
            && leaseStates.contains(entry.value(QStringLiteral("lease_state")).toString())
            && processStates.contains(
                entry.value(QStringLiteral("process_ownership_state")).toString())
            && entry.value(QStringLiteral("job_generation")).toVariant().toULongLong() >= 0
            && entry.value(QStringLiteral("blockers")).isArray()
            && !entry.value(QStringLiteral("dispatch_available")).toBool()
            && !entry.value(QStringLiteral("automatic_retry")).toBool()
            && !entry.value(QStringLiteral("automatic_approval")).toBool()
            && !entry.value(QStringLiteral("automatic_takeover")).toBool()
            && !entry.contains(QStringLiteral("prompt"))
            && !entry.contains(QStringLiteral("body"))
            && !entry.contains(QStringLiteral("result_content"))
            && !entry.contains(QStringLiteral("platform_payload"));
        if (!reviewValue.isNull()) {
            valid = valid && reviewValue.isObject()
                && review.value(QStringLiteral("schema_version")).toString()
                    == QStringLiteral("background-recovery-review/0.1")
                && decisionPattern.match(
                    review.value(QStringLiteral("decision_identity")).toString()).hasMatch()
                && review.value(QStringLiteral("event_sequence")).toVariant().toULongLong() > 0
                && !review.value(QStringLiteral("automatic_retry")).toBool()
                && !review.value(QStringLiteral("automatic_approval")).toBool()
                && !review.value(QStringLiteral("automatic_takeover")).toBool()
                && !review.value(QStringLiteral("dispatch_authority")).toBool()
                && !review.value(QStringLiteral("mutation_authority")).toBool();
        }
    }
    if (!valid) {
        if (m_backgroundRecoveryMoreButton) {
            m_backgroundRecoveryMoreButton->setEnabled(!m_backgroundRecoveryCursor.isEmpty());
        }
        addNotice(QStringLiteral("后台恢复状态未通过版本、会话或只读权限校验。"), true);
        return;
    }

    if (!m_backgroundRecoveryDialog) {
        auto *dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("agentBackgroundRecoveryDialog"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("后台恢复状态"));
        auto *layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(10);
        auto *table = new QTableWidget(0, 6, dialog);
        table->setObjectName(QStringLiteral("agentBackgroundRecoveryTable"));
        table->setHorizontalHeaderLabels({
            QStringLiteral("作业"), QStringLiteral("恢复结论"), QStringLiteral("状态"),
            QStringLiteral("租约"), QStringLiteral("进程归属"), QStringLiteral("阻塞原因"),
        });
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->hide();
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
        layout->addWidget(table, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        auto *more = buttons->addButton(QStringLiteral("加载更多"), QDialogButtonBox::ActionRole);
        more->setObjectName(QStringLiteral("agentBackgroundRecoveryMoreButton"));
        connect(more, &QPushButton::clicked, dialog, [this]() {
            if (!m_runtime || !m_runtime->isReady() || m_backgroundRecoveryCursor.isEmpty()
                    || !m_backgroundRecoveryRequestId.isEmpty()) return;
            m_backgroundRecoveryRequestId = m_runtime->backgroundRecovery(
                m_backgroundRecoverySessionId, m_backgroundRecoveryCursor, 100);
            if (m_backgroundRecoveryRequestId.isEmpty()) {
                addNotice(QStringLiteral("无法加载更多后台恢复状态。"), true);
                return;
            }
            if (m_backgroundRecoveryMoreButton) m_backgroundRecoveryMoreButton->setEnabled(false);
        });
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        connect(dialog, &QObject::destroyed, this, [this]() {
            m_backgroundRecoveryDialog = nullptr;
            m_backgroundRecoveryTable = nullptr;
            m_backgroundRecoveryMoreButton = nullptr;
            m_backgroundRecoveryRequestId.clear();
            m_backgroundRecoverySessionId.clear();
            m_backgroundRecoveryCursor = {};
        });
        m_backgroundRecoveryDialog = dialog;
        m_backgroundRecoveryTable = table;
        m_backgroundRecoveryMoreButton = more;
        dialog->resize(980, 440);
    }

    if (m_backgroundRecoveryTable->rowCount() == 1
            && m_backgroundRecoveryTable->item(0, 0)
            && m_backgroundRecoveryTable->item(0, 0)->data(Qt::UserRole).toBool()) {
        m_backgroundRecoveryTable->removeRow(0);
    }
    const QHash<QString, QString> actionLabels{
        {QStringLiteral("await_schedule"), QStringLiteral("等待计划")},
        {QStringLiteral("admission_review"), QStringLiteral("需要准入复核")},
        {QStringLiteral("keep_paused"), QStringLiteral("保持暂停")},
        {QStringLiteral("keep_waiting_approval"), QStringLiteral("等待审批")},
        {QStringLiteral("retry_review_required"), QStringLiteral("需要重试复核")},
        {QStringLiteral("monitor_owned_process"), QStringLiteral("仅监视归属进程")},
        {QStringLiteral("manual_reconciliation"), QStringLiteral("需要人工对账")},
        {QStringLiteral("terminal_review"), QStringLiteral("终态复核")},
    };
    for (const QJsonValue &value : entries) {
        const QJsonObject wrapper = value.toObject();
        const QJsonObject entry = wrapper.value(QStringLiteral("entry")).toObject();
        const QJsonObject review = wrapper.value(QStringLiteral("review")).toObject();
        const int row = m_backgroundRecoveryTable->rowCount();
        m_backgroundRecoveryTable->insertRow(row);
        QStringList blockers;
        for (const QJsonValue &blocker : entry.value(QStringLiteral("blockers")).toArray()) {
            blockers.append(blocker.toString());
        }
        const QString tooltip = QStringLiteral("项目：%1\n根：%2\n作业代次：%3\n快照：%4")
            .arg(entry.value(QStringLiteral("project_id")).toString())
            .arg(entry.value(QStringLiteral("root_id")).toString())
            .arg(entry.value(QStringLiteral("job_generation")).toVariant().toULongLong())
            .arg(result.value(QStringLiteral("snapshot_identity")).toString());
        const QString reviewText = review.isEmpty()
            ? QStringLiteral("未记录审查")
            : QStringLiteral("已记录：%1").arg(
                actionLabels.value(review.value(QStringLiteral("disposition")).toString()));
        const QStringList cells{
            entry.value(QStringLiteral("job_id")).toString(),
            reviewText,
            entry.value(QStringLiteral("status")).toString(),
            entry.value(QStringLiteral("lease_state")).toString(),
            entry.value(QStringLiteral("process_ownership_state")).toString(),
            blockers.join(QStringLiteral(", ")),
        };
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            item->setToolTip(tooltip);
            m_backgroundRecoveryTable->setItem(row, column, item);
        }
    }
    if (m_backgroundRecoveryTable->rowCount() == 0) {
        m_backgroundRecoveryTable->insertRow(0);
        auto *empty = new QTableWidgetItem(QStringLiteral("暂无后台恢复记录"));
        empty->setData(Qt::UserRole, true);
        empty->setTextAlignment(Qt::AlignCenter);
        m_backgroundRecoveryTable->setItem(0, 0, empty);
        m_backgroundRecoveryTable->setSpan(0, 0, 1, 6);
    }
    m_backgroundRecoveryCursor = nextCursorValue.toObject();
    m_backgroundRecoveryMoreButton->setVisible(!m_backgroundRecoveryCursor.isEmpty());
    m_backgroundRecoveryMoreButton->setEnabled(!m_backgroundRecoveryCursor.isEmpty());
    m_backgroundRecoveryDialog->show();
    m_backgroundRecoveryDialog->raise();
    m_backgroundRecoveryDialog->activateWindow();
}

void AgentWorkbenchWidget::beginCompactionCheckpoint(const QString &sessionId)
{
    if (sessionId.isEmpty() || !m_runtime || !m_runtime->isReady()
            || m_runtimeRecoveryMode || !m_compactionRequestId.isEmpty()) {
        return;
    }
    const CompactionReviewFormData form = promptCompactionReview(
        this,
        QStringLiteral("创建压缩审查记录"),
        QStringLiteral(
            "这是手动审查记录，不会压缩、删除或替换原始会话历史。激活和 provider compact 当前不可用。"),
        QStringLiteral("保存审查"));
    if (!form.error.isEmpty()) {
        addNotice(form.error, true);
        return;
    }
    if (!form.accepted) return;
    m_compactionSessionId = sessionId;
    m_compactionOperation = QStringLiteral("create");
    m_compactionRequestId = m_runtime->createCompactionCheckpoint(
        sessionId, form.checkpointId, form.preservationInstructions, form.summary);
    if (m_compactionRequestId.isEmpty()) {
        m_compactionOperation.clear();
        m_compactionSessionId.clear();
        addNotice(QStringLiteral("无法提交压缩审查记录。"), true);
        return;
    }
    addNotice(QStringLiteral("正在保存压缩审查记录…"));
}

void AgentWorkbenchWidget::beginCompactionCheckpointRead(const QString &sessionId)
{
    if (sessionId.isEmpty() || !m_runtime || !m_runtime->isReady()
            || m_runtimeRecoveryMode || !m_compactionRequestId.isEmpty()) {
        return;
    }
    bool accepted = false;
    const QString id = QInputDialog::getText(
        this, QStringLiteral("查看压缩审查"), QStringLiteral("审查 ID"),
        QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || id.isEmpty()) return;
    m_compactionSessionId = sessionId;
    m_compactionOperation = QStringLiteral("read");
    m_compactionRequestId = m_runtime->readCompactionCheckpoint(sessionId, id);
    if (m_compactionRequestId.isEmpty()) {
        m_compactionOperation.clear();
        m_compactionSessionId.clear();
        addNotice(QStringLiteral("无法读取压缩审查记录。"), true);
        return;
    }
    addNotice(QStringLiteral("正在读取压缩审查记录…"));
}

void AgentWorkbenchWidget::beginCompactionCheckpointRevision(
    const QString &sessionId, const QJsonObject &sourceReview)
{
    const QString sourceCheckpointId = sourceReview.value(QStringLiteral("checkpoint_id"))
        .toString();
    const QString sourceReviewId = sourceReview.value(QStringLiteral("review_id")).toString();
    if (sessionId.isEmpty() || sourceReview.value(QStringLiteral("session_id")).toString()
                != sessionId
            || sourceCheckpointId.isEmpty() || sourceReviewId.isEmpty()
            || !m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode
            || !m_compactionAvailable || !m_compactionRequestId.isEmpty()) {
        addNotice(QStringLiteral("当前压缩审查无法创建修订版。"), true);
        return;
    }
    const QString suggestedId = sourceCheckpointId.left(124) + QStringLiteral("-rev");
    const CompactionReviewFormData form = promptCompactionReview(
        this,
        QStringLiteral("编辑为新的压缩审查"),
        QStringLiteral(
            "编辑会创建新的不可变审查记录，并保留旧记录和修订关系。不会覆盖旧记录、激活上下文或调用 provider compact。"),
        QStringLiteral("保存修订版"),
        suggestedId,
        sourceReview.value(QStringLiteral("preservation_instructions")).toString(),
        sourceReview.value(QStringLiteral("summary")).toObject());
    if (!form.error.isEmpty()) {
        addNotice(form.error, true);
        return;
    }
    if (!form.accepted) return;
    if (form.checkpointId == sourceCheckpointId) {
        addNotice(QStringLiteral("修订版必须使用新的审查 ID。"), true);
        return;
    }
    m_compactionSessionId = sessionId;
    m_compactionOperation = QStringLiteral("revise");
    m_compactionRequestId = m_runtime->reviseCompactionCheckpoint(
        sessionId, sourceCheckpointId, sourceReviewId, form.checkpointId,
        form.preservationInstructions, form.summary);
    if (m_compactionRequestId.isEmpty()) {
        m_compactionOperation.clear();
        m_compactionSessionId.clear();
        addNotice(QStringLiteral("无法提交压缩审查修订版。"), true);
        return;
    }
    addNotice(QStringLiteral("正在保存压缩审查修订版…"));
}

void AgentWorkbenchWidget::showCompactionReview(const QJsonObject &result, bool replayed)
{
    const QJsonObject review = result.value(QStringLiteral("review")).toObject();
    const QString sessionId = m_compactionSessionId;
    const QString schema = result.value(QStringLiteral("schema_version")).toString();
    const bool revisionSchema = schema == QStringLiteral(
        "session-compaction-checkpoint-revise-result/0.1");
    const bool validSchema = schema == QStringLiteral(
        "session-compaction-checkpoint-create-result/0.1")
        || schema == QStringLiteral("session-compaction-checkpoint-read-result/0.1")
        || revisionSchema;
    const QJsonObject supersedes = result.value(QStringLiteral("supersedes")).toObject();
    const bool validSupersedes = supersedes.isEmpty()
        || (!supersedes.value(QStringLiteral("checkpoint_id")).toString().isEmpty()
            && !supersedes.value(QStringLiteral("review_id")).toString().isEmpty()
            && supersedes.value(QStringLiteral("checkpoint_id")).toString()
                != review.value(QStringLiteral("checkpoint_id")).toString()
            && supersedes.value(QStringLiteral("review_id")).toString()
                != review.value(QStringLiteral("review_id")).toString());
    const bool explicitUnavailable = result.value(QStringLiteral("activation_available")).isBool()
        && !result.value(QStringLiteral("activation_available")).toBool()
        && result.value(QStringLiteral("provider_compact_invoked")).isBool()
        && !result.value(QStringLiteral("provider_compact_invoked")).toBool();
    if (!validSchema || review.value(QStringLiteral("session_id")).toString() != sessionId
            || review.value(QStringLiteral("schema_version")).toString()
                != QStringLiteral("session-compaction/0.1")
            || review.value(QStringLiteral("review_id")).toString().isEmpty()
            || !explicitUnavailable || !validSupersedes
            || (revisionSchema && supersedes.isEmpty())) {
        m_compactionSessionId.clear();
        addNotice(QStringLiteral("压缩审查结果无效或包含不可用激活动作，未展示。"), true);
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("压缩审查记录"));
    dialog->resize(720, 560);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(8);
    auto *summary = new QPlainTextEdit(dialog);
    summary->setReadOnly(true);
    summary->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    QStringList lines{
        QStringLiteral("审查 ID：%1").arg(review.value(QStringLiteral("checkpoint_id")).toString()),
        QStringLiteral("会话：%1").arg(sessionId),
        QStringLiteral("事件边界：%1").arg(review.value(QStringLiteral("through_sequence"))
                                         .toVariant().toULongLong()),
        QStringLiteral("Review ID：%1").arg(review.value(QStringLiteral("review_id")).toString()),
        QStringLiteral("状态：%1").arg(review.value(QStringLiteral("state")).toString()),
        QStringLiteral("原始事件历史：保持权威"),
        QStringLiteral("激活：不可用 · Provider compact：未调用"),
    };
    if (replayed) lines.append(QStringLiteral("来源：已读取的持久化审查记录"));
    if (!supersedes.isEmpty()) {
        lines.append(QStringLiteral("修订自：%1").arg(
            supersedes.value(QStringLiteral("checkpoint_id")).toString()));
        lines.append(QStringLiteral("来源 Review ID：%1").arg(
            supersedes.value(QStringLiteral("review_id")).toString()));
    }
    const QString instructions = review.value(QStringLiteral("preservation_instructions"))
        .toString();
    if (!instructions.isEmpty()) {
        lines.append(QStringLiteral("保留指令："));
        lines.append(instructions);
    }
    const QJsonObject reviewSummary = review.value(QStringLiteral("summary")).toObject();
    const QList<QPair<QString, QString>> categories{
        {QStringLiteral("decisions"), QStringLiteral("决策")},
        {QStringLiteral("unresolved_tasks"), QStringLiteral("未解决任务")},
        {QStringLiteral("changed_files"), QStringLiteral("变更文件")},
        {QStringLiteral("commands"), QStringLiteral("命令")},
        {QStringLiteral("tests"), QStringLiteral("测试")},
        {QStringLiteral("failures"), QStringLiteral("失败")},
        {QStringLiteral("next_actions"), QStringLiteral("下一步")},
    };
    for (const auto &category : categories) {
        const QJsonArray values = reviewSummary.value(category.first).toArray();
        if (values.isEmpty()) continue;
        lines.append(QStringLiteral("%1：").arg(category.second));
        for (const QJsonValue &value : values) {
            lines.append(QStringLiteral("- %1").arg(value.toString()));
        }
    }
    const QJsonArray recovery = review.value(QStringLiteral("recovery_options")).toArray();
    if (!recovery.isEmpty()) {
        QStringList options;
        for (const QJsonValue &value : recovery) options.append(value.toString());
        lines.append(QStringLiteral("失败恢复选项：%1").arg(options.join(QStringLiteral("、"))));
    }
    summary->setPlainText(lines.join(QLatin1Char('\n')));
    layout->addWidget(summary, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto *revise = buttons->addButton(
        QStringLiteral("编辑为新审查"), QDialogButtonBox::ActionRole);
    revise->setObjectName(QStringLiteral("agentCompactionReviewReviseButton"));
    revise->setEnabled(m_compactionAvailable && !m_runtimeRecoveryMode
        && m_compactionRequestId.isEmpty());
    revise->setToolTip(QStringLiteral("创建新的不可变修订版；旧记录和原始事件历史保持不变"));
    connect(revise, &QPushButton::clicked, this, [this, dialog, sessionId, review]() {
        dialog->close();
        QTimer::singleShot(0, this, [this, sessionId, review]() {
            beginCompactionCheckpointRevision(sessionId, review);
        });
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog->show();
    m_compactionSessionId.clear();
    addNotice(QStringLiteral("压缩审查记录已读取；原始事件历史保持不变。"));
}

void AgentWorkbenchWidget::showOperationReviewFailure(const QString &message)
{
    m_operationStatusKnown = false;
    m_operationStatusBlocked = true;
    m_operationReview = {};
    if (m_operationStatusBanner) {
        m_operationStatusBanner->setText(message);
        m_operationStatusBanner->show();
        m_operationStatusBanner->setToolTip(QStringLiteral(
            "操作状态或复核结果不可验证；客户端不会推断成功，也不会执行恢复。"));
    }
    if (m_operationStatusRow) m_operationStatusRow->show();
    updateOperationReviewButton();
    updateRecoveryUi();
}

bool AgentWorkbenchWidget::currentOperationStatusBlocked() const
{
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    if (sessionId.isEmpty()) return false;
    return m_operationStatusSessionId != sessionId
        || !m_operationStatusKnown
        || m_operationStatusBlocked;
}

bool AgentWorkbenchWidget::currentSessionRecoveryRequired() const
{
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    return !sessionId.isEmpty() && m_recoverySessionIds.contains(sessionId);
}

bool AgentWorkbenchWidget::currentSessionDeletionPending() const
{
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    return !sessionId.isEmpty() && m_pendingDeletionSessionIds.contains(sessionId);
}

void AgentWorkbenchWidget::updateRecoveryUi()
{
    const bool sessionRecovery = currentSessionRecoveryRequired();
    const bool deletionPending = currentSessionDeletionPending();
    const bool operationBlocked = currentOperationStatusBlocked();
    const bool blocking = m_runtimeRecoveryMode || sessionRecovery || deletionPending
        || operationBlocked;
    if (m_recoveryBanner) {
        if (m_runtimeRecoveryMode) {
            m_recoveryBanner->setText(QStringLiteral(
                "工作台存储未能安全打开。当前仅提供只读诊断，项目、会话、终端和 Git 操作均已关闭。"));
            m_recoveryBanner->setStyleSheet(QStringLiteral(
                "QLabel { background:#fef3f2; color:#b42318; border-top:1px solid #fecdca;"
                "border-bottom:1px solid #fecdca; padding:8px 12px; font-size:10px; }"));
            m_recoveryBanner->show();
        } else if (sessionRecovery) {
            m_recoveryBanner->setText(QStringLiteral(
                "当前会话的事件或索引证据不完整，已隔离为只读。其他健康会话仍可继续使用。"));
            m_recoveryBanner->setStyleSheet(QStringLiteral(
                "QLabel { background:#fffaeb; color:#93370d; border-top:1px solid #fedf89;"
                "border-bottom:1px solid #fedf89; padding:8px 12px; font-size:10px; }"));
            m_recoveryBanner->show();
        } else if (deletionPending) {
            m_recoveryBanner->setText(QStringLiteral(
                "当前会话已安排删除并被冻结。历史仍可查看，可从左侧会话菜单撤销删除。"));
            m_recoveryBanner->setStyleSheet(QStringLiteral(
                "QLabel { background:#fef3f2; color:#b42318; border-top:1px solid #fecdca;"
                "border-bottom:1px solid #fecdca; padding:8px 12px; font-size:10px; }"));
            m_recoveryBanner->show();
        } else if (m_quarantinedSessionCount > 0) {
            m_recoveryBanner->setText(QStringLiteral(
                "检测到 %1 个只读隔离会话。可在左侧会话列表查看恢复状态。")
                .arg(m_quarantinedSessionCount));
            m_recoveryBanner->setStyleSheet(QStringLiteral(
                "QLabel { background:#fffaeb; color:#93370d; border-top:1px solid #fedf89;"
                "border-bottom:1px solid #fedf89; padding:8px 12px; font-size:10px; }"));
            m_recoveryBanner->show();
        } else if (m_startupRebuiltSessionCount > 0) {
            m_recoveryBanner->setText(QStringLiteral(
                "已从完整事件记录自动恢复 %1 个会话索引。")
                .arg(m_startupRebuiltSessionCount));
            m_recoveryBanner->setStyleSheet(QStringLiteral(
                "QLabel { background:#eff8ff; color:#175cd3; border-top:1px solid #b2ddff;"
                "border-bottom:1px solid #b2ddff; padding:8px 12px; font-size:10px; }"));
            m_recoveryBanner->show();
        } else {
            m_recoveryBanner->hide();
        }
    }
    if (m_newSessionButton) m_newSessionButton->setEnabled(!m_runtimeRecoveryMode);
    if (m_openProjectButton) m_openProjectButton->setEnabled(!m_runtimeRecoveryMode);
    if (m_runtimeRestartButton) {
        m_runtimeRestartButton->setEnabled(
            m_runtimeRestartRequired && !m_runtimeRecoveryMode);
    }
    if (m_retentionSettingsButton) {
        m_retentionSettingsButton->setEnabled(
            !m_runtimeRecoveryMode && !m_projectId.isEmpty());
    }
    if (m_importSessionButton) {
        m_importSessionButton->setEnabled(
            !m_runtimeRecoveryMode && m_runtime && m_runtime->isReady()
            && m_portableSessionRequestId.isEmpty());
    }
    if (m_modeGroup) {
        for (QAbstractButton *button : m_modeGroup->buttons()) {
            button->setEnabled(!m_runtimeRecoveryMode);
        }
    }
    if (m_composer) m_composer->setReadOnly(blocking);
    if (m_attachContextButton) m_attachContextButton->setEnabled(!blocking);
    if (m_pinFileContextAction) {
        m_pinFileContextAction->setEnabled(m_pinnedContextAvailable && !blocking
            && !m_projectId.isEmpty() && m_pinnedContextMutationRequestId.isEmpty()
            && m_pinnedFileReadRequestId.isEmpty());
    }
    if (m_pinSelectionContextAction) {
        m_pinSelectionContextAction->setEnabled(false);
    }
    if (m_workspaceTabs) m_workspaceTabs->setEnabled(!m_runtimeRecoveryMode);
    updateContextStrip();
    updateTurnAction();
    updateTerminalControls();
    updateEditorActions();
    updateGitPinControls();
}

void AgentWorkbenchWidget::requestProjectList()
{
    if (!m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode
            || !m_projectListRequestId.isEmpty()) {
        return;
    }
    m_projectListRequestId = m_runtime->listProjects();
}

void AgentWorkbenchWidget::populateProjectList(const QJsonObject &result)
{
    if (!m_projectList) return;
    const QString selectedId = m_projectId;
    m_projectList->clear();
    int selectedRow = -1;
    const QJsonArray projects = result.value(QStringLiteral("projects")).toArray();
    for (const QJsonValue &value : projects) {
        const QJsonObject project = value.toObject();
        const QString id = project.value(QStringLiteral("project_id"))
                               .toString(project.value(QStringLiteral("id")).toString());
        const QString root = project.value(QStringLiteral("root")).toString();
        const QString name = project.value(QStringLiteral("name"))
                                 .toString(QFileInfo(root).fileName());
        if (id.isEmpty() || root.isEmpty()) continue;
        const QString availability = project.value(QStringLiteral("availability"))
                                         .toString(QStringLiteral("unavailable"));
        const bool pinned = project.value(QStringLiteral("pinned")).toBool();
        const bool relinkRequired = project.value(QStringLiteral("relink_required")).toBool();
        const int liveSessions = project.value(QStringLiteral("live_session_count")).toInt();
        QString label = name.isEmpty() ? id : name;
        if (pinned) label.prepend(QStringLiteral("★ "));
        if (availability != QStringLiteral("available") || relinkRequired) {
            label += QStringLiteral(" · 不可用");
        } else if (liveSessions > 0) {
            label += QStringLiteral(" · 活跃 %1").arg(liveSessions);
        }
        auto *item = new QListWidgetItem(label);
        item->setData(kProjectIdRole, id);
        item->setData(kProjectRootRole, root);
        item->setData(kProjectAvailabilityRole, availability);
        item->setData(kProjectPinnedRole, pinned);
        item->setData(kProjectRelinkRole, relinkRequired);
        item->setToolTip(QStringLiteral("%1\n状态：%2\n会话：%3 · 活跃：%4")
                             .arg(root, availability,
                                  QString::number(project.value(QStringLiteral("session_count"))
                                                      .toInt()),
                                  QString::number(liveSessions)));
        if (availability != QStringLiteral("available") || relinkRequired) {
            item->setForeground(QColor(QStringLiteral("#b54708")));
        } else if (liveSessions > 0) {
            item->setForeground(QColor(QStringLiteral("#067647")));
        }
        m_projectList->addItem(item);
        if (id == selectedId) selectedRow = m_projectList->count() - 1;
    }
    if (m_projectList->count() == 0) {
        m_projectList->addItem(QStringLiteral("尚未打开项目"));
    } else if (selectedRow >= 0) {
        m_projectList->setCurrentRow(selectedRow);
    }
}

void AgentWorkbenchWidget::openProjectFromList(QListWidgetItem *item)
{
    if (!item || !m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode) return;
    const QString projectId = item->data(kProjectIdRole).toString();
    const QString root = item->data(kProjectRootRole).toString();
    if (projectId.isEmpty() || root.isEmpty() || projectId == m_projectId) return;
    storeActiveEditorState();
    bool hasUnsavedChanges = false;
    for (const EditorBuffer &buffer : m_editorBuffers) {
        hasUnsavedChanges = hasUnsavedChanges || buffer.modified;
    }
    if (hasUnsavedChanges) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("切换项目并放弃修改？"),
            QStringLiteral("当前标签中仍有未保存修改，切换项目会放弃这些内容。"),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) return;
    }
    saveEditorViewState();
    m_runtime->openProject(root);
}

void AgentWorkbenchWidget::chooseProject()
{
    if (m_runtimeRecoveryMode) {
        addNotice(QStringLiteral("存储处于只读恢复模式，暂不能打开项目。"), true);
        return;
    }
    storeActiveEditorState();
    bool hasUnsavedChanges = false;
    for (const EditorBuffer &buffer : m_editorBuffers) {
        hasUnsavedChanges = hasUnsavedChanges || buffer.modified;
    }
    if (hasUnsavedChanges) {
        const auto answer = QMessageBox::question(
            this,
            QStringLiteral("切换项目并放弃修改？"),
            QStringLiteral("当前标签中仍有未保存修改，切换项目会放弃这些内容。"),
            QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel);
        if (answer != QMessageBox::Discard) return;
    }
    saveEditorViewState();
    const QString root = QFileDialog::getExistingDirectory(
        this, QStringLiteral("打开项目文件夹"),
        m_projectRoot.isEmpty() ? QDir::homePath() : m_projectRoot);
    if (!root.isEmpty()) m_runtime->openProject(root);
}

void AgentWorkbenchWidget::beginProjectRootManagement()
{
    if (!m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode
            || m_projectId.isEmpty() || !m_projectRootsRequestId.isEmpty()
            || !m_projectRootMutationRequestId.isEmpty()) {
        return;
    }
    m_projectRootsRequestId = m_runtime->listProjectRoots(m_projectId);
    addNotice(QStringLiteral("正在读取项目根及独立访问范围…"));
}

void AgentWorkbenchWidget::showProjectRootsDialog(const QJsonObject &result)
{
    if (result.value(QStringLiteral("project_id")).toString() != m_projectId) return;
    const QJsonArray roots = result.value(QStringLiteral("roots")).toArray();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("管理项目根"));
    dialog.setMinimumSize(640, 360);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("项目根与访问范围"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size:15px; font-weight:700; color:#101828;"));
    layout->addWidget(title);
    auto *description = new QLabel(
        QStringLiteral("每个根拥有独立的只读或可写范围。新增根不会自动授予 Agent 访问权，主根不能移除。"),
        &dialog);
    description->setWordWrap(true);
    description->setStyleSheet(QStringLiteral("color:#667085; font-size:11px;"));
    layout->addWidget(description);

    auto *tree = new QTreeWidget(&dialog);
    tree->setColumnCount(3);
    tree->setHeaderLabels({QStringLiteral("路径"), QStringLiteral("范围"), QStringLiteral("状态")});
    tree->setRootIsDecorated(false);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    for (const QJsonValue &value : roots) {
        const QJsonObject root = value.toObject();
        auto *item = new QTreeWidgetItem(tree, {
            root.value(QStringLiteral("canonical_path")).toString(),
            root.value(QStringLiteral("access")).toString() == QStringLiteral("write")
                ? QStringLiteral("可写") : QStringLiteral("只读"),
            root.value(QStringLiteral("availability")).toString()
                == QStringLiteral("available") ? QStringLiteral("可用") : QStringLiteral("不可用"),
        });
        item->setData(0, Qt::UserRole, root.value(QStringLiteral("root_id")).toString());
        item->setData(0, Qt::UserRole + 1,
                      root.value(QStringLiteral("canonical_path")).toString());
        if (root.value(QStringLiteral("root_id")).toString() == QStringLiteral("root-1")) {
            item->setText(0, QStringLiteral("%1（主根）").arg(item->text(0)));
        }
    }
    layout->addWidget(tree, 1);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto *addRead = new QPushButton(QIcon(QStringLiteral(":/icons/lucide/plus.svg")),
                                    QStringLiteral("添加只读根"), &dialog);
    auto *addWrite = new QPushButton(QIcon(QStringLiteral(":/icons/lucide/plus.svg")),
                                     QStringLiteral("添加可写根"), &dialog);
    auto *remove = new QPushButton(QStringLiteral("移除所选根"), &dialog);
    remove->setEnabled(false);
    auto *activate = new QPushButton(QIcon(QStringLiteral(":/icons/lucide/folder-open.svg")),
                                     QStringLiteral("在文件树中打开"), &dialog);
    activate->setEnabled(false);
    connect(tree, &QTreeWidget::itemSelectionChanged, &dialog, [tree, remove]() {
        QTreeWidgetItem *item = tree->currentItem();
        remove->setEnabled(item && item->data(0, Qt::UserRole).toString()
                                     != QStringLiteral("root-1"));
    });
    connect(tree, &QTreeWidget::itemSelectionChanged, &dialog, [tree, activate]() {
        activate->setEnabled(tree->currentItem() != nullptr);
    });
    connect(activate, &QPushButton::clicked, &dialog, [this, &dialog, tree]() {
        QTreeWidgetItem *item = tree->currentItem();
        if (!item) return;
        if (m_editor->document()->isModified()) {
            const auto answer = QMessageBox::question(
                &dialog, QStringLiteral("切换文件根并放弃修改？"),
                QStringLiteral("当前编辑器有未保存修改，切换文件根会放弃这些修改。"),
                QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Discard) return;
        }
        const QString rootId = item->data(0, Qt::UserRole).toString();
        const QString path = item->data(0, Qt::UserRole + 1).toString();
        if (rootId.isEmpty() || path.isEmpty()) return;
        m_workspaceRootId = rootId;
        m_workspaceRootPath = path;
        m_workspaceWatchTimer->stop();
        m_workspaceWatchId.clear();
        m_watchedDirectories.clear();
        m_workspaceListRequests.clear();
        m_workspaceReadRequests.clear();
        m_workspaceMetadataRequests.clear();
        m_workspaceMetadataMessages.clear();
        m_workspaceSearchRequestId.clear();
        m_workspaceSearchId.clear();
        m_workspaceSearchCursor.clear();
        m_repositoryIndexRequestId.clear();
        m_repositoryIndexCancelRequestId.clear();
        m_repositoryMapRequestId.clear();
        m_repositoryIndexId.clear();
        m_languageServersRequestId.clear();
        m_languageRequestId.clear();
        m_diagnosticRawRequestId.clear();
        m_diagnosticRawReference.clear();
        m_repositoryIndexLoaded = false;
        m_repositoryIndexStale = false;
        m_treeItems.clear();
        m_fileTree->clear();
        resetEditorModel();
        m_fileStatus->setText(QStringLiteral("正在读取文件根：%1").arg(path));
        dialog.accept();
        requestDirectoryListing(QString());
        addNotice(QStringLiteral("文件树已切换到 %1（%2）；保存权限由该根的独立范围决定。")
                      .arg(QFileInfo(path).fileName(),
                           rootId == QStringLiteral("root-1") ? QStringLiteral("主根")
                                                                : QStringLiteral("额外根")));
    });
    auto addRoot = [this, &dialog](const QString &access) {
        const QString root = QFileDialog::getExistingDirectory(
            &dialog, access == QStringLiteral("write")
                ? QStringLiteral("选择可写项目根") : QStringLiteral("选择只读项目根"),
            m_projectRoot);
        if (root.isEmpty()) return;
        const QString scope = access == QStringLiteral("write")
            ? QStringLiteral("可写") : QStringLiteral("只读");
        const auto answer = QMessageBox::question(
            &dialog, QStringLiteral("确认添加项目根"),
            QStringLiteral("路径：%1\n范围：%2\n\n此操作只记录独立范围，不会自动授权 Agent 使用该目录。")
                .arg(QDir::toNativeSeparators(root), scope),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;
        m_projectRootMutationRequestId = m_runtime->addProjectRoot(m_projectId, root, access);
        dialog.accept();
    };
    connect(addRead, &QPushButton::clicked, &dialog,
            [addRoot]() { addRoot(QStringLiteral("read")); });
    connect(addWrite, &QPushButton::clicked, &dialog,
            [addRoot]() { addRoot(QStringLiteral("write")); });
    connect(remove, &QPushButton::clicked, &dialog, [this, &dialog, tree]() {
        QTreeWidgetItem *item = tree->currentItem();
        if (!item) return;
        const QString rootId = item->data(0, Qt::UserRole).toString();
        if (rootId.isEmpty() || rootId == QStringLiteral("root-1")) return;
        const auto answer = QMessageBox::warning(
            &dialog, QStringLiteral("移除项目根"),
            QStringLiteral("将从项目范围中移除：\n%1\n\n磁盘文件不会删除。")
                .arg(item->text(0)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) return;
        m_projectRootMutationRequestId = m_runtime->removeProjectRoot(m_projectId, rootId);
        dialog.accept();
    });
    actions->addWidget(addRead);
    actions->addWidget(addWrite);
    actions->addWidget(activate);
    actions->addWidget(remove);
    actions->addStretch();
    auto *close = new QPushButton(QStringLiteral("关闭"), &dialog);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    actions->addWidget(close);
    layout->addLayout(actions);
    dialog.exec();
}

void AgentWorkbenchWidget::requestSessionList()
{
    if (!m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode || !m_sessionList) {
        return;
    }
    if (!m_sessionListRequestId.isEmpty()) {
        m_sessionListRefreshPending = true;
        return;
    }
    const QString query = m_sessionSearchInput
        ? boundedUtf8Text(m_sessionSearchInput->text().trimmed(), 256)
        : QString();
    const QString requestId = query.isEmpty()
        ? m_runtime->listSessions(QString(), QString(), true, 100)
        : m_runtime->searchSessions(
              query,
              m_mode == QStringLiteral("work") ? m_projectId : QString(),
              true,
              100);
    if (!requestId.isEmpty()) m_sessionListRequestId = requestId;
}

void AgentWorkbenchWidget::beginSessionDeletion(QListWidgetItem *item)
{
    if (!item || !m_runtime || !m_runtime->isReady()
            || !m_sessionDeletionRequestId.isEmpty()) return;
    const QString sessionId = item->data(kSessionIdRole).toString();
    if (sessionId.isEmpty()) return;
    bool accepted = false;
    const QStringList choices{
        QStringLiteral("仅删除此会话"),
        QStringLiteral("删除此会话及其所有分支"),
    };
    const QString choice = QInputDialog::getItem(
        this, QStringLiteral("选择删除范围"),
        QStringLiteral("删除范围会在下一步显示精确影响。"),
        choices, 0, false, &accepted);
    if (!accepted) return;
    const QString scope = choice == choices.at(1)
        ? QStringLiteral("lineage") : QStringLiteral("session-only");
    m_sessionDeletionRequestId = m_runtime->previewSessionDeletion(sessionId, scope);
    if (!m_sessionDeletionRequestId.isEmpty()) {
        addNotice(QStringLiteral("正在核对会话、分支与产物影响…"));
    }
}

void AgentWorkbenchWidget::confirmSessionDeletion(const QJsonObject &preview)
{
    const QString sessionId = preview.value(QStringLiteral("root_session_id")).toString();
    const QString scope = preview.value(QStringLiteral("scope")).toString();
    const QJsonObject planHash = preview.value(QStringLiteral("plan_hash")).toObject();
    if (sessionId.isEmpty() || planHash.isEmpty()) {
        addNotice(QStringLiteral("删除预览缺少完整计划标识。"), true);
        return;
    }
    const QJsonArray blockers = preview.value(QStringLiteral("blocking_reasons")).toArray();
    if (!blockers.isEmpty()) {
        QStringList reasons;
        for (const QJsonValue &value : blockers) reasons.append(value.toString());
        QMessageBox::warning(
            this, QStringLiteral("当前不能删除会话"),
            QStringLiteral("运行时阻止了本次删除：\n%1\n\n请先结束活动任务、终端或恢复检查。")
                .arg(reasons.join(QLatin1Char('\n'))));
        return;
    }

    const qint64 sessionCount = preview.value(QStringLiteral("session_count"))
        .toVariant().toLongLong();
    const qint64 descendantCount = preview.value(QStringLiteral("descendant_count"))
        .toVariant().toLongLong();
    const qint64 turnCount = preview.value(QStringLiteral("turn_count"))
        .toVariant().toLongLong();
    const qint64 itemCount = preview.value(QStringLiteral("item_count"))
        .toVariant().toLongLong();
    const qint64 artifactCount = preview.value(QStringLiteral("artifact_reference_count"))
        .toVariant().toLongLong();
    const qint64 artifactBytes = preview.value(QStringLiteral("artifact_bytes"))
        .toVariant().toLongLong();
    QStringList affectedTitles;
    const QJsonArray affected = preview.value(QStringLiteral("affected_sessions")).toArray();
    for (int index = 0; index < affected.size() && index < 8; ++index) {
        const QJsonObject session = affected.at(index).toObject();
        affectedTitles.append(QStringLiteral("• %1")
            .arg(session.value(QStringLiteral("title")).toString(
                session.value(QStringLiteral("session_id")).toString())));
    }
    if (preview.value(QStringLiteral("affected_sessions_truncated")).toBool()) {
        affectedTitles.append(QStringLiteral("• 其余会话已省略显示"));
    }

    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(QStringLiteral("安排删除会话"));
    confirmation.setText(QStringLiteral("确认安排删除 %1 个会话？").arg(sessionCount));
    confirmation.setInformativeText(
        QStringLiteral(
            "范围：%1\n分支：%2\nTurn：%3 · 时间线项：%4\n产物：%5（%6）\n\n"
            "安排后会话立即冻结，但 7 天内仍可查看并撤销；到期后才清除内容，产物还会继续保留至少 24 小时。\n\n%7")
            .arg(scope == QStringLiteral("lineage")
                     ? QStringLiteral("此会话及全部分支")
                     : QStringLiteral("仅此会话"))
            .arg(descendantCount)
            .arg(turnCount)
            .arg(itemCount)
            .arg(artifactCount)
            .arg(formatByteCount(artifactBytes))
            .arg(affectedTitles.join(QLatin1Char('\n'))));
    QPushButton *schedule = confirmation.addButton(
        QStringLiteral("安排删除"), QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != schedule) return;

    constexpr qint64 kUndoWindowMs = 7LL * 24 * 60 * 60 * 1000;
    m_sessionDeletionRequestId = m_runtime->scheduleSessionDeletion(
        sessionId, scope, planHash, kUndoWindowMs);
}

void AgentWorkbenchWidget::beginPortableSessionExport(QListWidgetItem *item)
{
    if (!item || !m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode
            || !m_portableSessionRequestId.isEmpty()) return;
    const QString sessionId = item->data(kSessionIdRole).toString();
    if (sessionId.isEmpty()) return;
    m_portableSessionId = sessionId;
    m_portableSessionOperation = QStringLiteral("export-preview");
    m_portableSessionRequestId = m_runtime->previewPortableSessionExport(sessionId);
    if (!m_portableSessionRequestId.isEmpty()) {
        addNotice(QStringLiteral("正在核对会话导出内容与脱敏状态…"));
    }
}

void AgentWorkbenchWidget::confirmPortableSessionExport(const QJsonObject &preview)
{
    const QJsonArray blockers = preview.value(QStringLiteral("blocking_reasons")).toArray();
    if (!blockers.isEmpty()) {
        QStringList reasons;
        for (const QJsonValue &value : blockers) reasons.append(value.toString());
        QMessageBox::warning(
            this, QStringLiteral("当前不能导出会话"),
            QStringLiteral("运行时未能验证可导出的会话投影：\n%1")
                .arg(reasons.join(QLatin1Char('\n'))));
        m_portableSessionOperation.clear();
        return;
    }
    const QHash<QString, QString> categoryLabels{
        {QStringLiteral("session-metadata"), QStringLiteral("会话元数据")},
        {QStringLiteral("conversation-transcript"), QStringLiteral("对话记录")},
        {QStringLiteral("command-output"), QStringLiteral("命令输出")},
        {QStringLiteral("code-or-diff"), QStringLiteral("代码或 Diff")},
        {QStringLiteral("path-metadata"), QStringLiteral("路径元数据")},
    };
    const QHash<QString, QString> warningLabels{
        {QStringLiteral("portable-export-includes-transcript"),
         QStringLiteral("包含对话文本")},
        {QStringLiteral("portable-export-includes-command-output"),
         QStringLiteral("包含命令输出")},
        {QStringLiteral("portable-export-includes-code-or-diff"),
         QStringLiteral("包含代码或 Diff")},
        {QStringLiteral("portable-export-includes-paths"),
         QStringLiteral("包含相对路径信息")},
        {QStringLiteral("portable-export-redactions-applied"),
         QStringLiteral("已再次脱敏识别到的秘密")},
        {QStringLiteral("portable-export-local-fields-excluded"),
         QStringLiteral("已剔除本地产物及 provider opaque 字段")},
    };
    QStringList categories;
    for (const QJsonValue &value : preview.value(QStringLiteral("content_categories")).toArray()) {
        const QString code = value.toString();
        categories.append(categoryLabels.value(code, code));
    }
    QStringList warnings;
    for (const QJsonValue &value : preview.value(QStringLiteral("warnings")).toArray()) {
        const QString code = value.toString();
        warnings.append(warningLabels.value(code, code));
    }
    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(QStringLiteral("导出会话"));
    confirmation.setText(QStringLiteral("确认导出“%1”？")
        .arg(preview.value(QStringLiteral("title")).toString(
            preview.value(QStringLiteral("source_session_id")).toString())));
    confirmation.setInformativeText(
        QStringLiteral(
            "时间线项：%1 · 文件大小：%2\n内容：%3\n脱敏值：%4 · 剔除字段：%5\n\n%6")
            .arg(preview.value(QStringLiteral("item_count")).toVariant().toLongLong())
            .arg(formatByteCount(preview.value(QStringLiteral("package_bytes"))
                .toVariant().toLongLong()))
            .arg(categories.join(QStringLiteral("、")))
            .arg(preview.value(QStringLiteral("redacted_value_count"))
                .toVariant().toLongLong())
            .arg(preview.value(QStringLiteral("excluded_field_count"))
                .toVariant().toLongLong())
            .arg(warnings.join(QLatin1Char('\n'))));
    QPushButton *chooseFile = confirmation.addButton(
        QStringLiteral("选择保存位置"), QMessageBox::AcceptRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != chooseFile) {
        m_portableSessionOperation.clear();
        return;
    }

    QString fileName = preview.value(QStringLiteral("title")).toString(
        preview.value(QStringLiteral("source_session_id")).toString());
    fileName.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")),
                     QStringLiteral("_"));
    fileName = fileName.trimmed().left(80);
    if (fileName.isEmpty()) fileName = QStringLiteral("aegisy-session");
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出会话"),
        QDir::home().filePath(fileName + QStringLiteral(".aegisy-session.json")),
        QStringLiteral("Aegisy 会话 (*.aegisy-session.json);;JSON 文件 (*.json)"));
    if (path.isEmpty()) {
        m_portableSessionOperation.clear();
        return;
    }
    const QJsonObject packageHash = preview.value(QStringLiteral("package_hash")).toObject();
    if (packageHash.isEmpty()) {
        addNotice(QStringLiteral("导出预览缺少完整哈希。"), true);
        m_portableSessionOperation.clear();
        return;
    }
    m_portableSessionPath = path;
    m_portableSessionOperation = QStringLiteral("export");
    m_portableSessionRequestId = m_runtime->exportPortableSession(
        m_portableSessionId, packageHash);
}

void AgentWorkbenchWidget::beginPortableSessionImport()
{
    if (!m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode
            || !m_portableSessionRequestId.isEmpty()) return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入会话"), QDir::homePath(),
        QStringLiteral("Aegisy 会话 (*.aegisy-session.json *.json)"));
    if (path.isEmpty()) return;
    const QFileInfo info(path);
    if (!info.isFile() || info.size() <= 0 || info.size() > kMaxPortableSessionBytes) {
        QMessageBox::warning(
            this, QStringLiteral("无法导入会话"),
            QStringLiteral("会话包必须是非空文件，且不能超过 4 MiB。"));
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("无法导入会话"),
                             QStringLiteral("无法读取所选会话包。"));
        return;
    }
    const QByteArray bytes = file.read(kMaxPortableSessionBytes + 1);
    if (bytes.size() > kMaxPortableSessionBytes) {
        QMessageBox::warning(this, QStringLiteral("无法导入会话"),
                             QStringLiteral("会话包超过 4 MiB。"));
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        QMessageBox::warning(
            this, QStringLiteral("无法导入会话"),
            QStringLiteral("文件不是有效的 Aegisy JSON 会话包。"));
        return;
    }
    const QJsonObject package = document.object();
    const QString mode = package.value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("mode")).toString();
    QString targetProjectId;
    if (mode == QStringLiteral("work")) {
        if (m_projectId.isEmpty()) {
            QMessageBox::warning(
                this, QStringLiteral("需要目标项目"),
                QStringLiteral("Work 会话必须导入到一个已打开的项目。"));
            return;
        }
        bool projectAccepted = false;
        const QString projectLabel = m_projectLabel && !m_projectLabel->text().isEmpty()
            ? m_projectLabel->text() : m_projectId;
        QInputDialog::getItem(
            this, QStringLiteral("选择目标项目"), QStringLiteral("导入到"),
            {projectLabel}, 0, false, &projectAccepted);
        if (!projectAccepted) return;
        targetProjectId = m_projectId;
    }
    bool strategyAccepted = false;
    const QStringList strategies{
        QStringLiteral("冲突时创建副本"),
        QStringLiteral("检测到冲突时拒绝导入"),
    };
    const QString selectedStrategy = QInputDialog::getItem(
        this, QStringLiteral("选择碰撞处理"), QStringLiteral("源标识已存在时"),
        strategies, 0, false, &strategyAccepted);
    if (!strategyAccepted) return;
    const QString collisionStrategy = selectedStrategy == strategies.at(1)
        ? QStringLiteral("reject") : QStringLiteral("copy");

    m_portableSessionPackage = package;
    m_portableSessionPath = path;
    m_portableTargetProjectId = targetProjectId;
    m_portableCollisionStrategy = collisionStrategy;
    m_portableSessionOperation = QStringLiteral("import-preview");
    m_portableSessionRequestId = m_runtime->previewPortableSessionImport(
        package, targetProjectId, collisionStrategy);
    if (m_importSessionButton) m_importSessionButton->setEnabled(false);
    addNotice(QStringLiteral("正在验证会话包、内容类别与标识碰撞…"));
}

void AgentWorkbenchWidget::confirmPortableSessionImport(const QJsonObject &preview)
{
    const QJsonArray blockers = preview.value(QStringLiteral("blocking_reasons")).toArray();
    if (!blockers.isEmpty()) {
        QStringList reasons;
        const QHash<QString, QString> labels{
            {QStringLiteral("portable-import-project-required"),
             QStringLiteral("Work 会话缺少目标项目")},
            {QStringLiteral("portable-import-project-unavailable"),
             QStringLiteral("目标项目不可用或处于恢复隔离")},
            {QStringLiteral("portable-import-collision-rejected"),
             QStringLiteral("所选策略拒绝已存在的 Session/Item 标识")},
        };
        for (const QJsonValue &value : blockers) {
            const QString code = value.toString();
            reasons.append(labels.value(code, code));
        }
        QMessageBox::warning(
            this, QStringLiteral("当前不能导入会话"), reasons.join(QLatin1Char('\n')));
        m_portableSessionOperation.clear();
        if (m_importSessionButton) m_importSessionButton->setEnabled(true);
        return;
    }
    QStringList categories;
    const QHash<QString, QString> categoryLabels{
        {QStringLiteral("session-metadata"), QStringLiteral("会话元数据")},
        {QStringLiteral("conversation-transcript"), QStringLiteral("对话记录")},
        {QStringLiteral("command-output"), QStringLiteral("命令输出")},
        {QStringLiteral("code-or-diff"), QStringLiteral("代码或 Diff")},
        {QStringLiteral("path-metadata"), QStringLiteral("路径元数据")},
    };
    for (const QJsonValue &value : preview.value(QStringLiteral("content_categories")).toArray()) {
        const QString code = value.toString();
        categories.append(categoryLabels.value(code, code));
    }
    const qint64 itemCollisions = preview.value(QStringLiteral("source_item_id_collisions"))
        .toVariant().toLongLong();
    const bool sessionCollision = preview.value(QStringLiteral("source_session_collision"))
        .toBool();
    const bool remaps = preview.value(QStringLiteral("copy_will_remap_identifiers")).toBool();
    QString collisionSummary = sessionCollision || itemCollisions > 0
        ? QStringLiteral("源 Session：%1 · 源 Item：%2%3")
            .arg(sessionCollision ? QStringLiteral("已存在") : QStringLiteral("无碰撞"))
            .arg(itemCollisions)
            .arg(remaps ? QStringLiteral(" · 将创建副本并重映射") : QString())
        : QStringLiteral("未检测到源标识碰撞");
    QString target = preview.value(QStringLiteral("mode")).toString()
            == QStringLiteral("work")
        ? (m_projectLabel ? m_projectLabel->text() : m_portableTargetProjectId)
        : QStringLiteral("Chat（无项目绑定）");

    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Information);
    confirmation.setWindowTitle(QStringLiteral("导入会话"));
    confirmation.setText(QStringLiteral("确认导入“%1”？")
        .arg(preview.value(QStringLiteral("title")).toString(
            preview.value(QStringLiteral("source_session_id")).toString())));
    confirmation.setInformativeText(
        QStringLiteral("目标：%1\n时间线项：%2 · 文件大小：%3\n内容：%4\n碰撞：%5\n\n"
                       "导入仅携带脱敏后的便携历史，不携带 provider continuation。")
            .arg(target)
            .arg(preview.value(QStringLiteral("item_count")).toVariant().toLongLong())
            .arg(formatByteCount(preview.value(QStringLiteral("package_bytes"))
                .toVariant().toLongLong()))
            .arg(categories.join(QStringLiteral("、")))
            .arg(collisionSummary));
    QPushButton *importButton = confirmation.addButton(
        QStringLiteral("导入会话"), QMessageBox::AcceptRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != importButton) {
        m_portableSessionOperation.clear();
        if (m_importSessionButton) m_importSessionButton->setEnabled(true);
        return;
    }
    m_portableSessionOperation = QStringLiteral("import");
    m_portableSessionRequestId = m_runtime->importPortableSession(
        m_portableSessionPackage, m_portableTargetProjectId,
        m_portableCollisionStrategy);
}

void AgentWorkbenchWidget::beginRetentionPolicy(const QString &scopeKind,
                                                const QString &scopeId,
                                                const QString &scopeLabel)
{
    if (!m_runtime || !m_runtime->isReady() || m_runtimeRecoveryMode
            || scopeId.isEmpty() || !m_retentionPolicyRequestId.isEmpty()) return;
    m_retentionScopeKind = scopeKind;
    m_retentionScopeId = scopeId;
    m_retentionScopeLabel = scopeLabel;
    m_retentionPolicyRequestId = m_runtime->readRetentionPolicy(scopeKind, scopeId);
}

void AgentWorkbenchWidget::showRetentionPolicyDialog(const QJsonObject &result)
{
    if (result.value(QStringLiteral("scope_kind")).toString() != m_retentionScopeKind
            || result.value(QStringLiteral("scope_id")).toString() != m_retentionScopeId) {
        addNotice(QStringLiteral("保留策略响应与当前范围不匹配。"), true);
        return;
    }
    const QJsonObject policy = result.value(QStringLiteral("policy")).toObject();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("agentRetentionPolicyDialog"));
    dialog.setWindowTitle(m_retentionScopeKind == QStringLiteral("project")
        ? QStringLiteral("项目保留策略") : QStringLiteral("会话保留策略"));
    dialog.setMinimumWidth(430);
    auto *layout = new QVBoxLayout(&dialog);
    auto *scope = new QLabel(
        QStringLiteral("作用范围：%1").arg(m_retentionScopeLabel), &dialog);
    scope->setWordWrap(true);
    layout->addWidget(scope);

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(10);
    auto *archive = new QComboBox(&dialog);
    archive->setObjectName(QStringLiteral("agentRetentionArchive"));
    archive->addItem(QStringLiteral("不自动归档"), 0LL);
    archive->addItem(QStringLiteral("7 天未活动"), 7LL * 24 * 60 * 60 * 1000);
    archive->addItem(QStringLiteral("30 天未活动"), 30LL * 24 * 60 * 60 * 1000);
    archive->addItem(QStringLiteral("90 天未活动"), 90LL * 24 * 60 * 60 * 1000);
    archive->addItem(QStringLiteral("180 天未活动"), 180LL * 24 * 60 * 60 * 1000);
    auto *deletion = new QComboBox(&dialog);
    deletion->setObjectName(QStringLiteral("agentRetentionDelete"));
    deletion->addItem(QStringLiteral("不自动删除"), 0LL);
    deletion->addItem(QStringLiteral("归档后 30 天"), 30LL * 24 * 60 * 60 * 1000);
    deletion->addItem(QStringLiteral("归档后 90 天"), 90LL * 24 * 60 * 60 * 1000);
    deletion->addItem(QStringLiteral("归档后 180 天"), 180LL * 24 * 60 * 60 * 1000);
    deletion->addItem(QStringLiteral("归档后 365 天"), 365LL * 24 * 60 * 60 * 1000);
    auto *undo = new QComboBox(&dialog);
    undo->setObjectName(QStringLiteral("agentRetentionUndo"));
    undo->addItem(QStringLiteral("24 小时"), 1LL * 24 * 60 * 60 * 1000);
    undo->addItem(QStringLiteral("7 天"), 7LL * 24 * 60 * 60 * 1000);
    undo->addItem(QStringLiteral("30 天"), 30LL * 24 * 60 * 60 * 1000);
    auto *deleteScope = new QComboBox(&dialog);
    deleteScope->setObjectName(QStringLiteral("agentRetentionDeleteScope"));
    deleteScope->addItem(QStringLiteral("仅目标会话"), QStringLiteral("session-only"));
    deleteScope->addItem(QStringLiteral("目标会话及全部分支"), QStringLiteral("lineage"));

    const auto selectData = [](QComboBox *combo, const QVariant &value) {
        const int index = combo->findData(value);
        if (index >= 0) combo->setCurrentIndex(index);
    };
    if (!policy.isEmpty()) {
        selectData(archive, policy.value(QStringLiteral("archive_after_ms"))
                                .toVariant().toLongLong());
        selectData(deletion, policy.value(QStringLiteral("delete_after_ms"))
                                 .toVariant().toLongLong());
        selectData(undo, policy.value(QStringLiteral("undo_window_ms"))
                             .toVariant().toLongLong());
        selectData(deleteScope, policy.value(QStringLiteral("delete_scope")).toString());
    } else {
        selectData(archive, 30LL * 24 * 60 * 60 * 1000);
        selectData(deletion, 90LL * 24 * 60 * 60 * 1000);
        selectData(undo, 7LL * 24 * 60 * 60 * 1000);
    }
    grid->addWidget(new QLabel(QStringLiteral("自动归档"), &dialog), 0, 0);
    grid->addWidget(archive, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("自动删除"), &dialog), 1, 0);
    grid->addWidget(deletion, 1, 1);
    grid->addWidget(new QLabel(QStringLiteral("撤销期限"), &dialog), 2, 0);
    grid->addWidget(undo, 2, 1);
    grid->addWidget(new QLabel(QStringLiteral("删除范围"), &dialog), 3, 0);
    grid->addWidget(deleteScope, 3, 1);
    layout->addLayout(grid);

    auto *note = new QLabel(QStringLiteral(
        "自动删除只会先安排待删除状态。撤销期限结束后清除会话内容，关联产物仍至少保留 24 小时。"),
        &dialog);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#667085; font-size:11px;"));
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QPushButton *remove = buttons->addButton(
        QStringLiteral("移除自动策略"), QDialogButtonBox::DestructiveRole);
    remove->setEnabled(!policy.isEmpty());
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(remove, &QPushButton::clicked, &dialog, [&dialog]() { dialog.done(2); });
    layout->addWidget(buttons);

    while (true) {
        const int choice = dialog.exec();
        if (choice == QDialog::Rejected) return;
        if (choice == 2) {
            m_retentionPolicyRequestId = m_runtime->removeRetentionPolicy(
                m_retentionScopeKind, m_retentionScopeId);
            return;
        }
        const qint64 archiveAfter = archive->currentData().toLongLong();
        const qint64 deleteAfter = deletion->currentData().toLongLong();
        if (archiveAfter == 0 && deleteAfter == 0) {
            QMessageBox::warning(
                &dialog, QStringLiteral("策略没有动作"),
                QStringLiteral("请至少启用自动归档或自动删除；要禁用策略请使用“移除自动策略”。"));
            continue;
        }
        QJsonObject update{
            {QStringLiteral("scope_kind"), m_retentionScopeKind},
            {QStringLiteral("scope_id"), m_retentionScopeId},
            {QStringLiteral("undo_window_ms"), undo->currentData().toLongLong()},
            {QStringLiteral("delete_scope"), deleteScope->currentData().toString()},
        };
        if (archiveAfter > 0) {
            update.insert(QStringLiteral("archive_after_ms"), archiveAfter);
        }
        if (deleteAfter > 0) {
            update.insert(QStringLiteral("delete_after_ms"), deleteAfter);
        }
        m_retentionPolicyRequestId = m_runtime->setRetentionPolicy(update);
        return;
    }
}

void AgentWorkbenchWidget::populateSessionList(const QJsonObject &result)
{
    if (!m_sessionList) return;
    const QString selectedId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    m_sessionList->clear();
    m_archivedSessionIds.clear();
    m_recoverySessionIds.clear();
    m_pendingDeletionSessionIds.clear();
    int selectedRow = -1;
    const QJsonArray sessions = result.value(QStringLiteral("sessions")).toArray();
    auto appendSession = [this, &selectedId, &selectedRow](const QJsonObject &session) {
        const QString id = session.value(QStringLiteral("session_id")).toString();
        const QString mode = session.value(QStringLiteral("mode")).toString();
        const QString projectId = session.value(QStringLiteral("project_id")).toString();
        if (id.isEmpty() || mode.isEmpty()) return;
        if (!projectId.isEmpty() && projectId != m_projectId) return;
        if (mode == QStringLiteral("work") && projectId.isEmpty()) return;
        const QString title = session.value(QStringLiteral("title")).toString(id);
        const QString display = title == id ? id : QStringLiteral("%1 · %2").arg(title, id);
        const QString status = session.value(QStringLiteral("status")).toString();
        const bool recoveryRequired = session.value(
            QStringLiteral("recovery_required")).toBool();
        const bool deletionPending = session.value(
            QStringLiteral("deletion_pending")).toBool();
        const QJsonObject deletion = session.value(QStringLiteral("deletion")).toObject();
        const QJsonObject runtime = session.value(QStringLiteral("runtime")).toObject();
        if (!runtime.isEmpty()) storeSessionRuntimeBinding(id, runtime);
        const QJsonObject workspace = session.value(QStringLiteral("workspace")).toObject();
        storeSessionWorkspaceBinding(id, workspace);
        const QJsonArray matchedFields = session.value(QStringLiteral("matched_fields")).toArray();
        if (status == QStringLiteral("archived")) m_archivedSessionIds.insert(id);
        if (recoveryRequired) m_recoverySessionIds.insert(id);
        if (deletionPending) m_pendingDeletionSessionIds.insert(id);
        QString label = QStringLiteral("%1 · %2")
            .arg(mode == QStringLiteral("work") ? QStringLiteral("项目任务")
                                                  : QStringLiteral("新对话"), display);
        if (status == QStringLiteral("archived")) label += QStringLiteral(" · 已归档");
        if (recoveryRequired) label += QStringLiteral(" · 只读恢复");
        if (deletionPending) label += QStringLiteral(" · 待删除");
        auto *item = new QListWidgetItem(label);
        item->setData(kSessionIdRole, id);
        item->setData(kSessionModeRole, mode);
        item->setData(kSessionProjectRole, projectId);
        item->setData(kSessionStatusRole, status);
        item->setData(kSessionTitleRole, title);
        item->setData(kSessionRecoveryRole, recoveryRequired);
        item->setData(kSessionDeletionPendingRole, deletionPending);
        item->setData(kSessionDeletionIdRole,
                      deletion.value(QStringLiteral("deletion_id")).toString());
        const qint64 undoUntilMs = deletion.value(QStringLiteral("undo_until_ms"))
            .toVariant().toLongLong();
        item->setData(kSessionDeletionUndoUntilRole, undoUntilMs);
        QString tooltip = QStringLiteral("%1\n状态：%2%3")
            .arg(id, status, recoveryRequired ? QStringLiteral("\n需要恢复检查") : QString());
        const QString adapter = runtime.value(QStringLiteral("adapter")).toString();
        const QString model = runtime.value(QStringLiteral("model")).toString();
        if (!adapter.isEmpty()) tooltip += QStringLiteral("\n运行时：%1").arg(adapter);
        if (!model.isEmpty()) tooltip += QStringLiteral("\n模型：%1").arg(model);
        if (!matchedFields.isEmpty()) {
            QStringList labels;
            for (const QJsonValue &field : matchedFields) {
                const QString value = field.toString();
                if (value == QStringLiteral("title")) labels.append(QStringLiteral("标题"));
                else if (value == QStringLiteral("text")) labels.append(QStringLiteral("对话"));
                else if (value == QStringLiteral("model")) labels.append(QStringLiteral("模型"));
                else if (value == QStringLiteral("runtime")) labels.append(QStringLiteral("运行时"));
            }
            if (!labels.isEmpty()) tooltip += QStringLiteral("\n匹配：%1").arg(labels.join(QStringLiteral("、")));
        }
        if (deletionPending) {
            tooltip += QStringLiteral("\n删除待执行 · 可撤销至 %1")
                .arg(QDateTime::fromMSecsSinceEpoch(undoUntilMs)
                         .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        }
        item->setToolTip(tooltip);
        if (recoveryRequired) {
            item->setForeground(QColor(QStringLiteral("#b54708")));
        } else if (deletionPending) {
            item->setForeground(QColor(QStringLiteral("#b42318")));
        } else if (status == QStringLiteral("archived")) {
            item->setForeground(QColor(QStringLiteral("#98a2b3")));
        }
        m_sessionList->addItem(item);
        if (id == selectedId) selectedRow = m_sessionList->count() - 1;
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (const QJsonValue &value : sessions) {
            const QJsonObject session = value.toObject();
            const QString mode = session.value(QStringLiteral("mode")).toString();
            const QString projectId = session.value(QStringLiteral("project_id")).toString();
            const bool preferred = mode == QStringLiteral("work")
                && projectId == m_projectId && !projectId.isEmpty();
            if (preferred != (pass == 0)) continue;
            appendSession(session);
        }
    }
    if (m_sessionList->count() == 0) {
        m_sessionList->addItem(m_sessionSearchInput
                && !m_sessionSearchInput->text().trimmed().isEmpty()
            ? QStringLiteral("暂无匹配会话")
            : QStringLiteral("暂无可恢复会话"));
    } else if (selectedRow >= 0) {
        m_sessionList->setCurrentRow(selectedRow);
    }
    updateRecoveryUi();
}

void AgentWorkbenchWidget::loadSessionFromList(QListWidgetItem *item)
{
    if (!item || m_turnRunning || !m_runtime->isReady()) return;
    const QString sessionId = item->data(kSessionIdRole).toString();
    const QString mode = item->data(kSessionModeRole).toString();
    const QString projectId = item->data(kSessionProjectRole).toString();
    const bool recoveryRequired = item->data(kSessionRecoveryRole).toBool();
    if (sessionId.isEmpty() || mode.isEmpty()) return;
    if (mode == QStringLiteral("work") && projectId != m_projectId) {
        addNotice(QStringLiteral("该 Work 会话属于其他项目，请先打开对应文件夹。"), true);
        return;
    }
    for (QAbstractButton *button : m_modeGroup->buttons()) {
        if (button->property("mode").toString() == mode) button->setChecked(true);
    }
    setMode(mode);
    if (mode == QStringLiteral("work")) m_workSessionId = sessionId;
    else {
        m_chatSessionId = sessionId;
        m_chatSessionProjectId = projectId;
    }
    requestOperationStatus();
    if (recoveryRequired) {
        m_recoverySessionIds.insert(sessionId);
        updateRecoveryUi();
        m_runtime->sessionRecoveryStatus(sessionId);
        addNotice(QStringLiteral("该会话的持久化索引不可信，已保持只读。"), true);
        return;
    }
    if (!m_sessionReadRequestId.isEmpty()) return;
    resetSessionHistoryPagination();
    const QString requestId = m_runtime->readSession(sessionId);
    if (requestId.isEmpty()) return;
    m_sessionReadRequestId = requestId;
    addNotice(QStringLiteral("正在恢复会话…"));
}

void AgentWorkbenchWidget::loadOlderSessionHistory()
{
    if (!m_runtime || !m_runtime->isReady() || m_sessionHistoryId.isEmpty()
            || m_sessionHistoryCursor.isEmpty() || !m_sessionReadRequestId.isEmpty()) {
        return;
    }
    QScrollBar *bar = m_timelineScroll ? m_timelineScroll->verticalScrollBar() : nullptr;
    m_sessionHistoryScrollValue = bar ? bar->value() : 0;
    m_sessionHistoryScrollMaximum = bar ? bar->maximum() : 0;
    m_sessionHistoryAppending = true;
    m_sessionHistoryMoreButton->setText(QStringLiteral("正在加载…"));
    m_sessionHistoryMoreButton->setEnabled(false);
    const QString requestId = m_runtime->readSession(
        m_sessionHistoryId, m_sessionHistoryCursor, 100);
    if (requestId.isEmpty()) {
        m_sessionHistoryAppending = false;
        m_sessionHistoryMoreButton->setText(QStringLiteral("加载更早记录"));
        m_sessionHistoryMoreButton->setEnabled(true);
        return;
    }
    m_sessionReadRequestId = requestId;
}

void AgentWorkbenchWidget::resetSessionHistoryPagination()
{
    m_sessionHistoryId.clear();
    m_sessionHistoryCursor.clear();
    m_sessionHistoryAppending = false;
    m_sessionHistoryScrollValue = 0;
    m_sessionHistoryScrollMaximum = 0;
    if (m_sessionHistoryMoreButton) {
        m_sessionHistoryMoreButton->setText(QStringLiteral("加载更早记录"));
        m_sessionHistoryMoreButton->setEnabled(false);
        m_sessionHistoryMoreButton->hide();
    }
}

void AgentWorkbenchWidget::submitPrompt()
{
    const QString prompt = m_composer->toPlainText().trimmed();
    if (prompt.isEmpty()) return;
    if (!m_runtime->isReady()) {
        addNotice(QStringLiteral("本地运行时尚未就绪。"), true);
        return;
    }
    if (m_mode == QStringLiteral("work") && m_projectId.isEmpty()) {
        addNotice(QStringLiteral("请先打开项目文件夹，再开始 Work。"), true);
        return;
    }
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    if (m_runtimeRecoveryMode || currentSessionRecoveryRequired()) {
        addNotice(QStringLiteral("当前会话处于只读恢复状态，不能发送新任务。"), true);
        return;
    }
    if (currentSessionDeletionPending()) {
        addNotice(QStringLiteral("该会话已安排删除，请先从会话菜单撤销删除。"), true);
        return;
    }
    if (currentOperationStatusBlocked()) {
        addNotice(QStringLiteral(
            "当前会话的操作状态尚未完成审查，已保持只读；恢复动作暂不可用。"), true);
        return;
    }
    if (!sessionId.isEmpty() && m_archivedSessionIds.contains(sessionId)) {
        addNotice(QStringLiteral("该会话已归档，请右键恢复或新建会话后再发送。"), true);
        return;
    }
    const QJsonArray context = includedTurnContext();
    const QStringList pinnedContextIds = includedPinnedContextIds();
    if (!pinnedContextIds.isEmpty() && m_pinnedContextSetIdentity.isEmpty()) {
        addNotice(QStringLiteral("固定上下文集合身份未知，请刷新项目后重试。"), true);
        return;
    }
    m_composer->clear();
    m_sendButton->setEnabled(false);
    ensureSessionAndSubmit(prompt, context, m_pinnedContextSetIdentity, pinnedContextIds);
}

void AgentWorkbenchWidget::startPendingTurnIfReady()
{
    if (m_pendingPrompt.isEmpty() || !m_runtime || !m_runtime->isReady()
            || m_runtimeRecoveryMode || currentOperationStatusBlocked()) {
        return;
    }
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    if (sessionId.isEmpty()) return;
    const QString prompt = m_pendingPrompt;
    const QJsonArray context = m_pendingContext;
    const QString pinnedContextSetIdentity = m_pendingPinnedContextSetIdentity;
    const QStringList pinnedContextIds = m_pendingPinnedContextIds;
    m_pendingPrompt.clear();
    m_pendingContext = {};
    m_pendingPinnedContextSetIdentity.clear();
    m_pendingPinnedContextIds.clear();
    m_runtime->startTurn(sessionId, prompt, context,
                         pinnedContextSetIdentity, pinnedContextIds);
}

void AgentWorkbenchWidget::cancelActiveTurn()
{
    if (!m_turnRunning || m_turnCancelling || m_activeTurnSessionId.isEmpty()
            || m_activeTurnId.isEmpty() || !m_runtime->isReady()) {
        return;
    }
    m_turnCancelRequestId = m_runtime->cancelTurn(m_activeTurnSessionId, m_activeTurnId);
    if (m_turnCancelRequestId.isEmpty()) return;
    m_turnCancelling = true;
    updateTurnAction();
}

void AgentWorkbenchWidget::updateTurnAction()
{
    if (!m_sendButton) return;
    if (m_turnRunning) {
        m_sendButton->setText(m_turnCancelling ? QStringLiteral("正在停止")
                                               : QStringLiteral("停止"));
        m_sendButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/x.svg")));
        m_sendButton->setToolTip(m_turnCancelling
            ? QStringLiteral("已请求停止，正在等待运行时确认终态")
            : QStringLiteral("停止当前任务"));
        m_sendButton->setEnabled(m_runtime->isReady() && !m_turnCancelling);
        return;
    }
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    if (m_runtimeRecoveryMode || currentSessionRecoveryRequired()) {
        m_sendButton->setText(QStringLiteral("只读恢复"));
        m_sendButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/rotate-ccw.svg")));
        m_sendButton->setToolTip(QStringLiteral("恢复检查完成前不能继续此会话"));
        m_sendButton->setEnabled(false);
        return;
    }
    if (currentSessionDeletionPending()) {
        m_sendButton->setText(QStringLiteral("待删除"));
        m_sendButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/x.svg")));
        m_sendButton->setToolTip(QStringLiteral("从会话菜单撤销删除后才能继续发送"));
        m_sendButton->setEnabled(false);
        return;
    }
    if (currentOperationStatusBlocked()) {
        const bool unknown = !m_operationStatusKnown
            || m_operationStatusSessionId != sessionId;
        m_sendButton->setText(unknown ? QStringLiteral("检查状态") : QStringLiteral("操作暂停"));
        m_sendButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/rotate-ccw.svg")));
        m_sendButton->setToolTip(unknown
            ? QStringLiteral("正在确认操作状态；状态未知时不会继续写入会话")
            : QStringLiteral("检测到未完成操作；恢复动作暂不可用"));
        m_sendButton->setEnabled(false);
        return;
    }
    const bool archived = !sessionId.isEmpty() && m_archivedSessionIds.contains(sessionId);
    if (archived) {
        m_sendButton->setText(QStringLiteral("已归档"));
        m_sendButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/send.svg")));
        m_sendButton->setToolTip(QStringLiteral("右键会话并选择恢复后才能继续发送"));
        m_sendButton->setEnabled(false);
        return;
    }
    m_sendButton->setText(QStringLiteral("发送"));
    m_sendButton->setIcon(QIcon(QStringLiteral(":/icons/lucide/send.svg")));
    m_sendButton->setToolTip(QStringLiteral("发送消息"));
    m_sendButton->setEnabled(m_runtime->isReady());
}

void AgentWorkbenchWidget::ensureSessionAndSubmit(const QString &prompt,
                                                   const QJsonArray &context,
                                                   const QString &pinnedContextSetIdentity,
                                                   const QStringList &pinnedContextIds)
{
    bool contextNeedsProject = !pinnedContextIds.isEmpty();
    for (const QJsonValue &value : context) {
        if (!value.toObject().value(QStringLiteral("path")).toString().isEmpty()) {
            contextNeedsProject = true;
            break;
        }
    }
    if (m_mode == QStringLiteral("chat") && contextNeedsProject
            && m_chatSessionProjectId != m_projectId) {
        m_chatSessionId.clear();
        m_chatSessionProjectId.clear();
        addNotice(QStringLiteral("已为当前项目上下文创建新的只读 Chat 会话。"));
    }
    const QString sessionId = m_mode == QStringLiteral("work") ? m_workSessionId : m_chatSessionId;
    if (!sessionId.isEmpty()) {
        if (!m_runtime->startTurn(sessionId, prompt, context,
                                  pinnedContextSetIdentity, pinnedContextIds).isEmpty()) {
            clearContextItems();
        }
        return;
    }
    if (!m_pendingPrompt.isEmpty()) {
        addNotice(QStringLiteral("会话正在创建，请稍候。"));
        return;
    }
    m_pendingPrompt = prompt;
    m_pendingContext = context;
    m_pendingPinnedContextSetIdentity = pinnedContextSetIdentity;
    m_pendingPinnedContextIds = pinnedContextIds;
    clearContextItems();
    const bool bindProject = m_mode == QStringLiteral("work")
        || ((!context.isEmpty() || !pinnedContextIds.isEmpty()) && !m_projectId.isEmpty());
    m_runtime->startSession(m_mode, bindProject ? m_projectId : QString());
}

void AgentWorkbenchWidget::addContextItem(QJsonObject item)
{
    if (!item.contains(QStringLiteral("root_id"))
            && item.contains(QStringLiteral("path"))
            && !m_workspaceRootId.isEmpty()) {
        item.insert(QStringLiteral("root_id"), m_workspaceRootId);
    }
    QJsonObject identity = item;
    identity.remove(QStringLiteral("id"));
    identity.remove(QStringLiteral("included"));
    identity.remove(QStringLiteral("size"));
    const QByteArray digest = QCryptographicHash::hash(
        QJsonDocument(identity).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex();
    const QString id = QStringLiteral("context:%1").arg(QString::fromLatin1(digest));
    item.insert(QStringLiteral("id"), id);
    item.insert(QStringLiteral("included"), true);
    if (!item.contains(QStringLiteral("size"))) {
        item.insert(QStringLiteral("size"),
                    item.value(QStringLiteral("content")).toString().toUtf8().size());
    }
    for (int index = 0; index < m_contextItems.size(); ++index) {
        if (m_contextItems[index].value(QStringLiteral("id")).toString() == id) {
            m_contextItems[index] = item;
            rebuildContextPanel();
            addNotice(QStringLiteral("已更新相同来源的上下文。"));
            return;
        }
    }
    if (m_contextItems.size() >= kMaxTurnContextItems) {
        addNotice(QStringLiteral("一个 turn 最多添加 %1 项上下文。")
                      .arg(kMaxTurnContextItems), true);
        return;
    }
    m_contextItems.append(item);
    rebuildContextPanel();
}

void AgentWorkbenchWidget::addSelectedFileContext()
{
    QTreeWidgetItem *item = m_fileTree ? m_fileTree->currentItem() : nullptr;
    if (!item || item->data(0, kKindRole).toString() != QStringLiteral("file")) {
        addNotice(QStringLiteral("请先在文件树选择一个文本文件。"), true);
        return;
    }
    const QString path = item->data(0, kPathRole).toString();
    addContextItem({
        {QStringLiteral("kind"), QStringLiteral("file")},
        {QStringLiteral("label"), path},
        {QStringLiteral("origin"), QStringLiteral("file-tree")},
        {QStringLiteral("path"), path},
        {QStringLiteral("revision"), item->data(0, kRevisionRole).toString()},
        {QStringLiteral("size"), item->text(1).toLongLong()},
    });
}

void AgentWorkbenchWidget::requestPinnedContext()
{
    if (!m_pinnedContextAvailable || !m_runtime || !m_runtime->isReady()
            || m_runtimeRecoveryMode || m_projectId.isEmpty()
            || !m_pinnedContextListRequestId.isEmpty()
            || !m_pinnedContextMutationRequestId.isEmpty()) {
        return;
    }
    m_pinnedContextListRequestId = m_runtime->listPinnedContext(m_projectId);
}

void AgentWorkbenchWidget::applyPinnedContextResult(const QJsonObject &result)
{
    if (result.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("pinned-context/0.1")
            || result.value(QStringLiteral("content_bodies_included")).toBool(true)) {
        m_pendingPinnedIncludeId.clear();
        addNotice(QStringLiteral("运行时返回了无效的固定上下文集合。"), true);
        return;
    }
    QSet<QString> includedIds;
    for (const QJsonObject &item : std::as_const(m_pinnedContextItems)) {
        if (item.value(QStringLiteral("included")).toBool()) {
            includedIds.insert(item.value(QStringLiteral("id")).toString());
        }
    }
    if (!m_pendingPinnedIncludeId.isEmpty()) {
        includedIds.insert(m_pendingPinnedIncludeId);
    }
    m_pendingPinnedIncludeId.clear();
    m_pinnedContextItems.clear();
    const QJsonArray items = result.value(QStringLiteral("items")).toArray();
    for (const QJsonValue &value : items) {
        QJsonObject item = value.toObject();
        const QString id = item.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        item.insert(QStringLiteral("included"), includedIds.contains(id));
        m_pinnedContextItems.append(item);
    }
    m_pinnedContextSetIdentity = result.value(QStringLiteral("set_identity")).toString();
    rebuildContextPanel();
    updateEditorActions();
    updateTerminalControls();
    updateGitPinControls();
}

void AgentWorkbenchWidget::applyPinnedContextInvalidation(const QJsonObject &result)
{
    const QJsonObject invalidation = result
        .value(QStringLiteral("pinned_context_invalidation")).toObject();
    if (invalidation.isEmpty()) return;
    if (invalidation.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("pinned-context-source-invalidation/0.1")) {
        addNotice(QStringLiteral("固定上下文失效状态版本无效，正在重新读取。"), true);
        requestPinnedContext();
        return;
    }
    const QString state = invalidation.value(QStringLiteral("state")).toString();
    if (state == QStringLiteral("unchanged")) return;
    if (state != QStringLiteral("persisted")) {
        addNotice(QStringLiteral("固定上下文失效状态未能持久化，正在重新读取。"), true);
        requestPinnedContext();
        return;
    }
    const QString prefix = QStringLiteral("pinned-context:sha256:");
    const QString previous = invalidation
        .value(QStringLiteral("previous_set_identity")).toString();
    const QString next = invalidation.value(QStringLiteral("set_identity")).toString();
    const auto validIdentity = [](const QString &value, const QString &prefix) {
        return value.startsWith(prefix) && isLowerHex(value.mid(prefix.size()), 64);
    };
    if (!validIdentity(previous, prefix) || !validIdentity(next, prefix)
            || m_pinnedContextSetIdentity.isEmpty()
            || previous != m_pinnedContextSetIdentity) {
        addNotice(QStringLiteral("固定上下文集合已由其他操作更新，正在重新读取。"));
        requestPinnedContext();
        return;
    }
    m_pinnedContextSetIdentity = next;
    updateEditorActions();
    updateTerminalControls();
    updateGitPinControls();
}

QJsonArray AgentWorkbenchWidget::persistedPinnedContextItems() const
{
    QJsonArray items;
    for (QJsonObject item : m_pinnedContextItems) {
        item.remove(QStringLiteral("included"));
        items.append(item);
    }
    return items;
}

QStringList AgentWorkbenchWidget::includedPinnedContextIds() const
{
    QStringList ids;
    for (const QJsonObject &item : m_pinnedContextItems) {
        if (item.value(QStringLiteral("included")).toBool()) {
            ids.append(item.value(QStringLiteral("id")).toString());
        }
    }
    return ids;
}

void AgentWorkbenchWidget::pinImageContext()
{
    constexpr qint64 kMaxImageBytes = 8LL * 1024 * 1024;
    if (!m_pinnedContextAvailable || !m_imageContextAvailable
            || m_runtimeRecoveryMode || m_mode != QStringLiteral("work")
            || m_projectId.isEmpty() || m_workSessionId.isEmpty()) {
        addNotice(QStringLiteral("当前 Work 会话未提供固定图片能力。"), true);
        return;
    }
    if (currentSessionRecoveryRequired() || currentSessionDeletionPending()
            || currentOperationStatusBlocked() || !m_pinnedContextMutationRequestId.isEmpty()
            || !m_pinnedImageImportRequestId.isEmpty()) {
        addNotice(QStringLiteral("当前会话状态不允许导入固定图片。"), true);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("固定图片"), QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.webp)"));
    if (path.isEmpty()) return;
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() <= 0
            || info.size() > kMaxImageBytes) {
        addNotice(QStringLiteral("图片必须是 8 MiB 以内的普通 PNG、JPEG 或 WebP 文件。"), true);
        return;
    }
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    const QByteArray format = reader.format().toLower();
    const QSize size = reader.size();
    QString mediaType;
    if (format == QByteArrayLiteral("png")) {
        mediaType = QStringLiteral("image/png");
    } else if (format == QByteArrayLiteral("jpeg") || format == QByteArrayLiteral("jpg")) {
        mediaType = QStringLiteral("image/jpeg");
    } else if (format == QByteArrayLiteral("webp")) {
        mediaType = QStringLiteral("image/webp");
    }
    const qint64 pixels = static_cast<qint64>(size.width()) * size.height();
    if (mediaType.isEmpty() || !size.isValid() || size.width() > 8192
            || size.height() > 8192 || pixels <= 0 || pixels > 40000000) {
        addNotice(QStringLiteral("图片格式或尺寸不受支持（最大 8192px、4000 万像素）。"), true);
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        addNotice(QStringLiteral("无法读取所选图片。"), true);
        return;
    }
    const QByteArray content = file.read(kMaxImageBytes + 1);
    if (content.size() != info.size() || content.size() > kMaxImageBytes) {
        addNotice(QStringLiteral("图片读取不完整或超过大小限制。"), true);
        return;
    }
    m_pinnedImageImportRequestId = m_runtime->importPinnedImage(
        m_workSessionId, m_workspaceRootId, info.fileName(), mediaType, content);
    if (m_pinnedImageImportRequestId.isEmpty()) {
        addNotice(QStringLiteral("无法提交固定图片导入。"), true);
    }
}

void AgentWorkbenchWidget::finishPinnedImageImport(const QJsonObject &image)
{
    const QString schema = image.value(QStringLiteral("schema_version")).toString();
    const QString projectId = image.value(QStringLiteral("project_id")).toString();
    const QString sessionId = image.value(QStringLiteral("session_id")).toString();
    const QString rootId = image.value(QStringLiteral("root_id")).toString();
    const QString label = image.value(QStringLiteral("label")).toString();
    const QString reference = image.value(QStringLiteral("reference")).toString();
    const QString contentHash = image.value(QStringLiteral("content_hash")).toString();
    const QString mediaType = image.value(QStringLiteral("media_type")).toString();
    const qint64 bytes = image.value(QStringLiteral("bytes")).toVariant().toLongLong();
    const int width = image.value(QStringLiteral("width")).toInt();
    const int height = image.value(QStringLiteral("height")).toInt();
    const QString referencePrefix = QStringLiteral("image:sha256:");
    const QString hashPrefix = QStringLiteral("sha256:");
    const QString digest = contentHash.mid(hashPrefix.size());
    const bool lowerHex = digest.size() == 64
        && std::all_of(digest.cbegin(), digest.cend(), [](QChar character) {
            return (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
                || (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
        });
    if (schema != QStringLiteral("pinned-image/0.1") || projectId != m_projectId
            || sessionId != m_workSessionId || rootId != m_workspaceRootId
            || label.isEmpty() || !contentHash.startsWith(hashPrefix) || !lowerHex
            || reference != referencePrefix + digest || bytes <= 0
            || bytes > 8LL * 1024 * 1024 || width <= 0 || width > 8192
            || height <= 0 || height > 8192
            || (mediaType != QStringLiteral("image/png")
                && mediaType != QStringLiteral("image/jpeg")
                && mediaType != QStringLiteral("image/webp"))
            || image.value(QStringLiteral("content_included")).toBool(true)) {
        addNotice(QStringLiteral("固定图片返回的身份或作用域无效，未保存描述符。"), true);
        return;
    }
    const QString id = QStringLiteral("pin-image-%1").arg(digest);
    QJsonObject descriptor{
        {QStringLiteral("id"), id},
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("root_id"), rootId},
        {QStringLiteral("kind"), QStringLiteral("image")},
        {QStringLiteral("source"), QStringLiteral("user-image-import")},
        {QStringLiteral("label"), label},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("content_hash"), contentHash},
        {QStringLiteral("bytes"), static_cast<double>(bytes)},
        {QStringLiteral("freshness"), QStringLiteral("fresh")},
        {QStringLiteral("priority"), 800},
        {QStringLiteral("metadata"), QJsonObject{
            {QStringLiteral("media_type"), mediaType},
            {QStringLiteral("width"), QString::number(width)},
            {QStringLiteral("height"), QString::number(height)},
        }},
    };
    QJsonArray items;
    bool replaced = false;
    for (QJsonObject existing : m_pinnedContextItems) {
        existing.remove(QStringLiteral("included"));
        if (existing.value(QStringLiteral("id")).toString() == id) {
            items.append(descriptor);
            replaced = true;
        } else {
            items.append(existing);
        }
    }
    if (!replaced) {
        if (items.size() >= 128) {
            addNotice(QStringLiteral("固定上下文已达到 128 项上限。"), true);
            return;
        }
        items.append(descriptor);
    }
    m_pendingPinnedIncludeId = id;
    m_pinnedContextMutationRequestId = m_runtime->savePinnedContext(
        m_projectId, items, m_pinnedContextSetIdentity);
    if (m_pinnedContextMutationRequestId.isEmpty()) {
        m_pendingPinnedIncludeId.clear();
        addNotice(QStringLiteral("无法保存固定图片描述符。"), true);
    }
}

void AgentWorkbenchWidget::previewPinnedImage(const QJsonObject &item)
{
    const QString sessionId = item.value(QStringLiteral("session_id")).toString();
    const QString reference = item.value(QStringLiteral("reference")).toString();
    if (!m_imageContextAvailable || sessionId != m_workSessionId || reference.isEmpty()) {
        addNotice(QStringLiteral("该图片属于其他会话或预览能力不可用。"), true);
        return;
    }
    if (!m_pinnedImagePreviewRequestId.isEmpty()) return;
    m_pinnedImagePreviewReference = reference;
    m_pinnedImagePreviewRequestId = m_runtime->readPinnedImage(sessionId, reference);
    if (m_pinnedImagePreviewRequestId.isEmpty()) {
        m_pinnedImagePreviewReference.clear();
        addNotice(QStringLiteral("无法请求固定图片预览。"), true);
    }
    rebuildContextPanel();
}

void AgentWorkbenchWidget::showPinnedImagePreview(const QJsonObject &preview)
{
    const QString reference = preview.value(QStringLiteral("reference")).toString();
    const QString sessionId = preview.value(QStringLiteral("session_id")).toString();
    const QByteArray thumbnail = QByteArray::fromBase64(
        preview.value(QStringLiteral("thumbnail_data_base64")).toString().toLatin1(),
        QByteArray::AbortOnBase64DecodingErrors);
    QPixmap pixmap;
    const bool valid = preview.value(QStringLiteral("schema_version")).toString()
            == QStringLiteral("pinned-image-preview/0.1")
        && reference == m_pinnedImagePreviewReference && sessionId == m_workSessionId
        && preview.value(QStringLiteral("project_id")).toString() == m_projectId
        && preview.value(QStringLiteral("thumbnail_media_type")).toString()
            == QStringLiteral("image/png")
        && !preview.value(QStringLiteral("original_content_included")).toBool(true)
        && !thumbnail.isEmpty() && thumbnail.size() <= 2 * 1024 * 1024
        && pixmap.loadFromData(thumbnail, "PNG") && pixmap.width() <= 320
        && pixmap.height() <= 180;
    if (!valid) {
        addNotice(QStringLiteral("固定图片预览身份校验失败。"), true);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("固定图片预览"));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(10);
    auto *image = new QLabel(&dialog);
    image->setAlignment(Qt::AlignCenter);
    image->setPixmap(pixmap);
    image->setMinimumSize(320, 180);
    image->setStyleSheet(QStringLiteral("background:#f2f4f7; border:1px solid #e4e7ec; border-radius:6px;"));
    layout->addWidget(image);
    auto *metadata = new QLabel(QStringLiteral("%1 × %2 · %3 · %4 B")
        .arg(preview.value(QStringLiteral("width")).toInt())
        .arg(preview.value(QStringLiteral("height")).toInt())
        .arg(preview.value(QStringLiteral("media_type")).toString())
        .arg(preview.value(QStringLiteral("bytes")).toVariant().toLongLong()), &dialog);
    metadata->setAlignment(Qt::AlignCenter);
    metadata->setStyleSheet(QStringLiteral("color:#475467; border:none;"));
    layout->addWidget(metadata);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void AgentWorkbenchWidget::pinSelectedFileContext()
{
    if (!m_pinnedContextAvailable || m_runtimeRecoveryMode || m_projectId.isEmpty()) {
        addNotice(QStringLiteral("当前运行时未提供固定上下文能力。"), true);
        return;
    }
    if (!m_pinnedContextMutationRequestId.isEmpty()
            || !m_pinnedImageImportRequestId.isEmpty()
            || !m_pinnedFileReadRequestId.isEmpty()) {
        addNotice(QStringLiteral("固定上下文正在更新，请稍候。"));
        return;
    }
    QTreeWidgetItem *item = m_fileTree ? m_fileTree->currentItem() : nullptr;
    if (!item || item->data(0, kKindRole).toString() != QStringLiteral("file")) {
        addNotice(QStringLiteral("请先在文件树选择一个文本文件。"), true);
        return;
    }
    const QString path = item->data(0, kPathRole).toString();
    m_pinnedFileReadRequestId = m_runtime->readWorkspaceFile(
        m_projectId, path, m_workspaceRootId);
    if (m_pinnedFileReadRequestId.isEmpty()) {
        addNotice(QStringLiteral("无法读取待固定文件。"), true);
    }
}

void AgentWorkbenchWidget::pinEditorSelectionContext()
{
    if (!m_pinnedContextAvailable || m_runtimeRecoveryMode || m_projectId.isEmpty()) {
        addNotice(QStringLiteral("当前运行时未提供固定上下文能力。"), true);
        return;
    }
    if (currentSessionRecoveryRequired() || currentSessionDeletionPending()
            || currentOperationStatusBlocked()) {
        addNotice(QStringLiteral("当前会话状态不允许更新固定上下文。"), true);
        return;
    }
    if (!m_pinnedContextMutationRequestId.isEmpty()
            || !m_pinnedImageImportRequestId.isEmpty()
            || !m_pinnedFileReadRequestId.isEmpty()) {
        addNotice(QStringLiteral("固定上下文正在更新，请稍候。"));
        return;
    }
    if (m_openEditorPath.isEmpty() || !m_editorBuffers.contains(m_openEditorPath)) {
        addNotice(QStringLiteral("请先打开文件并选择代码。"), true);
        return;
    }
    const EditorBuffer &buffer = m_editorBuffers[m_openEditorPath];
    if (buffer.modified || buffer.conflict) {
        addNotice(QStringLiteral("请先保存并解决冲突后再固定编辑器选区。"), true);
        return;
    }
    const int start = qMin(buffer.cursorPosition, buffer.anchorPosition);
    const int end = qMax(buffer.cursorPosition, buffer.anchorPosition);
    if (start == end) {
        addNotice(QStringLiteral("编辑器中没有选中的文本。"), true);
        return;
    }
    const auto position = [&buffer](int offset) {
        const int bounded = qBound(0, offset, buffer.content.size());
        const int line = buffer.content.left(bounded).count(QLatin1Char('\n')) + 1;
        const int previousBreak = buffer.content.lastIndexOf(QLatin1Char('\n'), bounded - 1);
        const QString linePrefix = buffer.content.mid(
            previousBreak + 1, bounded - previousBreak - 1);
        return QPair<int, int>{line, linePrefix.toUcs4().size() + 1};
    };
    const auto startPosition = position(start);
    const auto endPosition = position(end);
    const QJsonObject metadata{
        {QStringLiteral("line"), QString::number(startPosition.first)},
        {QStringLiteral("column"), QString::number(startPosition.second)},
        {QStringLiteral("end_line"), QString::number(endPosition.first)},
        {QStringLiteral("end_column"), QString::number(endPosition.second)},
    };
    m_pendingPinnedSelection = QJsonObject{
        {QStringLiteral("path"), m_openEditorPath},
        {QStringLiteral("label"), QStringLiteral("%1:%2:%3-%4:%5")
            .arg(m_openEditorPath)
            .arg(startPosition.first)
            .arg(startPosition.second)
            .arg(endPosition.first)
            .arg(endPosition.second)},
        {QStringLiteral("metadata"), metadata},
    };
    m_pinnedFileReadRequestId = m_runtime->readWorkspaceFile(
        m_projectId, m_openEditorPath, m_workspaceRootId);
    if (m_pinnedFileReadRequestId.isEmpty()) {
        m_pendingPinnedSelection = {};
        addNotice(QStringLiteral("无法读取待固定选区的文件。"), true);
    }
}

bool AgentWorkbenchWidget::canPinCommandArtifact(const QJsonObject &artifact,
                                                 QString *reason) const
{
    const auto reject = [reason](const QString &message) {
        if (reason) *reason = message;
        return false;
    };
    if (!m_runtime || !m_runtime->isReady() || !m_pinnedContextAvailable) {
        return reject(QStringLiteral("当前运行时未提供固定上下文能力"));
    }
    const QString sessionId = artifact.value(QStringLiteral("session_id")).toString();
    if (m_mode != QStringLiteral("work") || m_workSessionId.isEmpty()
            || sessionId != m_workSessionId) {
        return reject(QStringLiteral("只能固定到生成该输出的当前 Work 会话"));
    }
    if (m_projectId.isEmpty()) {
        return reject(QStringLiteral("请先打开该 Work 会话绑定的项目"));
    }
    if (m_runtimeRecoveryMode || currentSessionRecoveryRequired()
            || currentSessionDeletionPending() || currentOperationStatusBlocked()) {
        return reject(QStringLiteral("当前会话状态不允许更新固定上下文"));
    }
    if (!m_pinnedContextMutationRequestId.isEmpty()
            || !m_pinnedImageImportRequestId.isEmpty()
            || !m_pinnedFileReadRequestId.isEmpty()) {
        return reject(QStringLiteral("固定上下文正在更新，请稍候"));
    }
    const QString reference = artifact.value(QStringLiteral("reference")).toString();
    const QString sha256 = artifact.value(QStringLiteral("sha256")).toString();
    const QString itemId = artifact.value(QStringLiteral("item_id")).toString();
    const QString contentType = artifact.value(QStringLiteral("content_type")).toString();
    const QByteArray content = artifact.value(QStringLiteral("content")).toString().toUtf8();
    const QString prefix = QStringLiteral("command-output:sha256:");
    const auto isLowerHex = [](const QString &value) {
        return value.size() == 64
            && std::all_of(value.cbegin(), value.cend(), [](QChar character) {
                return (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
                    || (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
            });
    };
    if (!reference.startsWith(prefix) || !isLowerHex(sha256)
            || reference.mid(prefix.size()) != sha256 || itemId.isEmpty()
            || itemId.size() > 128 || contentType != QStringLiteral("text/plain; charset=utf-8")
            || QString::fromLatin1(QCryptographicHash::hash(
                   content, QCryptographicHash::Sha256).toHex()) != sha256) {
        return reject(QStringLiteral("命令输出身份校验失败，不能固定"));
    }
    if (reason) reason->clear();
    return true;
}

void AgentWorkbenchWidget::pinCommandArtifact(const QJsonObject &artifact)
{
    QString reason;
    if (!canPinCommandArtifact(artifact, &reason)) {
        addNotice(reason, true);
        return;
    }
    const QString sessionId = artifact.value(QStringLiteral("session_id")).toString();
    const QString reference = artifact.value(QStringLiteral("reference")).toString();
    const QString sha256 = artifact.value(QStringLiteral("sha256")).toString();
    const QString itemId = artifact.value(QStringLiteral("item_id")).toString();
    const QByteArray content = artifact.value(QStringLiteral("content")).toString().toUtf8();
    const QByteArray idDigest = QCryptographicHash::hash(
        QStringLiteral("%1\n%2\n%3").arg(m_projectId, sessionId, reference).toUtf8(),
        QCryptographicHash::Sha256).toHex();
    const QString id = QStringLiteral("pin-artifact-%1").arg(QString::fromLatin1(idDigest));
    QJsonObject descriptor{
        {QStringLiteral("id"), id},
        {QStringLiteral("project_id"), m_projectId},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("kind"), QStringLiteral("artifact")},
        {QStringLiteral("source"), QStringLiteral("command-output")},
        {QStringLiteral("label"), QStringLiteral("命令输出 Artifact")},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("content_hash"), QStringLiteral("sha256:%1").arg(sha256)},
        {QStringLiteral("bytes"), static_cast<double>(content.size())},
        {QStringLiteral("freshness"), QStringLiteral("fresh")},
        {QStringLiteral("priority"), 700},
        {QStringLiteral("metadata"), QJsonObject{
            {QStringLiteral("item_id"), itemId},
        }},
    };
    QJsonArray items;
    bool replaced = false;
    for (QJsonObject item : m_pinnedContextItems) {
        item.remove(QStringLiteral("included"));
        if (item.value(QStringLiteral("id")).toString() == id) {
            items.append(descriptor);
            replaced = true;
        } else {
            items.append(item);
        }
    }
    if (!replaced) {
        if (items.size() >= 128) {
            addNotice(QStringLiteral("固定上下文已达到 128 项上限。"), true);
            return;
        }
        items.append(descriptor);
    }
    m_pendingPinnedIncludeId = id;
    m_pinnedContextMutationRequestId = m_runtime->savePinnedContext(
        m_projectId, items, m_pinnedContextSetIdentity);
    if (m_pinnedContextMutationRequestId.isEmpty()) {
        m_pendingPinnedIncludeId.clear();
        addNotice(QStringLiteral("无法提交命令输出固定上下文更新。"), true);
    }
}

void AgentWorkbenchWidget::finishPinnedFileRead(const QJsonObject &file)
{
    const QJsonObject pendingSelection = m_pendingPinnedSelection;
    const bool selection = !pendingSelection.isEmpty();
    const auto reject = [this](const QString &message) {
        m_pendingPinnedSelection = {};
        addNotice(message, true);
    };
    const QString path = file.value(QStringLiteral("path")).toString();
    const QString encoding = file.value(QStringLiteral("encoding")).toString();
    const QString newline = file.value(QStringLiteral("newline")).toString();
    if (path.isEmpty()
            || (newline != QStringLiteral("lf") && newline != QStringLiteral("crlf")
                && newline != QStringLiteral("none"))) {
        reject(QStringLiteral("混合或未知换行文件暂不能固定；请先规范化并保存。"));
        return;
    }
    if (selection && path != pendingSelection.value(QStringLiteral("path")).toString()) {
        reject(QStringLiteral("待固定选区的文件已变化，未写入固定上下文。"));
        return;
    }
    QString rawText = file.value(QStringLiteral("content")).toString();
    if (newline == QStringLiteral("crlf")) {
        rawText.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));
    }
    QByteArray raw = rawText.toUtf8();
    if (encoding == QStringLiteral("utf-8-bom")) raw.prepend("\xef\xbb\xbf", 3);
    else if (encoding != QStringLiteral("utf-8")) {
        reject(QStringLiteral("该文件编码暂不支持固定上下文。"));
        return;
    }
    const qint64 expectedBytes = file.value(QStringLiteral("size")).toVariant().toLongLong();
    if (raw.size() != expectedBytes) {
        reject(QStringLiteral("文件字节身份无法重建，未写入固定上下文。"));
        return;
    }
    const QString kind = selection ? QStringLiteral("selection") : QStringLiteral("file");
    const QString source = selection ? QStringLiteral("editor-selection")
                                     : QStringLiteral("file-tree");
    const QJsonObject metadata = selection
        ? pendingSelection.value(QStringLiteral("metadata")).toObject()
        : QJsonObject{};
    const QString descriptorLabel = selection
        ? pendingSelection.value(QStringLiteral("label")).toString(path)
        : path;
    const QByteArray idDigest = QCryptographicHash::hash(
        QStringLiteral("%1\n%2\n%3\n%4\n%5")
            .arg(m_projectId, m_workspaceRootId, kind, path,
                 QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact)))
            .toUtf8(),
        QCryptographicHash::Sha256).toHex();
    const QString id = QStringLiteral("pin-%1-%2").arg(
        kind, QString::fromLatin1(idDigest));
    QJsonObject descriptor{
        {QStringLiteral("id"), id},
        {QStringLiteral("project_id"), m_projectId},
        {QStringLiteral("root_id"), m_workspaceRootId},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("source"), source},
        {QStringLiteral("label"), descriptorLabel},
        {QStringLiteral("reference"), path},
        {QStringLiteral("content_hash"), QStringLiteral("sha256:%1").arg(
             QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256)
                                     .toHex()))},
        {QStringLiteral("bytes"), static_cast<double>(raw.size())},
        {QStringLiteral("revision"), file.value(QStringLiteral("revision"))},
        {QStringLiteral("freshness"), QStringLiteral("fresh")},
        {QStringLiteral("priority"), selection ? 800 : 850},
        {QStringLiteral("metadata"), metadata},
    };
    QJsonArray items;
    bool replaced = false;
    for (QJsonObject item : m_pinnedContextItems) {
        item.remove(QStringLiteral("included"));
        if (item.value(QStringLiteral("id")).toString() == id) {
            items.append(descriptor);
            replaced = true;
        } else {
            items.append(item);
        }
    }
    if (!replaced) items.append(descriptor);
    m_pendingPinnedSelection = {};
    m_pendingPinnedIncludeId = id;
    m_pinnedContextMutationRequestId = m_runtime->savePinnedContext(
        m_projectId, items, m_pinnedContextSetIdentity);
    if (m_pinnedContextMutationRequestId.isEmpty()) {
        m_pendingPinnedIncludeId.clear();
        addNotice(QStringLiteral("无法提交固定上下文更新。"), true);
    }
}

void AgentWorkbenchWidget::savePinnedContextOrder()
{
    if (!m_runtime || !m_runtime->isReady() || m_projectId.isEmpty()
            || !m_pinnedContextMutationRequestId.isEmpty()) {
        return;
    }
    m_pinnedContextMutationRequestId = m_runtime->savePinnedContext(
        m_projectId, persistedPinnedContextItems(), m_pinnedContextSetIdentity);
    if (m_pinnedContextMutationRequestId.isEmpty()) requestPinnedContext();
    rebuildContextPanel();
}

void AgentWorkbenchWidget::addEditorSelectionContext()
{
    if (m_openEditorPath.isEmpty() || !m_editorBuffers.contains(m_openEditorPath)) {
        addNotice(QStringLiteral("请先打开文件并选择代码。"), true);
        return;
    }
    const EditorBuffer &buffer = m_editorBuffers[m_openEditorPath];
    const int start = qMin(buffer.cursorPosition, buffer.anchorPosition);
    const int end = qMax(buffer.cursorPosition, buffer.anchorPosition);
    if (start == end) {
        addNotice(QStringLiteral("编辑器中没有选中的文本。"), true);
        return;
    }
    bool truncated = false;
    const QString content = boundedContextText(buffer.content.mid(start, end - start), &truncated);
    const QString before = buffer.content.left(start);
    const QString throughEnd = buffer.content.left(end);
    const int line = before.count(QLatin1Char('\n')) + 1;
    const int endLine = throughEnd.count(QLatin1Char('\n')) + 1;
    const int previousBreak = before.lastIndexOf(QLatin1Char('\n'));
    const int endBreak = throughEnd.lastIndexOf(QLatin1Char('\n'));
    const int column = start - previousBreak;
    const int endColumn = end - endBreak;
    addContextItem({
        {QStringLiteral("kind"), QStringLiteral("selection")},
        {QStringLiteral("label"), QStringLiteral("%1:%2-%3")
            .arg(m_openEditorPath).arg(line).arg(endLine)},
        {QStringLiteral("origin"), QStringLiteral("editor-selection")},
        {QStringLiteral("path"), m_openEditorPath},
        {QStringLiteral("content"), content},
        {QStringLiteral("revision"), buffer.revision},
        {QStringLiteral("line"), line},
        {QStringLiteral("column"), column},
        {QStringLiteral("end_line"), endLine},
        {QStringLiteral("end_column"), endColumn},
        {QStringLiteral("truncated"), truncated},
        {QStringLiteral("size"), content.toUtf8().size()},
    });
}

void AgentWorkbenchWidget::addSearchResultContext()
{
    QTreeWidgetItem *item = m_workspaceSearchResults
        ? m_workspaceSearchResults->currentItem() : nullptr;
    if (!item) {
        addNotice(QStringLiteral("请先选择一个搜索结果。"), true);
        return;
    }
    const QJsonObject match = QJsonObject::fromVariantMap(
        item->data(0, kContextRole).toMap());
    const QString path = match.value(QStringLiteral("path")).toString(item->text(0));
    const int line = qMax(1, match.value(QStringLiteral("line")).toInt(1));
    bool truncated = false;
    const QString content = boundedContextText(
        match.value(QStringLiteral("preview")).toString(item->text(2)), &truncated);
    addContextItem({
        {QStringLiteral("kind"), QStringLiteral("search")},
        {QStringLiteral("label"), QStringLiteral("%1:%2").arg(path).arg(line)},
        {QStringLiteral("origin"), QStringLiteral("workspace-search")},
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), content},
        {QStringLiteral("revision"), match.value(QStringLiteral("revision"))},
        {QStringLiteral("line"), line},
        {QStringLiteral("column"), qMax(1, match.value(QStringLiteral("column")).toInt(1))},
        {QStringLiteral("truncated"), truncated},
        {QStringLiteral("size"), content.toUtf8().size()},
    });
}

void AgentWorkbenchWidget::addDiagnosticContext()
{
    QTreeWidgetItem *item = m_languageDiagnostics
        ? m_languageDiagnostics->currentItem() : nullptr;
    if (!item) {
        addNotice(QStringLiteral("请先选择一条诊断。"), true);
        return;
    }
    const QJsonObject diagnostic = QJsonObject::fromVariantMap(
        item->data(0, kContextRole).toMap());
    const QString path = diagnostic.value(QStringLiteral("path")).toString(item->text(1));
    const QString severity = diagnostic.value(QStringLiteral("severity")).toString();
    const QString code = diagnostic.value(QStringLiteral("code")).toString();
    const QString message = diagnostic.value(QStringLiteral("message")).toString(item->text(4));
    const QString content = QStringLiteral("%1%2: %3")
        .arg(severity,
             code.isEmpty() ? QString() : QStringLiteral(" [%1]").arg(code),
             message);
    QJsonObject context{
        {QStringLiteral("kind"), QStringLiteral("diagnostic")},
        {QStringLiteral("label"), QStringLiteral("%1 · %2").arg(path, item->text(0))},
        {QStringLiteral("origin"), diagnostic.value(QStringLiteral("source_identity"))
            .toString(QStringLiteral("language-server"))},
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), boundedContextText(content)},
        {QStringLiteral("revision"), diagnostic.value(QStringLiteral("file_hash"))},
        {QStringLiteral("line"), diagnostic.value(QStringLiteral("line"))},
        {QStringLiteral("column"), diagnostic.value(QStringLiteral("column"))},
        {QStringLiteral("end_line"), diagnostic.value(QStringLiteral("end_line"))},
        {QStringLiteral("end_column"), diagnostic.value(QStringLiteral("end_column"))},
        {QStringLiteral("freshness"), diagnostic.value(QStringLiteral("freshness"))},
        {QStringLiteral("raw_output_ref"), diagnostic.value(QStringLiteral("raw_output_ref"))},
    };
    context.insert(QStringLiteral("size"),
                   context.value(QStringLiteral("content")).toString().toUtf8().size());
    addContextItem(context);
}

void AgentWorkbenchWidget::pinSelectedDiagnosticContext()
{
    if (!m_pinnedContextAvailable || m_runtimeRecoveryMode || m_projectId.isEmpty()) {
        addNotice(QStringLiteral("当前运行时未提供固定上下文能力。"), true);
        return;
    }
    if (currentSessionRecoveryRequired() || currentSessionDeletionPending()
            || currentOperationStatusBlocked()) {
        addNotice(QStringLiteral("当前会话状态不允许更新固定上下文。"), true);
        return;
    }
    if (!m_pinnedContextMutationRequestId.isEmpty()
            || !m_pinnedFileReadRequestId.isEmpty()
            || !m_pendingPinnedDiagnosticRequestId.isEmpty()
            || !m_diagnosticRawRequestId.isEmpty()) {
        addNotice(QStringLiteral("固定上下文或诊断原始记录正在更新，请稍候。"));
        return;
    }
    QTreeWidgetItem *item = m_languageDiagnostics
        ? m_languageDiagnostics->currentItem() : nullptr;
    if (!item) {
        addNotice(QStringLiteral("请先选择一条诊断。"), true);
        return;
    }
    const QJsonObject diagnostic = QJsonObject::fromVariantMap(
        item->data(0, kContextRole).toMap());
    const QString reference = diagnostic.value(QStringLiteral("raw_output_ref")).toString();
    const QString freshness = diagnostic.value(QStringLiteral("freshness"))
        .toString(QStringLiteral("fresh"));
    if (reference.isEmpty() || freshness == QStringLiteral("stale")) {
        addNotice(QStringLiteral("诊断原始记录已过期或不可用，请重新计算诊断。"), true);
        return;
    }
    m_pendingPinnedDiagnostic = diagnostic;
    m_pendingPinnedDiagnosticRequestId = m_runtime->diagnosticRaw(
        m_projectId, reference, m_workspaceRootId);
    if (m_pendingPinnedDiagnosticRequestId.isEmpty()) {
        m_pendingPinnedDiagnostic = {};
        addNotice(QStringLiteral("无法读取待固定诊断原始记录。"), true);
    }
}

void AgentWorkbenchWidget::finishPinnedDiagnosticRaw(const QJsonObject &raw)
{
    const QJsonObject diagnostic = m_pendingPinnedDiagnostic;
    m_pendingPinnedDiagnostic = {};
    const QString reference = raw.value(QStringLiteral("reference")).toString();
    const QString sha256 = raw.value(QStringLiteral("sha256")).toString();
    const QString contentType = raw.value(QStringLiteral("content_type")).toString();
    const QString prefix = QStringLiteral("diagnostic-raw:sha256:");
    const QByteArray content = raw.value(QStringLiteral("content")).toString().toUtf8();
    const auto isLowerHex = [](const QString &value) {
        return value.size() == 64
            && std::all_of(value.cbegin(), value.cend(), [](QChar character) {
                return (character >= QLatin1Char('0') && character <= QLatin1Char('9'))
                    || (character >= QLatin1Char('a') && character <= QLatin1Char('f'));
            });
    };
    if (contentType != QStringLiteral("application/vnd.aegisy.diagnostics+json")
            || !reference.startsWith(prefix) || !isLowerHex(sha256)
            || reference.mid(prefix.size()) != sha256
            || QString::fromLatin1(QCryptographicHash::hash(
                   content, QCryptographicHash::Sha256).toHex()) != sha256
            || raw.value(QStringLiteral("size")).toVariant().toLongLong() != content.size()
            || diagnostic.value(QStringLiteral("raw_output_ref")).toString() != reference) {
        addNotice(QStringLiteral("诊断原始记录身份校验失败，未写入固定上下文。"), true);
        return;
    }
    const QString path = diagnostic.value(QStringLiteral("path")).toString();
    const int line = diagnostic.value(QStringLiteral("line")).toInt();
    const int column = diagnostic.value(QStringLiteral("column")).toInt();
    const QString severity = diagnostic.value(QStringLiteral("severity")).toString();
    const QString sourceIdentity = diagnostic.value(QStringLiteral("source_identity"))
        .toString(QStringLiteral("diagnostic"));
    if (path.isEmpty() || sourceIdentity.isEmpty()) {
        addNotice(QStringLiteral("诊断缺少可固定的来源或文件身份。"), true);
        return;
    }
    const QByteArray idDigest = QCryptographicHash::hash(
        QStringLiteral("%1\n%2\n%3\n%4\n%5\n%6")
            .arg(m_projectId, m_workspaceRootId, reference, path)
            .arg(line).arg(column).arg(severity).toUtf8(),
        QCryptographicHash::Sha256).toHex();
    const QString id = QStringLiteral("pin-diagnostic-%1")
        .arg(QString::fromLatin1(idDigest));
    const QJsonObject metadata{
        {QStringLiteral("path"), path},
        {QStringLiteral("line"), QString::number(line)},
        {QStringLiteral("column"), QString::number(column)},
        {QStringLiteral("end_line"), QString::number(
             diagnostic.value(QStringLiteral("end_line")).toInt(line))},
        {QStringLiteral("end_column"), QString::number(
             diagnostic.value(QStringLiteral("end_column")).toInt(column))},
        {QStringLiteral("severity"), severity},
        {QStringLiteral("source_identity"), sourceIdentity},
    };
    QJsonObject descriptor{
        {QStringLiteral("id"), id},
        {QStringLiteral("project_id"), m_projectId},
        {QStringLiteral("root_id"), m_workspaceRootId},
        {QStringLiteral("kind"), QStringLiteral("diagnostic")},
        {QStringLiteral("source"), sourceIdentity},
        {QStringLiteral("label"), QStringLiteral("%1 · %2").arg(
             path, severity.isEmpty() ? QStringLiteral("诊断") : severity)},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("content_hash"), QStringLiteral("sha256:%1").arg(sha256)},
        {QStringLiteral("bytes"), static_cast<double>(content.size())},
        {QStringLiteral("revision"), diagnostic.value(QStringLiteral("file_hash"))},
        {QStringLiteral("freshness"), diagnostic.value(QStringLiteral("freshness"))
            .toString(QStringLiteral("fresh"))},
        {QStringLiteral("priority"), 760},
        {QStringLiteral("metadata"), metadata},
    };
    QJsonArray items;
    bool replaced = false;
    for (QJsonObject existing : m_pinnedContextItems) {
        existing.remove(QStringLiteral("included"));
        if (existing.value(QStringLiteral("id")).toString() == id) {
            items.append(descriptor);
            replaced = true;
        } else {
            items.append(existing);
        }
    }
    if (!replaced) {
        if (items.size() >= 128) {
            addNotice(QStringLiteral("固定上下文已达到 128 项上限。"), true);
            return;
        }
        items.append(descriptor);
    }
    m_pendingPinnedIncludeId = id;
    m_pinnedContextMutationRequestId = m_runtime->savePinnedContext(
        m_projectId, items, m_pinnedContextSetIdentity);
    if (m_pinnedContextMutationRequestId.isEmpty()) {
        m_pendingPinnedIncludeId.clear();
        addNotice(QStringLiteral("无法提交诊断固定上下文更新。"), true);
    }
}

void AgentWorkbenchWidget::addTextExcerptContext(const QString &kind,
                                                  const QString &origin,
                                                  const QString &label,
                                                  QPlainTextEdit *source)
{
    if (!source || !source->textCursor().hasSelection()) {
        addNotice(QStringLiteral("请先选择要添加的内容摘录。"), true);
        return;
    }
    QString selected = source->textCursor().selectedText();
    selected.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    bool truncated = false;
    selected = boundedContextText(selected, &truncated);
    addContextItem({
        {QStringLiteral("kind"), kind},
        {QStringLiteral("label"), label},
        {QStringLiteral("origin"), origin},
        {QStringLiteral("content"), selected},
        {QStringLiteral("truncated"), truncated},
        {QStringLiteral("size"), selected.toUtf8().size()},
    });
}

void AgentWorkbenchWidget::markPinnedContextStale(const QSet<QString> &paths)
{
    if (paths.isEmpty() || m_pinnedContextItems.isEmpty()) return;
    bool changed = false;
    for (QJsonObject &item : m_pinnedContextItems) {
        const QString kind = item.value(QStringLiteral("kind")).toString();
        const QString reference = item.value(QStringLiteral("reference")).toString();
        const QString diagnosticPath = item.value(QStringLiteral("metadata"))
            .toObject().value(QStringLiteral("path")).toString();
        const bool pathMatches = (kind == QStringLiteral("file")
                                  || kind == QStringLiteral("selection"))
            ? paths.contains(reference)
            : kind == QStringLiteral("diagnostic") && paths.contains(diagnosticPath);
        if (!pathMatches
                || item.value(QStringLiteral("freshness")).toString() == QStringLiteral("stale")) {
            continue;
        }
        item.insert(QStringLiteral("freshness"), QStringLiteral("stale"));
        changed = true;
    }
    if (changed) {
        rebuildContextPanel();
        addNotice(QStringLiteral("固定上下文已标记为 stale，请重新固定或确认预检结果。"));
    }
}

void AgentWorkbenchWidget::markPinnedTerminalStale(const QString &terminalId)
{
    if (terminalId.isEmpty() || m_pinnedContextItems.isEmpty()) return;
    bool changed = false;
    for (QJsonObject &item : m_pinnedContextItems) {
        if (item.value(QStringLiteral("kind")).toString()
                != QStringLiteral("terminal_excerpt")
                || item.value(QStringLiteral("metadata")).toObject()
                    .value(QStringLiteral("terminal_id")).toString() != terminalId
                || item.value(QStringLiteral("freshness")).toString()
                    == QStringLiteral("stale")) {
            continue;
        }
        item.insert(QStringLiteral("freshness"), QStringLiteral("stale"));
        changed = true;
    }
    if (changed) {
        rebuildContextPanel();
        addNotice(QStringLiteral("终端固定上下文已失效，请重新固定终端输出。"));
    }
}

void AgentWorkbenchWidget::rebuildContextPanel()
{
    if (!m_contextList || !m_contextPanel || !m_contextSummary) return;
    m_contextList->clear();
    qint64 includedBytes = 0;
    int includedCount = 0;
    auto appendRow = [this, &includedBytes, &includedCount](const QJsonObject &context,
                                                              bool pinned,
                                                              int pinnedIndex) {
        auto *item = new QListWidgetItem(m_contextList);
        item->setSizeHint(QSize(0, 28));
        auto *row = new QWidget(m_contextList);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        auto *included = new QCheckBox(row);
        included->setObjectName(QStringLiteral("agentContextIncludeCheck"));
        included->setChecked(context.value(QStringLiteral("included")).toBool(true));
        included->setToolTip(pinned ? QStringLiteral("将固定上下文包含在下一次发送中")
                                    : QStringLiteral("包含在下一次发送中"));
        layout->addWidget(included);
        const QString label = context.value(QStringLiteral("label")).toString();
        const QString kind = context.value(QStringLiteral("kind")).toString();
        const QJsonObject metadataObject = context.value(QStringLiteral("metadata")).toObject();
        const qint64 size = pinned
            ? context.value(QStringLiteral("bytes")).toVariant().toLongLong()
            : context.value(QStringLiteral("size")).toVariant().toLongLong();
        QString suffix = context.value(QStringLiteral("truncated")).toBool()
            ? QStringLiteral(" · 已截断") : QString();
        if (pinned && context.value(QStringLiteral("freshness")).toString()
                == QStringLiteral("stale")) {
            suffix += QStringLiteral(" · stale");
        }
        const QString prefix = pinned ? QStringLiteral("固定 · ") : QString();
        QString detail = QStringLiteral("%1 B").arg(size);
        if (kind == QStringLiteral("image")) {
            detail = QStringLiteral("%1 × %2 · %3 · %4 B")
                .arg(metadataObject.value(QStringLiteral("width")).toString())
                .arg(metadataObject.value(QStringLiteral("height")).toString())
                .arg(metadataObject.value(QStringLiteral("media_type")).toString())
                .arg(size);
        }
        auto *text = new QLabel(QStringLiteral("%1%2 · %3%4")
                                    .arg(prefix, label, detail, suffix), row);
        text->setToolTip(QStringLiteral("来源：%1\n类型：%2\n%3")
            .arg(context.value(QStringLiteral("origin")).toString(),
                 kind, label));
        text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        text->setStyleSheet(QStringLiteral("color:#475467; font-size:9px; border:none;"));
        layout->addWidget(text, 1);
        QPushButton *previewImage = nullptr;
        if (pinned && kind == QStringLiteral("image")) {
            previewImage = new QPushButton(row);
            previewImage->setObjectName(QStringLiteral("agentPinnedImagePreviewButton"));
            previewImage->setIcon(QIcon(QStringLiteral(":/icons/lucide/search.svg")));
            previewImage->setToolTip(QStringLiteral("预览固定图片"));
            previewImage->setFixedSize(22, 22);
            previewImage->setEnabled(m_imageContextAvailable
                && context.value(QStringLiteral("session_id")).toString() == m_workSessionId
                && m_pinnedImagePreviewRequestId.isEmpty());
            previewImage->setStyleSheet(QStringLiteral(
                "QPushButton { background:transparent; border:none; border-radius:4px; padding:4px; }"
                "QPushButton:hover { background:#eaecf0; }"
                "QPushButton:disabled { opacity:0.45; }"));
            layout->addWidget(previewImage);
        }
        QPushButton *moveUp = nullptr;
        QPushButton *moveDown = nullptr;
        if (pinned) {
            moveUp = new QPushButton(QStringLiteral("↑"), row);
            moveUp->setObjectName(QStringLiteral("agentPinnedContextMoveUpButton"));
            moveUp->setToolTip(QStringLiteral("提高固定上下文优先顺序"));
            moveUp->setEnabled(pinnedIndex > 0 && m_pinnedContextMutationRequestId.isEmpty()
                && m_pinnedImageImportRequestId.isEmpty());
            moveUp->setFixedSize(22, 22);
            moveDown = new QPushButton(QStringLiteral("↓"), row);
            moveDown->setObjectName(QStringLiteral("agentPinnedContextMoveDownButton"));
            moveDown->setToolTip(QStringLiteral("降低固定上下文优先顺序"));
            moveDown->setEnabled(pinnedIndex + 1 < m_pinnedContextItems.size()
                && m_pinnedContextMutationRequestId.isEmpty()
                && m_pinnedImageImportRequestId.isEmpty());
            moveDown->setFixedSize(22, 22);
            for (QPushButton *button : {moveUp, moveDown}) {
                button->setStyleSheet(QStringLiteral(
                    "QPushButton { background:transparent; border:none; border-radius:4px;"
                    " padding:1px; color:#667085; font-size:12px; }"
                    "QPushButton:hover { background:#eaecf0; color:#101828; }"
                    "QPushButton:disabled { color:#d0d5dd; }"));
                layout->addWidget(button);
            }
        }
        auto *remove = new QPushButton(row);
        remove->setObjectName(pinned ? QStringLiteral("agentPinnedContextRemoveButton")
                                     : QStringLiteral("agentContextRemoveButton"));
        remove->setIcon(QIcon(QStringLiteral(":/icons/lucide/x.svg")));
        remove->setToolTip(pinned ? QStringLiteral("解除固定上下文")
                                  : QStringLiteral("移除此上下文"));
        remove->setFixedSize(22, 22);
        remove->setStyleSheet(QStringLiteral(
            "QPushButton { background:transparent; border:none; border-radius:4px; padding:4px; }"
            "QPushButton:hover { background:#eaecf0; }"));
        layout->addWidget(remove);
        const QString id = context.value(QStringLiteral("id")).toString();
        if (previewImage) {
            connect(previewImage, &QPushButton::clicked, this,
                    [this, context]() { previewPinnedImage(context); });
        }
        connect(included, &QCheckBox::toggled, this, [this, id, pinned, included](bool checked) {
            if (checked && includedTurnContext().size() + includedPinnedContextIds().size()
                    >= kMaxTurnContextItems) {
                const QSignalBlocker blocker(included);
                included->setChecked(false);
                addNotice(QStringLiteral("一个 turn 最多包含 %1 项上下文。")
                              .arg(kMaxTurnContextItems), true);
                return;
            }
            QList<QJsonObject> &items = pinned ? m_pinnedContextItems : m_contextItems;
            for (QJsonObject &candidate : items) {
                if (candidate.value(QStringLiteral("id")).toString() == id) {
                    candidate.insert(QStringLiteral("included"), checked);
                    break;
                }
            }
            QTimer::singleShot(0, this, &AgentWorkbenchWidget::rebuildContextPanel);
        });
        connect(remove, &QPushButton::clicked, this, [this, id, pinned]() {
            if (pinned) {
                if (!m_pinnedContextMutationRequestId.isEmpty()
                        || !m_pinnedImageImportRequestId.isEmpty()
                        || m_projectId.isEmpty()) return;
                m_pinnedContextMutationRequestId = m_runtime->removePinnedContext(
                    m_projectId, id, m_pinnedContextSetIdentity);
                return;
            }
            m_contextItems.removeIf([&id](const QJsonObject &candidate) {
                return candidate.value(QStringLiteral("id")).toString() == id;
            });
            QTimer::singleShot(0, this, &AgentWorkbenchWidget::rebuildContextPanel);
        });
        if (pinned) {
            connect(moveUp, &QPushButton::clicked, this, [this, pinnedIndex]() {
                if (pinnedIndex <= 0 || !m_pinnedContextMutationRequestId.isEmpty()
                        || !m_pinnedImageImportRequestId.isEmpty()) return;
                m_pinnedContextItems.swapItemsAt(pinnedIndex, pinnedIndex - 1);
                savePinnedContextOrder();
            });
            connect(moveDown, &QPushButton::clicked, this, [this, pinnedIndex]() {
                if (pinnedIndex + 1 >= m_pinnedContextItems.size()
                        || !m_pinnedContextMutationRequestId.isEmpty()
                        || !m_pinnedImageImportRequestId.isEmpty()) return;
                m_pinnedContextItems.swapItemsAt(pinnedIndex, pinnedIndex + 1);
                savePinnedContextOrder();
            });
        }
        m_contextList->setItemWidget(item, row);
        if (included->isChecked()) {
            ++includedCount;
            includedBytes += size;
        }
    };
    for (const QJsonObject &context : std::as_const(m_contextItems)) {
        appendRow(context, false, -1);
    }
    for (int index = 0; index < m_pinnedContextItems.size(); ++index) {
        appendRow(m_pinnedContextItems.at(index), true, index);
    }
    if (m_pinnedContextItems.isEmpty()) {
        m_contextSummary->setText(QStringLiteral("上下文 %1/%2 · 发送 %3 项 · %4 B")
            .arg(m_contextItems.size()).arg(kMaxTurnContextItems)
            .arg(includedCount).arg(includedBytes));
    } else {
        m_contextSummary->setText(QStringLiteral("临时 %1 · 固定 %2 · 发送 %3/%4 项 · %5 B")
            .arg(m_contextItems.size()).arg(m_pinnedContextItems.size())
            .arg(includedCount).arg(kMaxTurnContextItems).arg(includedBytes));
    }
    const int rows = m_contextItems.size() + m_pinnedContextItems.size();
    m_contextList->setFixedHeight(qMin(5, rows) * 28 + 2);
    m_contextPanel->setVisible(rows > 0);
    updateContextStrip();
}

void AgentWorkbenchWidget::clearContextItems()
{
    m_contextItems.clear();
    rebuildContextPanel();
}

QJsonArray AgentWorkbenchWidget::includedTurnContext() const
{
    QJsonArray result;
    for (QJsonObject context : m_contextItems) {
        if (!context.value(QStringLiteral("included")).toBool(true)) continue;
        context.remove(QStringLiteral("included"));
        context.remove(QStringLiteral("size"));
        context.remove(QStringLiteral("truncated"));
        result.append(context);
    }
    return result;
}

void AgentWorkbenchWidget::addTimelineItem(const QJsonObject &item, bool prepend)
{
    const QString id = item.value(QStringLiteral("id")).toString();
    const QString role = item.value(QStringLiteral("role")).toString();
    const QString kind = item.value(QStringLiteral("kind")).toString();
    const QJsonObject data = item.value(QStringLiteral("data")).toObject();
    const QJsonObject output = data.value(QStringLiteral("output")).toObject();
    const QJsonObject artifact = output.value(QStringLiteral("artifact")).toObject();
    const QString artifactReference = artifact.value(QStringLiteral("reference")).toString();
    const QString artifactSession = data.value(QStringLiteral("session_id")).toString();
    const QString content = item.value(QStringLiteral("content")).toString();
    if (id.isEmpty()) return;
    if (QLabel *existing = m_itemLabels.value(id, nullptr)) {
        existing->setText(content);
        if (QPushButton *button = m_itemArtifactButtons.value(id, nullptr)) {
            button->setProperty("artifactReference", artifactReference);
            button->setProperty("artifactSession", artifactSession);
            button->setVisible(!artifactReference.isEmpty() && !artifactSession.isEmpty());
        }
        return;
    }
    m_emptyTimeline->hide();
    auto *bubble = new QFrame(m_timelineContent);
    bubble->setObjectName(QStringLiteral("timelineBubble"));
    bubble->setMinimumWidth(230);
    bubble->setMaximumWidth(340);
    bubble->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    bubble->setStyleSheet(role == QStringLiteral("user")
        ? QStringLiteral("QFrame#timelineBubble { background:#165DFF; border:none; border-radius:7px; }")
        : kind == QStringLiteral("command")
            ? QStringLiteral("QFrame#timelineBubble { background:#f8fafc; border:1px solid #d0d5dd; border-radius:7px; }")
        : QStringLiteral("QFrame#timelineBubble { background:#ffffff; border:1px solid #e4e7ec; border-radius:7px; }"));
    auto *layout = new QVBoxLayout(bubble);
    layout->setContentsMargins(11, 9, 11, 9);
    layout->setSpacing(3);
    const QString risk = data.value(QStringLiteral("risk")).toObject()
        .value(QStringLiteral("level")).toString();
    const QString roleText = role == QStringLiteral("user") ? QStringLiteral("你")
        : role == QStringLiteral("system") ? QStringLiteral("运行时")
        : kind == QStringLiteral("command")
            ? QStringLiteral("命令 · 只读沙箱 · 风险 %1").arg(risk)
                                            : QStringLiteral("Aegisy Agent");
    auto *roleLabel = new QLabel(roleText, bubble);
    roleLabel->setTextFormat(Qt::PlainText);
    if (kind == QStringLiteral("command")) {
        roleLabel->setObjectName(QStringLiteral("timelineCommandRole"));
    }
    roleLabel->setStyleSheet(role == QStringLiteral("user")
        ? QStringLiteral("color:#dbeafe; font-size:9px; font-weight:700;")
        : kind == QStringLiteral("error")
            ? QStringLiteral("color:#b42318; font-size:9px; font-weight:700;")
        : kind == QStringLiteral("command") && risk == QStringLiteral("high")
            ? QStringLiteral("color:#b42318; font-size:9px; font-weight:700;")
        : kind == QStringLiteral("command") && risk == QStringLiteral("medium")
            ? QStringLiteral("color:#b54708; font-size:9px; font-weight:700;")
        : kind == QStringLiteral("command")
            ? QStringLiteral("color:#067647; font-size:9px; font-weight:700;")
        : QStringLiteral("color:#165DFF; font-size:9px; font-weight:700;"));
    auto *message = new QLabel(content, bubble);
    message->setTextFormat(Qt::PlainText);
    if (kind == QStringLiteral("command")) {
        message->setObjectName(QStringLiteral("timelineCommandContent"));
        message->setStyleSheet(QStringLiteral(
            "background:transparent; color:#344054; font-family:Menlo,Consolas,monospace; font-size:10px;"));
    }
    message->setMinimumWidth(200);
    message->setMaximumWidth(310);
    message->setWordWrap(true);
    message->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (kind != QStringLiteral("command")) {
        message->setStyleSheet(role == QStringLiteral("user")
            ? QStringLiteral("background:transparent; color:#ffffff; font-size:12px;")
            : QStringLiteral("background:transparent; color:#344054; font-size:12px;"));
    }
    layout->addWidget(roleLabel);
    layout->addWidget(message);
    if (kind == QStringLiteral("command")) {
        auto *artifactButton = new QPushButton(QStringLiteral("查看完整输出"), bubble);
        artifactButton->setObjectName(QStringLiteral("timelineCommandArtifactButton"));
        artifactButton->setFixedHeight(26);
        artifactButton->setVisible(!artifactReference.isEmpty() && !artifactSession.isEmpty());
        artifactButton->setProperty("artifactReference", artifactReference);
        artifactButton->setProperty("artifactSession", artifactSession);
        artifactButton->setStyleSheet(QStringLiteral(
            "QPushButton { background:#ffffff; color:#344054; border:1px solid #d0d5dd;"
            "border-radius:5px; padding:2px 8px; font-size:9px; text-align:left; }"
            "QPushButton:hover { background:#f2f4f7; }"));
        connect(artifactButton, &QPushButton::clicked, this, [this, artifactButton, id]() {
            const QString reference = artifactButton->property("artifactReference").toString();
            const QString sessionId = artifactButton->property("artifactSession").toString();
            if (reference.isEmpty() || sessionId.isEmpty()) return;
            const QString requestId = m_runtime->readCommandArtifact(sessionId, reference);
            if (!requestId.isEmpty()) {
                artifactButton->setEnabled(false);
                m_commandArtifactRequests.insert(requestId, id);
            }
        });
        layout->addWidget(artifactButton);
        m_itemArtifactButtons.insert(id, artifactButton);
    }
    m_itemLabels.insert(id, message);
    const int insertAt = prepend ? 1 : qMax(0, m_timelineLayout->count() - 1);
    m_timelineLayout->insertWidget(insertAt, bubble, 0,
        role == QStringLiteral("user") ? Qt::AlignRight : Qt::AlignLeft);
    if (!prepend) {
        QTimer::singleShot(0, m_timelineScroll->verticalScrollBar(),
                          [bar = m_timelineScroll->verticalScrollBar()]() {
            bar->setValue(bar->maximum());
        });
    }
}

void AgentWorkbenchWidget::addNotice(const QString &text, bool error)
{
    if (text.isEmpty() || !m_timelineLayout) return;
    m_emptyTimeline->hide();
    auto *label = new QLabel(text, m_timelineContent);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(error
        ? QStringLiteral("color:#b42318; background:#fef3f2; border:1px solid #fecdca; border-radius:6px; padding:7px; font-size:10px;")
        : QStringLiteral("color:#475467; background:#f2f4f7; border:none; border-radius:6px; padding:7px; font-size:10px;"));
    m_timelineLayout->insertWidget(qMax(0, m_timelineLayout->count() - 1), label);
}

bool AgentWorkbenchWidget::storeSessionRuntimeBinding(
    const QString &sessionId, const QJsonObject &runtime)
{
    if (sessionId.isEmpty()) return false;
    const QString adapter = runtime.value(QStringLiteral("adapter")).toString().trimmed();
    const QString version = runtime.value(QStringLiteral("version"))
        .toString(runtime.value(QStringLiteral("adapter_version")).toString()).trimmed();
    const QString provider = runtime.value(QStringLiteral("provider")).toString().trimmed();
    const QString model = runtime.value(QStringLiteral("model")).toString().trimmed();
    const QString permission = runtime.value(
        QStringLiteral("permission_profile")).toString().trimmed();
    const bool valid = isBoundedRuntimeMetadata(adapter, 128, true)
        && isBoundedRuntimeMetadata(version, 64, true)
        && isBoundedRuntimeMetadata(provider, 128, false)
        && isBoundedRuntimeMetadata(model, 128, false)
        && permission == QStringLiteral("read-only");
    if (!valid) {
        m_sessionRuntimeBindings.remove(sessionId);
        updateSessionRuntimePresentation();
        return false;
    }
    m_sessionRuntimeBindings.insert(sessionId, QJsonObject{
        {QStringLiteral("adapter"), adapter},
        {QStringLiteral("version"), version},
        {QStringLiteral("provider"), provider},
        {QStringLiteral("model"), model},
        {QStringLiteral("permission_profile"), permission},
    });
    updateSessionRuntimePresentation();
    return true;
}

bool AgentWorkbenchWidget::storeSessionWorkspaceBinding(
    const QString &sessionId, const QJsonObject &workspace)
{
    if (sessionId.isEmpty()) return false;
    if (workspace.isEmpty()) {
        m_sessionWorkspaceBindings.remove(sessionId);
        updateContextStrip();
        return false;
    }
    const QString boundSessionId = workspace.value(
        QStringLiteral("session_id")).toString();
    const QString projectId = workspace.value(QStringLiteral("project_id")).toString();
    const QString rootId = workspace.value(QStringLiteral("root_id")).toString();
    const QString workspaceKind = workspace.value(
        QStringLiteral("workspace_kind")).toString();
    const QString gitState = workspace.value(QStringLiteral("git_state")).toString();
    const QJsonValue branchValue = workspace.value(QStringLiteral("branch"));
    const QString branch = branchValue.isString() ? branchValue.toString() : QString();
    const bool branchRedacted = workspace.value(
        QStringLiteral("branch_redacted")).toBool();
    const QJsonValue headValue = workspace.value(QStringLiteral("head_oid"));
    const QString head = headValue.isString() ? headValue.toString() : QString();
    const bool detached = workspace.value(QStringLiteral("detached")).toBool();
    const bool unborn = workspace.value(QStringLiteral("unborn")).toBool();
    const bool dedicated = workspace.value(
        QStringLiteral("dedicated_worktree")).toBool();
    const QJsonValue capturedValue = workspace.value(QStringLiteral("captured_at_ms"));
    const bool gitStateValid = gitState == QStringLiteral("unavailable")
        || gitState == QStringLiteral("not-repository")
        || gitState == QStringLiteral("repository-only")
        || gitState == QStringLiteral("worktree");
    const bool branchValid = branchValue.isNull()
        || (branchValue.isString() && !branch.isEmpty()
            && isBoundedRuntimeMetadata(branch, 512, true));
    const bool headValid = headValue.isNull()
        || (headValue.isString() && isLowerHexIdentity(head, 40, 64));
    const bool booleansPresent = workspace.value(QStringLiteral("branch_redacted")).isBool()
        && workspace.value(QStringLiteral("detached")).isBool()
        && workspace.value(QStringLiteral("unborn")).isBool()
        && workspace.value(QStringLiteral("dedicated_worktree")).isBool()
        && workspace.value(QStringLiteral("raw_paths_included")).isBool()
        && workspace.value(QStringLiteral("permission_granted")).isBool();
    const bool branchStateValid = branchRedacted
        ? branchValue.isNull()
        : !(detached && branchValue.isString());
    const bool valid = workspace.value(QStringLiteral("schema_version")).toString()
            == QStringLiteral("session-workspace-binding/0.1")
        && boundSessionId == sessionId
        && isBoundedRuntimeMetadata(projectId, 128, true)
        && isBoundedRuntimeMetadata(rootId, 128, true)
        && workspaceKind == QStringLiteral("project-root")
        && gitStateValid && branchValid && headValid && booleansPresent
        && branchStateValid && !dedicated
        && capturedValue.isDouble() && capturedValue.toDouble() >= 0
        && !workspace.value(QStringLiteral("raw_paths_included")).toBool(true)
        && !workspace.value(QStringLiteral("permission_granted")).toBool(true)
        && (gitState == QStringLiteral("worktree")
                || (branchValue.isNull() && headValue.isNull() && !detached && !unborn));
    if (!valid) {
        m_sessionWorkspaceBindings.remove(sessionId);
        updateContextStrip();
        return false;
    }
    m_sessionWorkspaceBindings.insert(sessionId, workspace);
    updateContextStrip();
    return true;
}

QJsonObject AgentWorkbenchWidget::activeSessionRuntimeBinding() const
{
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    return m_sessionRuntimeBindings.value(sessionId);
}

QJsonObject AgentWorkbenchWidget::activeSessionWorkspaceBinding() const
{
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    return m_sessionWorkspaceBindings.value(sessionId);
}

void AgentWorkbenchWidget::updateSessionRuntimePresentation()
{
    if (!m_modelPicker) return;
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    const QJsonObject binding = m_sessionRuntimeBindings.value(sessionId);
    QString display;
    QString tooltip;
    if (sessionId.isEmpty()) {
        display = QStringLiteral("Aegisy / Codex Auto");
        tooltip = QStringLiteral("会话创建后显示持久化 Runtime 与模型绑定");
    } else if (binding.isEmpty()) {
        display = QStringLiteral("Provider -- / Model --");
        tooltip = QStringLiteral("当前会话的 Runtime binding 不可用；权限保持只读门控");
    } else {
        const QString provider = runtimeMetadataLabel(
            binding, QStringLiteral("provider"));
        const QString model = runtimeMetadataLabel(binding, QStringLiteral("model"));
        display = QStringLiteral("%1 / %2").arg(provider, model);
        tooltip = QStringLiteral("%1 · %2 · %3")
            .arg(binding.value(QStringLiteral("adapter")).toString(),
                 binding.value(QStringLiteral("version")).toString(),
                 binding.value(QStringLiteral("permission_profile")).toString());
    }
    if (m_modelProfileReadOnlyAvailable) {
        tooltip += QStringLiteral(" · Profile 元数据只读");
        if (m_modelProfileSnapshotValid) {
            tooltip += QStringLiteral("（%1 个）").arg(m_modelProfileCount);
        }
    }
    if (m_modelPicker->count() != 1 || m_modelPicker->itemText(0) != display) {
        const QSignalBlocker blocker(m_modelPicker);
        m_modelPicker->clear();
        m_modelPicker->addItem(display);
    }
    m_modelPicker->setToolTip(tooltip);
}

void AgentWorkbenchWidget::updateContextStrip()
{
    if (!m_contextStrip) return;
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    if (m_contextInspectButton) {
        const bool blocked = m_runtimeRecoveryMode || currentSessionRecoveryRequired()
            || currentSessionDeletionPending() || currentOperationStatusBlocked();
        const QStringList pinnedContextIds = includedPinnedContextIds();
        m_contextInspectButton->setEnabled(!blocked && !m_turnRunning
            && (!m_contextItems.isEmpty() || !pinnedContextIds.isEmpty())
            && !sessionId.isEmpty()
            && m_runtime && m_runtime->isReady()
            && m_contextInspectRequestId.isEmpty());
    }
    const int selectedCount = includedTurnContext().size() + includedPinnedContextIds().size();
    const QString context = selectedCount == 0
        ? QStringLiteral("上下文 0") : QStringLiteral("上下文 %1").arg(selectedCount);
    const QString project = m_projectRoot.isEmpty()
        ? QStringLiteral("项目 --") : QStringLiteral("项目 %1").arg(QFileInfo(m_projectRoot).fileName());
    updateSessionRuntimePresentation();
    const QJsonObject binding = activeSessionRuntimeBinding();
    const QJsonObject workspaceBinding = activeSessionWorkspaceBinding();
    const bool hasSession = !sessionId.isEmpty();
    const QString adapter = binding.value(QStringLiteral("adapter")).toString();
    const QString version = binding.value(QStringLiteral("version")).toString();
    const QString provider = runtimeMetadataLabel(
        binding, QStringLiteral("provider"));
    const QString model = runtimeMetadataLabel(binding, QStringLiteral("model"));
    const QString runtime = m_runtimeRecoveryMode
        ? QStringLiteral("Runtime 恢复")
        : !adapter.isEmpty()
            ? QStringLiteral("Runtime %1").arg(adapter)
            : (m_runtime && m_runtime->isReady()
                   ? (hasSession ? QStringLiteral("Runtime 未绑定")
                                 : QStringLiteral("Runtime 就绪"))
                   : QStringLiteral("Runtime 连接中"));
    const QString permission = binding.value(QStringLiteral("permission_profile")).toString()
            == QStringLiteral("read-only")
        ? QStringLiteral("权限 只读")
        : (hasSession ? QStringLiteral("权限 未知（只读门控）")
                      : QStringLiteral("权限 只读默认"));
    const QString rootId = workspaceBinding.value(QStringLiteral("root_id")).toString();
    const QString workspace = rootId.isEmpty()
        ? (hasSession && m_mode == QStringLiteral("work")
              ? QStringLiteral("工作区 未绑定") : QStringLiteral("工作区 --"))
        : QStringLiteral("工作区 %1").arg(rootId);
    const QString boundBranch = workspaceBinding.value(QStringLiteral("branch")).toString();
    const QString gitState = workspaceBinding.value(QStringLiteral("git_state")).toString();
    const bool branchRedacted = workspaceBinding.value(
        QStringLiteral("branch_redacted")).toBool();
    const bool detached = workspaceBinding.value(QStringLiteral("detached")).toBool();
    const bool branchDrift = !boundBranch.isEmpty() && !m_gitCurrentBranch.isEmpty()
        && boundBranch != m_gitCurrentBranch;
    QString branch;
    if (!hasSession) {
        branch = m_gitCurrentBranch.isEmpty()
            ? QStringLiteral("分支 --") : QStringLiteral("分支 %1").arg(m_gitCurrentBranch);
    } else if (workspaceBinding.isEmpty()) {
        branch = m_mode == QStringLiteral("work")
            ? QStringLiteral("分支 未绑定") : QStringLiteral("分支 --");
    } else if (gitState != QStringLiteral("worktree")) {
        branch = gitState == QStringLiteral("not-repository")
            ? QStringLiteral("分支 非 Git") : QStringLiteral("分支 不可用");
    } else if (branchRedacted) {
        branch = QStringLiteral("分支 已隐藏");
    } else if (detached) {
        branch = QStringLiteral("分支 分离 HEAD");
    } else if (!boundBranch.isEmpty()) {
        branch = branchDrift
            ? QStringLiteral("分支 %1（已漂移）").arg(boundBranch)
            : QStringLiteral("分支 %1").arg(boundBranch);
    } else {
        branch = QStringLiteral("分支 --");
    }
    m_contextStrip->setText(QStringLiteral("%1 · %2 · %3 · 模型 %4 · %5 · %6 · %7")
        .arg(m_mode == QStringLiteral("chat") ? QStringLiteral("Chat") : QStringLiteral("Work"),
             QStringLiteral("%1 / %2").arg(project, workspace), runtime, model,
             permission, branch, context));
    m_contextStrip->setToolTip(QStringLiteral(
        "执行上下文：%1\n%2\n%3\n%4%5\nProvider/模型：%6 / %7\n%8\n%9%10\n%11")
        .arg(m_mode == QStringLiteral("chat") ? QStringLiteral("Chat") : QStringLiteral("Work"),
             project, workspace, runtime,
             version.isEmpty() ? QString() : QStringLiteral(" · %1").arg(version),
             provider, model, permission, branch,
             branchDrift ? QStringLiteral("；当前只读观测：%1").arg(m_gitCurrentBranch)
                         : QString(),
             context));
}

void AgentWorkbenchWidget::inspectContext()
{
    if (!m_runtime || !m_runtime->isReady()
            || (m_contextItems.isEmpty() && includedPinnedContextIds().isEmpty())) return;
    const QString sessionId = m_mode == QStringLiteral("work")
        ? m_workSessionId : m_chatSessionId;
    if (sessionId.isEmpty()) {
        addNotice(QStringLiteral("请先发送一条消息创建会话，再预检上下文。"), true);
        return;
    }
    if (!m_contextInspectRequestId.isEmpty()) return;
    QJsonArray context;
    for (QJsonObject item : std::as_const(m_contextItems)) {
        const bool included = item.value(QStringLiteral("included")).toBool(true);
        item.remove(QStringLiteral("included"));
        item.remove(QStringLiteral("size"));
        item.remove(QStringLiteral("truncated"));
        if (!included && !item.contains(QStringLiteral("exclusion_reason"))) {
            item.insert(QStringLiteral("exclusion_reason"),
                        QStringLiteral("client-excluded"));
        }
        context.append(item);
    }
    const QStringList pinnedContextIds = includedPinnedContextIds();
    if (!pinnedContextIds.isEmpty() && m_pinnedContextSetIdentity.isEmpty()) {
        addNotice(QStringLiteral("固定上下文集合身份未知，请刷新项目后重试。"), true);
        return;
    }
    m_contextInspectRequestId = m_runtime->inspectTurnContext(
        sessionId, context, m_pinnedContextSetIdentity, pinnedContextIds);
    updateContextStrip();
}

void AgentWorkbenchWidget::showContextInspection(const QJsonObject &result)
{
    if (result.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("context-inspector/0.1")
            || result.value(QStringLiteral("content_included")).toBool(true)
            || result.value(QStringLiteral("model_started")).toBool(true)
            || result.value(QStringLiteral("persisted")).toBool(true)) {
        addNotice(QStringLiteral("运行时返回了无效的上下文预检结果，已保持只读。"), true);
        return;
    }
    const QJsonObject context = result.value(QStringLiteral("context")).toObject();
    const QJsonObject manifest = context.value(QStringLiteral("manifest")).toObject();
    const QJsonObject budget = context.value(QStringLiteral("budget")).toObject();
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("发送前上下文预检"));
    dialog->setMinimumSize(700, 430);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto *title = new QLabel(QStringLiteral("上下文将如何进入下一次 turn"), dialog);
    title->setStyleSheet(QStringLiteral("font-size:14px; font-weight:700; color:#101828;"));
    layout->addWidget(title);
    const QJsonObject tokenizer = budget.value(QStringLiteral("tokenizer")).toObject();
    const QString tokenizerAuthority = tokenizer.value(QStringLiteral("authority")).toString();
    const QString tokenizerLabel = tokenizerAuthority.isEmpty()
        ? QStringLiteral("tokenizer 状态未知")
        : QStringLiteral("tokenizer：%1").arg(tokenizerAuthority);
    const QString summary = QStringLiteral(
        "%1 项 · %2 · 预算 %3/%4 · 估算 %5 tokens · %6 · %7")
        .arg(context.value(QStringLiteral("item_count")).toInt())
        .arg(formatByteCount(context.value(QStringLiteral("bytes")).toVariant().toLongLong()))
        .arg(formatByteCount(budget.value(QStringLiteral("allocated_bytes"))
                            .toVariant().toLongLong()))
        .arg(formatByteCount(budget.value(QStringLiteral("max_total_bytes"))
                            .toVariant().toLongLong()))
        .arg(manifest.value(QStringLiteral("estimated_tokens")).toVariant().toLongLong())
        .arg(tokenizerLabel)
        .arg(context.value(QStringLiteral("truncated")).toBool()
                 ? QStringLiteral("已截断") : QStringLiteral("未截断"));
    auto *summaryLabel = new QLabel(summary, dialog);
    summaryLabel->setObjectName(QStringLiteral("agentContextInspectionSummary"));
    summaryLabel->setStyleSheet(QStringLiteral(
        "color:#475467; background:#f2f4f7; border:1px solid #e4e7ec;"
        "border-radius:6px; padding:8px;"));
    layout->addWidget(summaryLabel);
    auto *table = new QTreeWidget(dialog);
    table->setObjectName(QStringLiteral("agentContextInspectionTable"));
    table->setColumnCount(7);
    table->setHeaderLabels({QStringLiteral("来源"), QStringLiteral("类型"),
                            QStringLiteral("信任"), QStringLiteral("大小"),
                            QStringLiteral("估算"), QStringLiteral("状态"),
                            QStringLiteral("原因")});
    table->setRootIsDecorated(false);
    table->setUniformRowHeights(true);
    table->setAlternatingRowColors(true);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->header()->setStretchLastSection(true);
    table->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    const QJsonArray budgetEntries = budget.value(QStringLiteral("entries")).toArray();
    QHash<QString, QJsonObject> budgetById;
    for (const QJsonValue &value : budgetEntries) {
        const QJsonObject entry = value.toObject();
        budgetById.insert(entry.value(QStringLiteral("id")).toString(), entry);
    }
    for (const QJsonValue &value : manifest.value(QStringLiteral("entries")).toArray()) {
        const QJsonObject entry = value.toObject();
        const QJsonObject budgetEntry = budgetById.value(entry.value(QStringLiteral("id"))
                                                               .toString());
        auto *row = new QTreeWidgetItem(table);
        QString source = entry.value(QStringLiteral("source")).toString();
        for (const QJsonObject &clientItem : std::as_const(m_contextItems)) {
            if (clientItem.value(QStringLiteral("id")).toString()
                    == entry.value(QStringLiteral("id")).toString()) {
                source = QStringLiteral("%1 · %2")
                    .arg(clientItem.value(QStringLiteral("label")).toString(), source);
                break;
            }
        }
        row->setText(0, source);
        row->setText(1, entry.value(QStringLiteral("kind")).toString());
        row->setText(2, entry.value(QStringLiteral("trust")).toString());
        row->setText(3, formatByteCount(budgetEntry.value(QStringLiteral("requested_bytes"))
                                        .toVariant().toLongLong()));
        row->setText(4, QStringLiteral("%1 tokens")
                         .arg(budgetEntry.value(QStringLiteral("estimated_tokens"))
                                  .toVariant().toLongLong()));
        const bool included = entry.value(QStringLiteral("included")).toBool();
        row->setText(5, included ? QStringLiteral("包含") : QStringLiteral("排除"));
        row->setText(6, entry.value(QStringLiteral("inclusion_reason")).toString());
        row->setToolTip(0, QStringLiteral("新鲜度：%1")
                            .arg(entry.value(QStringLiteral("freshness")).toString()));
    }
    layout->addWidget(table, 1);
    auto *footer = new QLabel(QStringLiteral(
        "只读预检：不会启动模型、写入历史或返回文件/指令正文。上下文内容仍按 untrusted-data 处理。"),
                              dialog);
    footer->setWordWrap(true);
    footer->setStyleSheet(QStringLiteral("color:#667085; font-size:10px;"));
    layout->addWidget(footer);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    dialog->show();
}
