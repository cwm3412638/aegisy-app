#include "agent_workbench_widget.h"
#include "agent_runtime_client.h"
#include "app_theme.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDebug>
#include <QImage>
#include <QElapsedTimer>
#include <QFile>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTreeWidget>
#include <QPlainTextEdit>
#include <algorithm>
#include <iterator>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) qCritical() << message;
    return condition;
}

int nonTransparentPixels(const QImage &image)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 32) ++count;
        }
    }
    return count;
}

bool isLightPixel(const QImage &image, const QPoint &point)
{
    if (!image.rect().contains(point)) return false;
    const QColor color = image.pixelColor(point);
    return color.red() > 180 && color.green() > 180 && color.blue() > 180;
}

QPushButton *buttonWithText(QWidget &root, const QString &text)
{
    for (QPushButton *button : root.findChildren<QPushButton *>()) {
        if (button->text() == text) return button;
    }
    return nullptr;
}

template <typename Predicate>
bool waitUntil(QApplication &application, Predicate predicate, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        application.processEvents();
        QThread::msleep(10);
    }
    application.processEvents();
    return predicate();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    AppTheme::apply(application);
    QTemporaryDir workbenchData;
    if (!expect(workbenchData.isValid(), "cannot create isolated Workbench data root")) {
        return 1;
    }
    qputenv("AEGISY_WORKBENCH_DATA_ROOT", workbenchData.path().toUtf8());
    AgentWorkbenchWidget workbench;
    workbench.resize(1100, 700);
    workbench.show();
    application.processEvents();

    QPushButton *runtimeRestart = workbench.findChild<QPushButton *>(
        QStringLiteral("agentRuntimeRestartButton"));
    QLabel *runtimeStatus = workbench.findChild<QLabel *>(QStringLiteral("agentRuntimeStatus"));

#ifdef AEGISY_EXPECT_AGENTD
    if (!expect(runtimeStatus
                    && waitUntil(application, [runtimeStatus]() {
                        return runtimeStatus->text().startsWith(QStringLiteral("●"));
                    }),
                "workbench did not complete the AAP handshake")) {
        return 1;
    }
    if (!expect(QFileInfo::exists(workbenchData.filePath(
                    QStringLiteral("aegisy-workbench.sqlite3"))),
                "Qt host did not configure the durable Workbench data root")) {
        return 1;
    }
    QTextEdit *composer = workbench.findChild<QTextEdit *>(QStringLiteral("agentComposer"));
    QPushButton *send = workbench.findChild<QPushButton *>(QStringLiteral("agentSendButton"));
    if (!expect(composer && send && send->isEnabled(),
                "ready runtime did not enable the composer")) {
        return 1;
    }
    composer->setPlainText(QStringLiteral("验证 AAP 对话链路"));
    send->click();
    if (!expect(waitUntil(application, [&workbench]() {
                    return workbench.findChildren<QFrame *>(QStringLiteral("timelineBubble")).size() >= 2;
                }),
                "AAP turn did not render user and agent timeline items")) {
        return 1;
    }
#endif

    QSplitter *splitter = workbench.findChild<QSplitter *>();
    QTabWidget *tabs = workbench.findChild<QTabWidget *>();
    QComboBox *modelPicker = workbench.findChild<QComboBox *>();
    QTreeWidget *fileTree = workbench.findChild<QTreeWidget *>(QStringLiteral("agentFileTree"));
    QLineEdit *fileFilter = workbench.findChild<QLineEdit *>(QStringLiteral("agentFileFilter"));
    QLineEdit *workspaceSearch = workbench.findChild<QLineEdit *>(
        QStringLiteral("agentWorkspaceSearchInput"));
    QComboBox *workspaceSearchMode = workbench.findChild<QComboBox *>(
        QStringLiteral("agentWorkspaceSearchMode"));
    QTreeWidget *workspaceSearchResults = workbench.findChild<QTreeWidget *>(
        QStringLiteral("agentWorkspaceSearchResults"));
    QLabel *workspaceSearchStatus = workbench.findChild<QLabel *>(
        QStringLiteral("agentWorkspaceSearchStatus"));
    QPushButton *workspaceSearchButton = workbench.findChild<QPushButton *>(
        QStringLiteral("agentWorkspaceSearchButton"));
    QPushButton *workspaceSearchCancel = workbench.findChild<QPushButton *>(
        QStringLiteral("agentWorkspaceSearchCancelButton"));
    QPushButton *workspaceSearchMore = workbench.findChild<QPushButton *>(
        QStringLiteral("agentWorkspaceSearchMoreButton"));
    QTreeWidget *repositorySymbols = workbench.findChild<QTreeWidget *>(
        QStringLiteral("agentRepositorySymbols"));
    QTreeWidget *repositoryDependencies = workbench.findChild<QTreeWidget *>(
        QStringLiteral("agentRepositoryDependencies"));
    QPlainTextEdit *repositoryMap = workbench.findChild<QPlainTextEdit *>(
        QStringLiteral("agentRepositoryMapPreview"));
    QComboBox *repositoryBudget = workbench.findChild<QComboBox *>(
        QStringLiteral("agentRepositoryMapBudget"));
    QLabel *repositoryStatus = workbench.findChild<QLabel *>(
        QStringLiteral("agentRepositoryStatus"));
    QPushButton *repositoryRefresh = workbench.findChild<QPushButton *>(
        QStringLiteral("agentRepositoryRefreshButton"));
    QPushButton *repositoryCancel = workbench.findChild<QPushButton *>(
        QStringLiteral("agentRepositoryCancelButton"));
    QTreeWidget *languageResults = workbench.findChild<QTreeWidget *>(
        QStringLiteral("agentLanguageResults"));
    QTreeWidget *languageDiagnostics = workbench.findChild<QTreeWidget *>(
        QStringLiteral("agentLanguageDiagnostics"));
    QLabel *languageStatus = workbench.findChild<QLabel *>(
        QStringLiteral("agentLanguageStatus"));
    QPushButton *languageDefinition = workbench.findChild<QPushButton *>(
        QStringLiteral("agentLanguageDefinitionButton"));
    QPushButton *languageReferences = workbench.findChild<QPushButton *>(
        QStringLiteral("agentLanguageReferencesButton"));
    QPushButton *languageDiagnosticsButton = workbench.findChild<QPushButton *>(
        QStringLiteral("agentLanguageDiagnosticsButton"));
    QPushButton *languageRaw = workbench.findChild<QPushButton *>(
        QStringLiteral("agentLanguageRawButton"));
    QPushButton *languageStop = workbench.findChild<QPushButton *>(
        QStringLiteral("agentLanguageStopButton"));
    QPlainTextEdit *diagnosticRawPreview = workbench.findChild<QPlainTextEdit *>(
        QStringLiteral("agentDiagnosticRawPreview"));
    QPlainTextEdit *editor = workbench.findChild<QPlainTextEdit *>(
        QStringLiteral("agentReadOnlyEditor"));
    QTabBar *editorTabs = workbench.findChild<QTabBar *>(QStringLiteral("agentEditorTabs"));
    QComboBox *recentFiles = workbench.findChild<QComboBox *>(
        QStringLiteral("agentRecentFiles"));
    QLineEdit *editorFind = workbench.findChild<QLineEdit *>(QStringLiteral("agentEditorFind"));
    QLineEdit *editorReplace = workbench.findChild<QLineEdit *>(
        QStringLiteral("agentEditorReplace"));
    QPushButton *replaceAll = workbench.findChild<QPushButton *>(
        QStringLiteral("agentEditorReplaceAll"));
    QLabel *editorPath = workbench.findChild<QLabel *>(QStringLiteral("agentEditorPath"));
    QLabel *editorMeta = workbench.findChild<QLabel *>(QStringLiteral("agentEditorMeta"));
    QPushButton *editorSave = workbench.findChild<QPushButton *>(
        QStringLiteral("agentEditorSaveButton"));
    QPushButton *editorReload = workbench.findChild<QPushButton *>(
        QStringLiteral("agentEditorReloadButton"));
    QPushButton *editorSplit = workbench.findChild<QPushButton *>(
        QStringLiteral("agentEditorSplitButton"));
    QPushButton *sendButton = workbench.findChild<QPushButton *>(
        QStringLiteral("agentSendButton"));
    QPushButton *attachContext = workbench.findChild<QPushButton *>(
        QStringLiteral("agentAttachContextButton"));
    QWidget *contextPanel = workbench.findChild<QWidget *>(
        QStringLiteral("agentContextPanel"));
    QLabel *contextSummary = workbench.findChild<QLabel *>(
        QStringLiteral("agentContextSummary"));
    QListWidget *contextList = workbench.findChild<QListWidget *>(
        QStringLiteral("agentContextList"));
    QPushButton *editorContext = workbench.findChild<QPushButton *>(
        QStringLiteral("agentEditorContextButton"));
    QPlainTextEdit *terminalExcerpt = workbench.findChild<QPlainTextEdit *>(
        QStringLiteral("agentTerminalExcerptPreview"));
    QComboBox *terminalPicker = workbench.findChild<QComboBox *>(
        QStringLiteral("agentTerminalPicker"));
    QLabel *terminalStatus = workbench.findChild<QLabel *>(
        QStringLiteral("agentTerminalStatus"));
    QPushButton *terminalNew = workbench.findChild<QPushButton *>(
        QStringLiteral("agentTerminalNewButton"));
    QPushButton *terminalStop = workbench.findChild<QPushButton *>(
        QStringLiteral("agentTerminalStopButton"));
    QPushButton *terminalRestart = workbench.findChild<QPushButton *>(
        QStringLiteral("agentTerminalRestartButton"));
    QPushButton *terminalRemove = workbench.findChild<QPushButton *>(
        QStringLiteral("agentTerminalRemoveButton"));
    QPushButton *terminalContext = workbench.findChild<QPushButton *>(
        QStringLiteral("agentTerminalContextButton"));
    QAction *terminalNewForeground = workbench.findChild<QAction *>(
        QStringLiteral("agentTerminalNewForegroundAction"));
    QListWidget *sessionList = workbench.findChild<QListWidget *>(
        QStringLiteral("agentSessionList"));
    QPushButton *retentionSettings = workbench.findChild<QPushButton *>(
        QStringLiteral("agentRetentionSettingsButton"));
    QPushButton *importSession = workbench.findChild<QPushButton *>(
        QStringLiteral("agentImportSessionButton"));
    QPushButton *sessionHistoryMore = workbench.findChild<QPushButton *>(
        QStringLiteral("agentSessionHistoryMoreButton"));
    QLabel *recoveryBanner = workbench.findChild<QLabel *>(
        QStringLiteral("agentRecoveryBanner"));
    QPlainTextEdit *gitDiff = workbench.findChild<QPlainTextEdit *>(
        QStringLiteral("agentGitDiffPreview"));
    QLabel *gitSummary = workbench.findChild<QLabel *>(
        QStringLiteral("agentGitSummary"));
    QTreeWidget *gitHistory = workbench.findChild<QTreeWidget *>(
        QStringLiteral("agentGitHistory"));
    QComboBox *gitDiffScope = workbench.findChild<QComboBox *>(
        QStringLiteral("agentGitDiffScope"));
    QPushButton *gitRefresh = workbench.findChild<QPushButton *>(
        QStringLiteral("agentGitRefreshButton"));
    QLabel *workspaceEditSummary = workbench.findChild<QLabel *>(
        QStringLiteral("agentWorkspaceEditSummary"));
    QTreeWidget *workspaceEditFiles = workbench.findChild<QTreeWidget *>(
        QStringLiteral("agentWorkspaceEditFiles"));
    QPlainTextEdit *workspaceEditDiff = workbench.findChild<QPlainTextEdit *>(
        QStringLiteral("agentWorkspaceEditDiff"));
    QPushButton *workspaceEditMore = workbench.findChild<QPushButton *>(
        QStringLiteral("agentWorkspaceEditMoreButton"));
    QAction *fileContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentFileTreeContextAction"));
    QAction *searchContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentSearchResultContextAction"));
    QAction *diagnosticContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentDiagnosticContextAction"));
    QAction *terminalContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentTerminalExcerptContextAction"));
    QAction *gitContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentGitDiffContextAction"));
    QPushButton *newSession = buttonWithText(workbench, QStringLiteral("新建会话"));
    QPushButton *openFolder = buttonWithText(workbench, QStringLiteral("打开文件夹"));
    QPushButton *chat = buttonWithText(workbench, QStringLiteral("Chat"));
    QPushButton *work = buttonWithText(workbench, QStringLiteral("Work"));
    AgentRuntimeClient *runtimeClient = workbench.findChild<AgentRuntimeClient *>();
    const QImage image = workbench.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const int pixels = image.width() * image.height();
    const QPoint tabStripTail = tabs
        ? tabs->mapTo(&workbench, QPoint(tabs->width() - 8, 20))
        : QPoint(-1, -1);
    const QPoint modelDropDown = modelPicker
        ? modelPicker->mapTo(&workbench,
                             QPoint(modelPicker->width() - 12, modelPicker->height() / 2))
        : QPoint(-1, -1);

    if (!expect(image.size() == QSize(1100, 700), "workbench dimensions changed")
            || !expect(modelPicker
                           && QFile(QStringLiteral(":/icons/lucide/chevron-down.svg")).exists()
                           && application.styleSheet().contains(
                               QStringLiteral("lucide/chevron-down.svg"))
                           && !application.styleSheet().contains(
                               QStringLiteral("border-top: 6px solid"))
                           && !modelPicker->styleSheet().contains(
                               QStringLiteral("QComboBox::down-arrow")),
                       "combo boxes must use the shared SVG down arrow")
            || !expect(nonTransparentPixels(image) > pixels * 9 / 10,
                       "workbench rendered mostly transparent")
            || !expect(splitter && splitter->count() == 3,
                       "workbench must keep three primary panes")
            || !expect(tabs && tabs->count() == 7,
                       "workspace canvas must expose seven initial views")
            || !expect(workspaceSearch && workspaceSearchMode && workspaceSearchResults
                           && workspaceSearchStatus && workspaceSearchButton
                           && workspaceSearchCancel && workspaceSearchMore
                           && workspaceSearchMode->count() == 3
                           && !workspaceSearchCancel->isEnabled()
                           && !workspaceSearchMore->isEnabled(),
                       "bounded workspace search controls are missing")
            || !expect(repositorySymbols && repositoryDependencies && repositoryMap
                           && repositoryBudget && repositoryStatus && repositoryRefresh
                           && repositoryCancel && !repositoryCancel->isEnabled()
                           && repositoryMap->isReadOnly() && repositoryBudget->count() == 5,
                       "repository structure controls are missing")
            || !expect(languageResults && languageDiagnostics && languageStatus
                           && languageDefinition && languageReferences
                           && languageDiagnosticsButton && languageRaw && languageStop
                           && diagnosticRawPreview && diagnosticRawPreview->isReadOnly()
                           && !languageDefinition->isEnabled()
                           && !languageReferences->isEnabled()
                           && !languageDiagnosticsButton->isEnabled()
                           && !languageRaw->isEnabled(),
                       "language-server bridge controls are missing")
            || !expect(fileTree && editor && editor->isReadOnly(),
                       "read-only file workspace controls are missing")
            || !expect(editorTabs && recentFiles && editorFind && editorReplace && replaceAll
                           && editorTabs->count() == 0,
                       "multi-buffer editor controls are missing")
            || !expect(editorPath && editorMeta && editorSave && editorReload && editorSplit
                           && !editorSave->isEnabled()
                           && !editorReload->isEnabled()
                           && !editorSplit->isEnabled(),
                       "editor save controls have an invalid empty state")
            || !expect(attachContext && contextPanel && contextSummary && contextList
                           && editorContext && terminalExcerpt && gitDiff
                           && fileContextAction && searchContextAction
                           && diagnosticContextAction && terminalContextAction
                           && gitContextAction && contextList->count() == 0
                           && contextPanel->isHidden() && !editorContext->isEnabled(),
                       "structured turn-context controls are missing")
            || !expect(workspaceEditSummary && workspaceEditFiles && workspaceEditDiff
                           && workspaceEditMore && workspaceEditDiff->isReadOnly()
                           && !workspaceEditMore->isEnabled(),
                       "workspace edit preview controls are missing")
            || !expect(gitSummary && gitHistory && gitDiffScope && gitRefresh && gitDiff
                           && gitHistory->columnCount() == 4
                           && gitDiffScope->count() == 3 && gitDiff->isReadOnly()
                           && !gitRefresh->icon().isNull(),
                       "read-only Git query workspace controls are missing")
            || !expect(terminalPicker && terminalStatus && terminalNew && terminalStop
                           && terminalRestart && terminalRemove && terminalContext
                           && terminalNewForeground && sessionList && sessionHistoryMore
                           && retentionSettings && importSession
                           && !retentionSettings->isEnabled()
                           && !importSession->icon().isNull()
                           && terminalPicker->count() == 0
                           && !terminalNew->isEnabled() && !terminalStop->isEnabled()
                           && !terminalRestart->isEnabled() && !terminalRemove->isEnabled()
                           && !terminalContext->isEnabled()
                           && sessionHistoryMore->isHidden()
                           && !sessionHistoryMore->icon().isNull(),
                       "terminal lifecycle controls have an invalid empty state")
            || !expect(recoveryBanner && recoveryBanner->isHidden()
                           && runtimeRestart && !runtimeRestart->isEnabled(),
                       "recovery banner must exist and start hidden for a healthy runtime")
            || !expect(newSession && openFolder && sendButton
                           && !newSession->icon().isNull()
                           && !openFolder->icon().isNull()
                           && !sendButton->icon().isNull()
                           && !editorSave->icon().isNull()
                           && !editorReload->icon().isNull()
                           && !editorSplit->icon().isNull()
                           && !workspaceSearchButton->icon().isNull()
                           && !workspaceSearchCancel->icon().isNull(),
                       "workbench vector icon resources are missing")
            || !expect(fileFilter && fileTree->columnCount() == 3,
                       "searchable Git-decorated file tree is missing")
            || !expect(isLightPixel(image, tabStripTail),
                       "workspace tab strip rendered a dark trailing region")
            || !expect(!isLightPixel(image, modelDropDown),
                       "model selector down arrow did not render")
            || !expect(chat && work && runtimeClient && chat->isChecked() && !work->isChecked(),
                       "Chat must be the explicit default mode")) {
        return 1;
    }

    runtimeClient->runtimeInitialized(QJsonObject{
        {QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("status"), QStringLiteral("read-only-recovery")},
        }},
    });
    application.processEvents();
    if (!expect(!recoveryBanner->isHidden()
                    && recoveryBanner->text().contains(QStringLiteral("只读诊断"))
                    && sendButton->text() == QStringLiteral("只读恢复")
                    && !sendButton->isEnabled() && !newSession->isEnabled()
                    && !openFolder->isEnabled() && !importSession->isEnabled(),
                "whole-store recovery did not expose and enforce the blocking UI")) {
        return 1;
    }
    runtimeClient->runtimeInitialized(QJsonObject{
        {QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("status"), QStringLiteral("ready")},
        }},
    });
    runtimeClient->projectionRecoveryStatusRead(QJsonObject{
        {QStringLiteral("startup"), QJsonObject{
            {QStringLiteral("rebuilt_sessions"), 2},
        }},
        {QStringLiteral("current_quarantined_sessions"), 0},
    });
    application.processEvents();
    if (!expect(!recoveryBanner->isHidden()
                    && recoveryBanner->text().contains(QStringLiteral("自动恢复 2 个"))
                    && sendButton->isEnabled() && newSession->isEnabled()
                    && openFolder->isEnabled() && importSession->isEnabled(),
                "startup projection recovery did not render a non-blocking notice")) {
        return 1;
    }
    runtimeClient->sessionStarted(QStringLiteral("recovery-render-session"), QJsonObject{
        {QStringLiteral("id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("mode"), QStringLiteral("chat")},
        {QStringLiteral("title"), QStringLiteral("Recovery render")},
        {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("provider"), QStringLiteral("fixture")},
            {QStringLiteral("model"), QStringLiteral("fixture")},
            {QStringLiteral("adapter"), QStringLiteral("fixture")},
            {QStringLiteral("version"), QStringLiteral("1")},
            {QStringLiteral("permission_profile"), QStringLiteral("read-only")},
        }},
    });
    runtimeClient->sessionRecoveryStatusRead(QJsonObject{
        {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("recovery_required"), true},
        {QStringLiteral("issues"), QJsonArray{
            QStringLiteral("event-payload-or-sequence-invalid"),
        }},
    });
    application.processEvents();
    if (!expect(recoveryBanner->text().contains(QStringLiteral("当前会话"))
                    && sendButton->text() == QStringLiteral("只读恢复")
                    && !sendButton->isEnabled(),
                "session quarantine did not disable the active composer")) {
        return 1;
    }
    runtimeClient->sessionRecoveryStatusRead(QJsonObject{
        {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("recovery_required"), false},
        {QStringLiteral("issues"), QJsonArray{}},
    });
    runtimeClient->projectionRecoveryStatusRead(QJsonObject{
        {QStringLiteral("startup"), QJsonObject{
            {QStringLiteral("rebuilt_sessions"), 0},
        }},
        {QStringLiteral("current_quarantined_sessions"), 0},
    });
    application.processEvents();
    if (!expect(recoveryBanner->isHidden() && sendButton->isEnabled(),
                "cleared recovery state did not restore the healthy composer")) {
        return 1;
    }
    runtimeClient->runtimeHealthRead(QJsonObject{
        {QStringLiteral("state"), QStringLiteral("exited")},
        {QStringLiteral("restart_required"), true},
    });
    application.processEvents();
    if (!expect(runtimeRestart && runtimeRestart->isEnabled()
                    && runtimeStatus->text().contains(QStringLiteral("Codex 不可用")),
                "exited Codex health did not enable the recovery action")) {
        return 1;
    }
    runtimeClient->runtimeHealthRead(QJsonObject{
        {QStringLiteral("state"), QStringLiteral("running")},
        {QStringLiteral("restart_required"), false},
    });
    application.processEvents();
    if (!expect(!runtimeRestart->isEnabled(),
                "running Codex health did not clear the recovery action")) {
        return 1;
    }
    runtimeClient->requestFailed({}, QStringLiteral("runtime/restart"),
                                 QStringLiteral("Codex App Server restart failed: opaque provider payload"),
                                 -32110);
    application.processEvents();
    bool rawRestartTextVisible = runtimeStatus->toolTip().contains(
        QStringLiteral("opaque provider payload"));
    for (QLabel *label : workbench.findChildren<QLabel *>()) {
        rawRestartTextVisible = rawRestartTextVisible
            || label->text().contains(QStringLiteral("opaque provider payload"));
    }
    if (!expect(runtimeRestart->isEnabled()
                    && runtimeRestart->text() == QStringLiteral("重试 Codex")
                    && runtimeStatus->toolTip().contains(QStringLiteral("错误码 -32110"))
                    && runtimeStatus->toolTip().contains(QStringLiteral("详细信息已隐藏"))
                    && !rawRestartTextVisible,
                "Codex reconnect failure leaked raw provider detail or lost recovery state")) {
        return 1;
    }

    const QJsonObject commandBase{
        {QStringLiteral("id"), QStringLiteral("command-render-fixture")},
        {QStringLiteral("kind"), QStringLiteral("command")},
        {QStringLiteral("role"), QStringLiteral("tool")},
        {QStringLiteral("content"), QStringLiteral("$ printf '<unsafe>'\n")},
    };
    QJsonObject startedCommand = commandBase;
    startedCommand.insert(QStringLiteral("state"), QStringLiteral("started"));
    startedCommand.insert(QStringLiteral("data"), QJsonObject{
        {QStringLiteral("session_id"), QStringLiteral("session-render-fixture")},
        {QStringLiteral("risk"), QJsonObject{{QStringLiteral("level"), QStringLiteral("low")}}},
        {QStringLiteral("output"), QJsonObject{{QStringLiteral("artifact"), QJsonValue::Null}}},
    });
    runtimeClient->timelineEvent(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("item.started")},
        {QStringLiteral("item"), startedCommand},
    });
    QJsonObject completedCommand = startedCommand;
    completedCommand.insert(QStringLiteral("state"), QStringLiteral("completed"));
    completedCommand.insert(QStringLiteral("data"), QJsonObject{
        {QStringLiteral("session_id"), QStringLiteral("session-render-fixture")},
        {QStringLiteral("risk"), QJsonObject{{QStringLiteral("level"), QStringLiteral("low")}}},
        {QStringLiteral("output"), QJsonObject{
            {QStringLiteral("artifact"), QJsonObject{
                {QStringLiteral("reference"), QStringLiteral(
                    "command-output:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")}
            }}
        }},
    });
    runtimeClient->timelineEvent(QJsonObject{
        {QStringLiteral("event"), QStringLiteral("item.completed")},
        {QStringLiteral("item"), completedCommand},
    });
    QPushButton *commandArtifactButton = workbench.findChild<QPushButton *>(
        QStringLiteral("timelineCommandArtifactButton"));
    QLabel *commandContent = workbench.findChild<QLabel *>(
        QStringLiteral("timelineCommandContent"));
    if (!expect(commandArtifactButton && !commandArtifactButton->isHidden()
                    && commandArtifactButton->property("artifactReference").toString()
                        .startsWith(QStringLiteral("command-output:sha256:"))
                    && commandContent && commandContent->textFormat() == Qt::PlainText
                    && commandContent->text().contains(QStringLiteral("<unsafe>")),
                "structured command timeline did not expose a plain-text artifact action")) {
        return 1;
    }

    runtimeClient->timelineEvent(QJsonObject{
        {QStringLiteral("session_id"), QStringLiteral("session-cancel-fixture")},
        {QStringLiteral("turn_id"), QStringLiteral("turn-cancel-fixture")},
        {QStringLiteral("event"), QStringLiteral("turn.started")},
    });
    application.processEvents();
    if (!expect(sendButton->text() == QStringLiteral("停止") && sendButton->isEnabled()
                    && sendButton->width() == 84,
                "running turn did not expose a stable stop action")) {
        return 1;
    }
    runtimeClient->timelineEvent(QJsonObject{
        {QStringLiteral("session_id"), QStringLiteral("session-cancel-fixture")},
        {QStringLiteral("turn_id"), QStringLiteral("turn-cancel-fixture")},
        {QStringLiteral("event"), QStringLiteral("turn.cancellation-acknowledged")},
    });
    application.processEvents();
    if (!expect(sendButton->text() == QStringLiteral("正在停止") && !sendButton->isEnabled(),
                "accepted cancellation was incorrectly presented as a terminal state")) {
        return 1;
    }
    runtimeClient->timelineEvent(QJsonObject{
        {QStringLiteral("session_id"), QStringLiteral("session-cancel-fixture")},
        {QStringLiteral("turn_id"), QStringLiteral("turn-cancel-fixture")},
        {QStringLiteral("event"), QStringLiteral("turn.interrupted")},
    });
    application.processEvents();
    if (!expect(sendButton->text() == QStringLiteral("发送") && sendButton->isEnabled(),
                "interrupted turn did not restore the composer action")) {
        return 1;
    }
    runtimeClient->timelineEvent(QJsonObject{
        {QStringLiteral("session_id"), QStringLiteral("session-cancel-fixture")},
        {QStringLiteral("turn_id"), QStringLiteral("turn-failed-fixture")},
        {QStringLiteral("event"), QStringLiteral("turn.failed")},
        {QStringLiteral("item"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("runtime-error-fixture")},
            {QStringLiteral("kind"), QStringLiteral("error")},
            {QStringLiteral("role"), QStringLiteral("system")},
            {QStringLiteral("state"), QStringLiteral("completed")},
            {QStringLiteral("content"), QStringLiteral("运行时错误（详细内容已隐藏）")},
            {QStringLiteral("data"), QJsonObject{
                {QStringLiteral("schema_version"), QStringLiteral("runtime-error/0.1")},
                {QStringLiteral("class"), QStringLiteral("transport")},
                {QStringLiteral("retryable"), true},
            }},
        }},
    });
    application.processEvents();
    bool failureNoticeVisible = false;
    for (QLabel *label : workbench.findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("任务失败 · 类型：传输 · 可以重试"))) {
            failureNoticeVisible = true;
            break;
        }
    }
    if (!expect(failureNoticeVisible,
                "runtime-error category and retry state were not projected in Qt")) {
        return 1;
    }

    // Provider lifecycle failures must expose only a bounded operation/code state.
    const QString rawProviderFailure =
        QStringLiteral("Codex provider request failed: response body contained [REDACTED]");
    runtimeClient->requestFailed({}, QStringLiteral("session/archive"),
                                 rawProviderFailure, -32143);
    runtimeClient->requestFailed({}, QStringLiteral("session/unarchive"),
                                 QStringLiteral("Codex provider state is not loaded; opaque detail"),
                                 -32141);
    runtimeClient->requestFailed({}, QStringLiteral("session/fork"),
                                 QStringLiteral("Codex provider archive was acknowledged but local persistence failed and compensation also failed"),
                                 -32145);
    application.processEvents();
    bool archiveFailureVisible = false;
    bool restoreFailureVisible = false;
    bool forkFailureVisible = false;
    bool rawProviderTextVisible = false;
    for (QLabel *label : workbench.findChildren<QLabel *>()) {
        const QString text = label->text();
        archiveFailureVisible = archiveFailureVisible
            || text.contains(QStringLiteral("归档会话失败（错误码 -32143；provider 详细信息已隐藏）"));
        restoreFailureVisible = restoreFailureVisible
            || text.contains(QStringLiteral("恢复会话失败（错误码 -32141；provider 详细信息已隐藏）"));
        forkFailureVisible = forkFailureVisible
            || text.contains(QStringLiteral("创建会话分支失败（错误码 -32145；provider 详细信息已隐藏）"));
        rawProviderTextVisible = rawProviderTextVisible
            || text.contains(QStringLiteral("response body contained"))
            || text.contains(QStringLiteral("opaque detail"));
    }
    if (!expect(archiveFailureVisible && restoreFailureVisible && forkFailureVisible
                    && !rawProviderTextVisible,
                "provider lifecycle failures leaked raw detail or lost recovery codes")) {
        return 1;
    }
    const QJsonObject commandDiagnostic{
        {QStringLiteral("path"), QStringLiteral("src/main.rs")},
        {QStringLiteral("line"), 2},
        {QStringLiteral("column"), 5},
        {QStringLiteral("severity"), QStringLiteral("error")},
        {QStringLiteral("message"), QStringLiteral("cannot find function missing")},
        {QStringLiteral("source_kind"), QStringLiteral("command")},
        {QStringLiteral("source_identity"), QStringLiteral("command:rustc")},
        {QStringLiteral("source_command"), QStringLiteral("cargo check")},
        {QStringLiteral("file_hash"), QStringLiteral("content:fixture")},
        {QStringLiteral("freshness"), QStringLiteral("fresh")},
        {QStringLiteral("raw_output_ref"), QStringLiteral(
            "diagnostic-raw:sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")},
    };
    runtimeClient->timelineEvent(QJsonObject{
        {QStringLiteral("session_id"), QStringLiteral("session-command-diagnostic")},
        {QStringLiteral("turn_id"), QStringLiteral("turn-command-diagnostic")},
        {QStringLiteral("event"), QStringLiteral("diagnostics.observed")},
        {QStringLiteral("item"), QJsonObject{
            {QStringLiteral("id"), QStringLiteral("diagnostics-command-fixture")},
            {QStringLiteral("kind"), QStringLiteral("diagnostic")},
            {QStringLiteral("role"), QStringLiteral("tool")},
            {QStringLiteral("state"), QStringLiteral("completed")},
            {QStringLiteral("content"), QStringLiteral("rustc reported one diagnostic")},
            {QStringLiteral("data"), QJsonObject{
                {QStringLiteral("source_kind"), QStringLiteral("command")},
                {QStringLiteral("source_identity"), QStringLiteral("command:rustc")},
                {QStringLiteral("raw_output_ref"), commandDiagnostic.value(
                    QStringLiteral("raw_output_ref"))},
                {QStringLiteral("diagnostics"), QJsonArray{commandDiagnostic}},
            }},
        }},
    });
    application.processEvents();
    if (!expect(languageDiagnostics->topLevelItemCount() == 1
                    && languageDiagnostics->topLevelItem(0)->text(1)
                        == QStringLiteral("src/main.rs")
                    && languageDiagnostics->topLevelItem(0)->toolTip(4).contains(
                        QStringLiteral("命令：cargo check"))
                    && languageStatus->text().contains(QStringLiteral("命令诊断"))
                    && languageStatus->text().contains(QStringLiteral("command:rustc")),
                "command diagnostics did not render provenance and navigation metadata")) {
        return 1;
    }

    auto *sourceItem = new QTreeWidgetItem(fileTree, QStringList{QStringLiteral("main.cpp")});
    sourceItem->setData(0, Qt::UserRole, QStringLiteral("src/main.cpp"));
    auto *readmeItem = new QTreeWidgetItem(fileTree, QStringList{QStringLiteral("README.md")});
    readmeItem->setData(0, Qt::UserRole, QStringLiteral("README.md"));
    fileFilter->setText(QStringLiteral("main"));
    application.processEvents();
    if (!expect(!sourceItem->isHidden() && readmeItem->isHidden(),
                "file tree filter did not hide non-matching loaded items")) {
        return 1;
    }
    fileFilter->clear();
    application.processEvents();
    if (!expect(!sourceItem->isHidden() && !readmeItem->isHidden(),
                "clearing file tree filter did not restore loaded items")) {
        return 1;
    }

#ifdef AEGISY_EXPECT_AGENTD
    QTemporaryDir project;
    const QString editablePath = project.filePath(QStringLiteral("editable.txt"));
    QFile editable(editablePath);
    if (!expect(project.isValid() && editable.open(QIODevice::WriteOnly),
                "could not create editable workspace fixture")) {
        return 1;
    }
    editable.write("original\r\ncontent\r\n");
    editable.close();
    QFile second(project.filePath(QStringLiteral("second.txt")));
    if (!expect(second.open(QIODevice::WriteOnly),
                "could not create second editor fixture")) {
        return 1;
    }
    second.write("second\nfile\n");
    second.close();
    for (int fileIndex = 0; fileIndex < 3; ++fileIndex) {
        QFile paged(project.filePath(QStringLiteral("paged-%1.txt").arg(fileIndex)));
        if (!expect(paged.open(QIODevice::WriteOnly),
                    "could not create paged search fixture")) {
            return 1;
        }
        QByteArray content;
        for (int line = 0; line < 20; ++line) {
            content.append("pagehit result\n");
        }
        paged.write(content);
        paged.close();
    }
    QFile binary(project.filePath(QStringLiteral("binary.dat")));
    if (!expect(binary.open(QIODevice::WriteOnly), "could not create binary fallback fixture")) {
        return 1;
    }
    binary.write(QByteArray::fromRawData("\x01\x00\x02", 3));
    binary.close();
    QFile source(project.filePath(QStringLiteral("source.cpp")));
    if (!expect(source.open(QIODevice::WriteOnly),
                "could not create repository index fixture")) {
        return 1;
    }
    const QString sourceContent = QStringLiteral(
        "int add(int a, int b) { return a + b; }\n"
        "int run() { return add(1, 2); }\n");
    source.write(sourceContent.toUtf8());
    source.close();
    AgentRuntimeClient *runtime = workbench.findChild<AgentRuntimeClient *>();
    QString openedProjectId;
    QString openedProjectRoot;
    QString previewSessionId;
    QObject::connect(runtime, &AgentRuntimeClient::projectOpened,
                     &workbench, [&openedProjectId, &openedProjectRoot](
                         const QString &, const QJsonObject &opened) {
        openedProjectId = opened.value(QStringLiteral("id")).toString();
        openedProjectRoot = opened.value(QStringLiteral("root")).toString();
    });
    QObject::connect(runtime, &AgentRuntimeClient::sessionStarted,
                     &workbench, [&previewSessionId, &openedProjectId](
                         const QString &, const QJsonObject &session) {
        if (session.value(QStringLiteral("mode")).toString() == QStringLiteral("work")
                && session.value(QStringLiteral("project_id")).toString()
                    == openedProjectId) {
            previewSessionId = session.value(QStringLiteral("id")).toString();
        }
    });
    runtime->openProject(project.path());
    if (!expect(waitUntil(application, [fileTree, &openedProjectId]() {
                    return !openedProjectId.isEmpty()
                        && !fileTree->findItems(QStringLiteral("editable.txt"),
                                               Qt::MatchExactly | Qt::MatchRecursive).isEmpty();
                }),
                "workspace fixture did not populate the file tree")) {
        return 1;
    }
    if (!expect(retentionSettings->isEnabled(),
                "project retention settings did not enable for an opened project")) {
        return 1;
    }
    runtime->startSession(QStringLiteral("work"), openedProjectId);
    if (!expect(waitUntil(application, [&previewSessionId]() {
                    return !previewSessionId.isEmpty();
                }),
                "workspace edit preview fixture did not create a Work session")) {
        return 1;
    }
    auto sha256 = [](const QByteArray &bytes) {
        return QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    };
    QByteArray proposedContent;
    for (int line = 0; line < 9000; ++line) {
        proposedContent.append(QStringLiteral("line-%1\n").arg(line).toUtf8());
    }
    const QByteArray baseContent("original\r\ncontent\r\n");
    const QString proposedHash = sha256(proposedContent);
    const QString proposedReference = QStringLiteral("workspace-edit-content:sha256:%1")
        .arg(proposedHash);
    const QJsonObject edit{
        {QStringLiteral("schema_version"), QStringLiteral("workspace-edit/0.2")},
        {QStringLiteral("edit_id"), QStringLiteral("render-edit-preview")},
        {QStringLiteral("project_id"), openedProjectId},
        {QStringLiteral("root"), QJsonObject{
            {QStringLiteral("canonical_path"), openedProjectRoot},
            {QStringLiteral("identity"),
             QStringLiteral("workspace-root:sha256:%1")
                 .arg(sha256(openedProjectRoot.toUtf8()))},
        }},
        {QStringLiteral("operations"), QJsonArray{QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("update")},
            {QStringLiteral("path"), QStringLiteral("editable.txt")},
            {QStringLiteral("base"), QJsonObject{
                {QStringLiteral("sha256"), sha256(baseContent)},
                {QStringLiteral("bytes"), baseContent.size()},
            }},
            {QStringLiteral("content"), QJsonObject{
                {QStringLiteral("reference"), proposedReference},
                {QStringLiteral("hash"), QJsonObject{
                    {QStringLiteral("sha256"), proposedHash},
                    {QStringLiteral("bytes"), proposedContent.size()},
                }},
                {QStringLiteral("format"), QJsonObject{
                    {QStringLiteral("encoding"), QStringLiteral("utf-8")},
                    {QStringLiteral("newline"), QStringLiteral("lf")},
                    {QStringLiteral("mode"), QStringLiteral("preserve")},
                }},
            }},
        }}},
    };
    runtime->previewWorkspaceEdit(previewSessionId, edit, QJsonArray{QJsonObject{
        {QStringLiteral("reference"), proposedReference},
        {QStringLiteral("content"), QString::fromUtf8(proposedContent)},
    }});
    if (!expect(waitUntil(application, [workspaceEditSummary, workspaceEditFiles,
                                        workspaceEditDiff, workspaceEditMore]() {
                    return workspaceEditSummary->text().contains(QStringLiteral("基线检查通过"))
                        && workspaceEditFiles->topLevelItemCount() == 2
                        && workspaceEditDiff->toPlainText().contains(
                            QStringLiteral("--- a/editable.txt"))
                        && workspaceEditMore->isEnabled();
                }, 5000),
                "workspace edit preview did not render bounded aggregate diff metadata")) {
        return 1;
    }
    const int initialDiffSize = workspaceEditDiff->toPlainText().toUtf8().size();
    workspaceEditMore->click();
    if (!expect(waitUntil(application, [workspaceEditDiff, initialDiffSize]() {
                    return workspaceEditDiff->toPlainText().toUtf8().size() > initialDiffSize;
                }),
                "workspace edit preview did not page the remaining diff content")) {
        return 1;
    }
    QFile unchanged(editablePath);
    if (!expect(unchanged.open(QIODevice::ReadOnly)
                    && unchanged.readAll() == baseContent,
                "workspace edit preview modified the project before approval/apply")) {
        return 1;
    }
    const QByteArray sensitiveContent("PLACEHOLDER=review\n");
    const QString sensitiveHash = sha256(sensitiveContent);
    const QString sensitiveReference =
        QStringLiteral("workspace-edit-content:sha256:%1").arg(sensitiveHash);
    const QJsonObject sensitiveEdit{
        {QStringLiteral("schema_version"), QStringLiteral("workspace-edit/0.2")},
        {QStringLiteral("edit_id"), QStringLiteral("render-sensitive-preview")},
        {QStringLiteral("project_id"), openedProjectId},
        {QStringLiteral("root"), QJsonObject{
            {QStringLiteral("canonical_path"), openedProjectRoot},
            {QStringLiteral("identity"),
             QStringLiteral("workspace-root:sha256:%1")
                 .arg(sha256(openedProjectRoot.toUtf8()))},
        }},
        {QStringLiteral("operations"), QJsonArray{QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("create")},
            {QStringLiteral("path"), QStringLiteral(".env")},
            {QStringLiteral("content"), QJsonObject{
                {QStringLiteral("reference"), sensitiveReference},
                {QStringLiteral("hash"), QJsonObject{
                    {QStringLiteral("sha256"), sensitiveHash},
                    {QStringLiteral("bytes"), sensitiveContent.size()},
                }},
                {QStringLiteral("format"), QJsonObject{
                    {QStringLiteral("encoding"), QStringLiteral("utf-8")},
                    {QStringLiteral("newline"), QStringLiteral("lf")},
                    {QStringLiteral("mode"), QStringLiteral("regular")},
                }},
            }},
        }}},
    };
    runtime->previewWorkspaceEdit(previewSessionId, sensitiveEdit,
                                  QJsonArray{QJsonObject{
        {QStringLiteral("reference"), sensitiveReference},
        {QStringLiteral("content"), QString::fromUtf8(sensitiveContent)},
    }});
    if (!expect(waitUntil(application, [workspaceEditSummary, workspaceEditFiles]() {
                    return workspaceEditSummary->text().contains(QStringLiteral("阻塞警告"))
                        && workspaceEditFiles->topLevelItemCount() == 2
                        && workspaceEditFiles->topLevelItem(1)->text(3).contains(
                            QStringLiteral("sensitive-path"));
                }),
                "sensitive workspace edit warning was not visible in review UI")) {
        return 1;
    }
    if (!expect(!QFile::exists(project.filePath(QStringLiteral(".env"))),
                "sensitive workspace edit preview created a file")) {
        return 1;
    }
    runtime->renameSession(previewSessionId, QStringLiteral("Review session"));
    if (!expect(waitUntil(application, [sessionList]() {
                    return sessionList->count() > 0
                        && sessionList->item(0)->text().contains(
                            QStringLiteral("Review session"));
                }),
                "session rename did not refresh the Workbench list")) {
        return 1;
    }
    runtime->archiveSession(previewSessionId);
    if (!expect(waitUntil(application, [sessionList]() {
                    return sessionList->count() > 0
                        && sessionList->item(0)->text().contains(QStringLiteral("已归档"));
                }),
                "session archive did not remain visible for recovery")) {
        return 1;
    }
    runtime->unarchiveSession(previewSessionId);
    if (!expect(waitUntil(application, [sessionList]() {
                    return sessionList->count() > 0
                        && !sessionList->item(0)->text().contains(QStringLiteral("已归档"));
                }),
                "session restore did not reactivate the Workbench row")) {
        return 1;
    }
    QString deletionPreviewRequest;
    QJsonObject deletionPreview;
    QObject::connect(runtime, &AgentRuntimeClient::sessionDeletionPreviewed,
                     &workbench, [&](const QString &requestId, const QJsonObject &preview) {
        if (requestId == deletionPreviewRequest) deletionPreview = preview;
    });
    deletionPreviewRequest = runtime->previewSessionDeletion(
        previewSessionId, QStringLiteral("session-only"));
    if (!expect(waitUntil(application, [&deletionPreview]() {
                    return !deletionPreview.isEmpty();
                }),
                "session deletion preview did not cross the Qt AAP bridge")) {
        return 1;
    }
    if (!expect(deletionPreview.value(QStringLiteral("blocking_reasons"))
                    .toArray().isEmpty(),
                "idle session deletion preview was unexpectedly blocked")) {
        return 1;
    }
    QString deletionChangeRequest;
    QJsonObject deletionReceipt;
    QObject::connect(runtime, &AgentRuntimeClient::sessionDeletionChanged,
                     &workbench, [&](const QString &requestId, const QString &,
                                     const QJsonObject &receipt) {
        if (requestId == deletionChangeRequest) deletionReceipt = receipt;
    });
    deletionChangeRequest = runtime->scheduleSessionDeletion(
        previewSessionId, QStringLiteral("session-only"),
        deletionPreview.value(QStringLiteral("plan_hash")).toObject(),
        7LL * 24 * 60 * 60 * 1000);
    const bool pendingDeletionVisible = waitUntil(
        application, [sessionList, &deletionReceipt]() {
                    return deletionReceipt.value(QStringLiteral("state")).toString()
                               == QStringLiteral("pending")
                        && sessionList->count() > 0
                        && sessionList->item(0)->text().contains(QStringLiteral("待删除"));
                });
    if (!pendingDeletionVisible) {
        qCritical() << "deletion receipt" << deletionReceipt
                    << "send" << send->text() << send->isEnabled();
        for (int row = 0; row < sessionList->count(); ++row) {
            qCritical() << "session row" << row << sessionList->item(row)->text();
        }
    }
    if (!expect(pendingDeletionVisible,
                "pending deletion did not freeze and label the current Qt session")) {
        return 1;
    }
    sessionList->setCurrentRow(0);
    sessionList->itemClicked(sessionList->item(0));
    if (!expect(waitUntil(application, [send]() {
                    return send->text() == QStringLiteral("待删除") && !send->isEnabled();
                }),
                "selecting a pending-deletion session did not freeze Qt actions")) {
        return 1;
    }
    deletionChangeRequest = runtime->undoSessionDeletion(
        deletionReceipt.value(QStringLiteral("deletion_id")).toString());
    if (!expect(waitUntil(application, [sessionList, send, &deletionReceipt]() {
                    return deletionReceipt.value(QStringLiteral("state")).toString()
                               == QStringLiteral("cancelled")
                        && sessionList->count() > 0
                        && !sessionList->item(0)->text().contains(QStringLiteral("待删除"))
                        && send->text() == QStringLiteral("发送")
                        && send->isEnabled();
                }),
                "session deletion undo did not restore the Qt workflow")) {
        return 1;
    }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    terminalNewForeground->trigger();
    if (!expect(waitUntil(application, [terminalPicker, sessionList]() {
                    return terminalPicker->count() == 1 && sessionList->count() > 0
                        && sessionList->item(0)->text().startsWith(
                            QStringLiteral("项目任务 · "));
                }, 5000),
                "terminal UI did not create a foreground terminal and Work session")) {
        return 1;
    }
    const QString sessionRow = sessionList->item(0)->text();
    const QString sessionId = sessionRow.mid(
        sessionRow.lastIndexOf(QStringLiteral(" · ")) + 3);
    const QString terminalId = terminalPicker->currentData().toString();
#ifdef Q_OS_WIN
    const QByteArray terminalCommand("echo Aegisy terminal UI\r\nexit 0\r\n");
#else
    const QByteArray terminalCommand("printf 'Aegisy terminal UI\\n'; exit 0\n");
#endif
    runtime->inputUserTerminal(sessionId, terminalId, terminalCommand);
    if (!expect(waitUntil(application, [terminalExcerpt, terminalRemove, terminalStatus]() {
                    return terminalExcerpt->toPlainText().contains(
                               QStringLiteral("Aegisy terminal UI"))
                        && terminalRemove->isEnabled()
                        && terminalStatus->text().contains(QStringLiteral("exited"));
                }, 5000),
                "terminal UI did not stream PTY output and observe process exit")) {
        return 1;
    }
    terminalRemove->click();
    if (!expect(waitUntil(application, [terminalPicker]() {
                    return terminalPicker->count() == 0;
                }),
                "terminal UI did not remove the exited terminal")) {
        return 1;
    }
#endif
    int structureTab = -1;
    for (int index = 0; index < tabs->count(); ++index) {
        if (tabs->tabText(index) == QStringLiteral("结构")) structureTab = index;
    }
    tabs->setCurrentIndex(structureTab);
    if (!expect(structureTab >= 0
                    && waitUntil(application, [repositorySymbols, repositoryMap]() {
                        return repositorySymbols->topLevelItemCount() >= 1
                            && repositoryMap->toPlainText().contains(
                                QStringLiteral("function run"));
                    }, 5000),
                "repository structure view did not index symbols and build a map")) {
        return 1;
    }
    if (!expect(repositoryStatus->text().contains(QStringLiteral("个符号"))
                    && repositorySymbols->findItems(
                        QStringLiteral("run"), Qt::MatchExactly | Qt::MatchRecursive).size() == 1,
                "repository structure view lost index status or symbol navigation")) {
        return 1;
    }
    QTreeWidgetItem *editableItem = fileTree->findItems(
        QStringLiteral("editable.txt"), Qt::MatchExactly | Qt::MatchRecursive).first();
    QTreeWidgetItem *binaryItem = fileTree->findItems(
        QStringLiteral("binary.dat"), Qt::MatchExactly | Qt::MatchRecursive).first();
    QTreeWidgetItem *secondItem = fileTree->findItems(
        QStringLiteral("second.txt"), Qt::MatchExactly | Qt::MatchRecursive).first();
    QTreeWidgetItem *sourceFileItem = fileTree->findItems(
        QStringLiteral("source.cpp"), Qt::MatchExactly | Qt::MatchRecursive).first();
    workspaceSearch->setText(QStringLiteral("original"));
    workspaceSearchButton->click();
    if (!expect(waitUntil(application, [workspaceSearchResults]() {
                    return workspaceSearchResults->topLevelItemCount() >= 1;
                }),
                "workspace text search did not return the fixture match")) {
        return 1;
    }
    QTreeWidgetItem *searchMatch = workspaceSearchResults->topLevelItem(0);
    if (!expect(searchMatch->data(0, Qt::UserRole).toString()
                    == QStringLiteral("editable.txt")
                    && searchMatch->text(1).startsWith(QStringLiteral("1:")),
                "workspace search result lost its path or range")) {
        return 1;
    }
    QMetaObject::invokeMethod(workspaceSearchResults, "itemActivated", Qt::DirectConnection,
                              Q_ARG(QTreeWidgetItem *, searchMatch), Q_ARG(int, 0));
    if (!expect(waitUntil(application, [editor, editorPath]() {
                    return editorPath->text().startsWith(QStringLiteral("editable.txt"))
                        && editor->toPlainText() == QStringLiteral("original\ncontent\n");
                }),
                "workspace search result did not open the matched file")) {
        return 1;
    }
    workspaceSearch->setText(QStringLiteral("pagehit"));
    workspaceSearchButton->click();
    if (!expect(waitUntil(application, [workspaceSearchResults, workspaceSearchMore]() {
                    return workspaceSearchResults->topLevelItemCount() == 50
                        && workspaceSearchMore->isEnabled();
                }),
                "workspace search did not expose the bounded first page")) {
        return 1;
    }
    workspaceSearchMore->click();
    if (!expect(waitUntil(application, [workspaceSearchResults, workspaceSearchMore]() {
                    return workspaceSearchResults->topLevelItemCount() == 60
                        && !workspaceSearchMore->isEnabled();
                }),
                "workspace search did not append the final page")) {
        return 1;
    }
    QMetaObject::invokeMethod(fileTree, "itemActivated", Qt::DirectConnection,
                              Q_ARG(QTreeWidgetItem *, binaryItem), Q_ARG(int, 0));
    if (!expect(waitUntil(application, [editor, editorPath, editorMeta]() {
                    return editor->isReadOnly()
                        && editorPath->text().startsWith(QStringLiteral("binary.dat"))
                        && editorMeta->text().contains(QStringLiteral("只读降级"));
                }),
                "binary file did not enter read-only fallback")) {
        return 1;
    }
    QMetaObject::invokeMethod(fileTree, "itemActivated", Qt::DirectConnection,
                              Q_ARG(QTreeWidgetItem *, editableItem), Q_ARG(int, 0));
    if (!expect(waitUntil(application, [editor]() {
                    return !editor->isReadOnly()
                        && editor->toPlainText() == QStringLiteral("original\ncontent\n");
                }),
                "editable text file did not open through AAP")) {
        return 1;
    }
    fileTree->setCurrentItem(editableItem);
    fileContextAction->trigger();
    QTextCursor contextSelection = editor->textCursor();
    contextSelection.setPosition(0);
    contextSelection.setPosition(8, QTextCursor::KeepAnchor);
    editor->setTextCursor(contextSelection);
    if (!expect(waitUntil(application, [editorContext]() {
                    return editorContext->isEnabled();
                }),
                "editor selection did not enable its context action")) {
        return 1;
    }
    editorContext->click();
    workspaceSearchResults->setCurrentItem(workspaceSearchResults->topLevelItem(0));
    searchContextAction->trigger();
    terminalExcerpt->setPlainText(QStringLiteral("test command output\nsecond line"));
    terminalExcerpt->selectAll();
    terminalContextAction->trigger();
    gitDiff->setPlainText(QStringLiteral("@@ -1 +1 @@\n-old\n+new"));
    gitDiff->selectAll();
    gitContextAction->trigger();
    if (!expect(waitUntil(application, [contextPanel, contextList, contextSummary]() {
                    return contextPanel->isVisible() && contextList->count() == 5
                        && contextSummary->text().contains(QStringLiteral("发送 5 项"));
                }),
                "context actions did not populate the bounded composer queue")) {
        return 1;
    }
    const QList<QCheckBox *> inclusionChecks = workbench.findChildren<QCheckBox *>(
        QStringLiteral("agentContextIncludeCheck"));
    QList<QCheckBox *> visibleInclusionChecks;
    std::copy_if(inclusionChecks.cbegin(), inclusionChecks.cend(),
                 std::back_inserter(visibleInclusionChecks),
                 [](QCheckBox *check) { return check->isVisible(); });
    if (!expect(visibleInclusionChecks.size() == 5,
                "context queue did not expose inclusion controls")) {
        return 1;
    }
    visibleInclusionChecks.first()->setChecked(false);
    if (!expect(waitUntil(application, [contextSummary]() {
                    return contextSummary->text().contains(QStringLiteral("发送 4 项"));
                }),
                "context inclusion toggle did not update submission state")) {
        return 1;
    }
    const QList<QCheckBox *> refreshedChecks = workbench.findChildren<QCheckBox *>(
        QStringLiteral("agentContextIncludeCheck"));
    auto refreshed = std::find_if(refreshedChecks.cbegin(), refreshedChecks.cend(),
                                  [](QCheckBox *check) { return check->isVisible(); });
    if (!expect(refreshed != refreshedChecks.cend(),
                "context queue lost its visible inclusion control")) {
        return 1;
    }
    (*refreshed)->setChecked(true);
    if (!expect(waitUntil(application, [contextSummary]() {
                    return contextSummary->text().contains(QStringLiteral("发送 5 项"));
                }),
                "context inclusion toggle did not restore submission state")) {
        return 1;
    }
    work->click();
    composer->setPlainText(QStringLiteral("检查选定上下文"));
    sendButton->click();
    if (!expect(waitUntil(application, [&workbench, contextPanel, contextList]() {
                    const QList<QLabel *> labels = workbench.findChildren<QLabel *>();
                    return contextPanel->isHidden() && contextList->count() == 0
                        && std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                            return label->text().contains(QStringLiteral("untrusted data"))
                                && label->text().contains(QStringLiteral("original"));
                        });
                }),
                "structured context was not sent through AAP with authoritative file data")) {
        return 1;
    }
    editor->selectAll();
    editor->insertPlainText(QStringLiteral("saved\ncontent\n"));
    QMetaObject::invokeMethod(fileTree, "itemActivated", Qt::DirectConnection,
                              Q_ARG(QTreeWidgetItem *, secondItem), Q_ARG(int, 0));
    if (!expect(waitUntil(application, [editor, editorTabs, recentFiles]() {
                    return editor->toPlainText() == QStringLiteral("second\nfile\n")
                        && editorTabs->count() == 3 && recentFiles->count() >= 4;
                }),
                "opening a second text file did not create a recent editor tab")) {
        return 1;
    }
    int editableTab = -1;
    for (int index = 0; index < editorTabs->count(); ++index) {
        if (editorTabs->tabData(index).toString() == QStringLiteral("editable.txt")) {
            editableTab = index;
            break;
        }
    }
    editorTabs->setCurrentIndex(editableTab);
    if (!expect(editableTab >= 0
                    && waitUntil(application, [editor, editorTabs, editableTab]() {
                        return editor->toPlainText() == QStringLiteral("saved\ncontent\n")
                            && editor->document()->isModified()
                            && editorTabs->tabText(editableTab).endsWith(QLatin1Char('*'));
                    }),
                "switching editor tabs lost the unsaved document buffer")) {
        return 1;
    }
    editorFind->setText(QStringLiteral("content"));
    editorReplace->setText(QStringLiteral("body"));
    replaceAll->click();
    if (!expect(editor->toPlainText() == QStringLiteral("saved\nbody\n"),
                "replace-all did not update the active editable buffer")) {
        return 1;
    }
    if (!expect(editorSave->isEnabled(), "dirty editor did not enable save")) return 1;
    editorSave->click();
    if (!expect(waitUntil(application, [&editablePath]() {
                    QFile saved(editablePath);
                    return saved.open(QIODevice::ReadOnly)
                        && saved.readAll() == QByteArray("saved\r\nbody\r\n");
                }),
                "editor save did not preserve CRLF on disk")) {
        return 1;
    }
    if (!expect(waitUntil(application, [editor, editorSave]() {
                    return !editor->document()->isModified() && !editorSave->isEnabled();
                }),
                "successful save did not clear editor dirty state")) {
        return 1;
    }
    if (!expect(workspaceSearchStatus->text().contains(QStringLiteral("过期")),
                "workspace mutation did not mark search results stale")) {
        return 1;
    }
    QMetaObject::invokeMethod(fileTree, "itemActivated", Qt::DirectConnection,
                              Q_ARG(QTreeWidgetItem *, sourceFileItem), Q_ARG(int, 0));
    if (!expect(waitUntil(application, [editor, languageDefinition, &sourceContent]() {
                    return editor->toPlainText() == sourceContent
                        && languageDefinition->isEnabled();
                }),
                "C++ fixture did not enable language-server actions")) {
        return 1;
    }
    const bool clangdAvailable = languageStatus->text().contains(QStringLiteral("clangd"));
    QTextCursor definitionCursor = editor->textCursor();
    definitionCursor.setPosition(sourceContent.indexOf(QStringLiteral("add(1")) + 1);
    editor->setTextCursor(definitionCursor);
    languageDefinition->click();
    if (clangdAvailable) {
        if (!expect(waitUntil(application, [editor, languageResults, languageStatus]() {
                        return editor->textCursor().blockNumber() == 0
                            && languageResults->topLevelItemCount() >= 1
                            && languageStatus->text().contains(QStringLiteral("clangd"));
                    }, 5000),
                    "clangd definition did not navigate to the declaration")) {
            return 1;
        }
        editor->selectAll();
        editor->insertPlainText(QStringLiteral("int run() { return missing_name; }\n"));
        languageDiagnosticsButton->click();
        if (!expect(waitUntil(application, [languageDiagnostics, languageStatus]() {
                        return languageDiagnostics->topLevelItemCount() >= 1
                            && languageStatus->text().contains(QStringLiteral("诊断"));
                    }, 5000),
                    "clangd diagnostics did not render the unsaved document error")) {
            return 1;
        }
        QTreeWidgetItem *diagnosticItem = languageDiagnostics->topLevelItem(0);
        if (!expect(diagnosticItem->text(3) == QStringLiteral("新鲜")
                        && diagnosticItem->text(4).contains(QStringLiteral("missing_name"))
                        && diagnosticItem->toolTip(4).contains(QStringLiteral("文件 SHA-256"))
                        && languageRaw->isEnabled(),
                    "observed diagnostic provenance or freshness did not render")) {
            return 1;
        }
        languageRaw->click();
        if (!expect(waitUntil(application, [diagnosticRawPreview]() {
                        return diagnosticRawPreview->toPlainText().contains(
                            QStringLiteral("missing_name"));
                    }),
                    "diagnostic raw authority did not render")) {
            return 1;
        }
        languageDiagnostics->setCurrentItem(diagnosticItem);
        diagnosticContextAction->trigger();
        if (!expect(waitUntil(application, [contextList, contextSummary]() {
                        return contextList->count() == 1
                            && contextSummary->text().contains(QStringLiteral("发送 1 项"));
                    }),
                    "diagnostic context action did not populate the composer queue")) {
            return 1;
        }
        const QList<QPushButton *> removeButtons = workbench.findChildren<QPushButton *>(
            QStringLiteral("agentContextRemoveButton"));
        auto visibleRemove = std::find_if(removeButtons.cbegin(), removeButtons.cend(),
                                          [](QPushButton *button) {
            return button->isVisible();
        });
        QPushButton *removeContext = visibleRemove == removeButtons.cend()
            ? nullptr : *visibleRemove;
        if (!expect(removeContext, "context row did not expose a removal control")) return 1;
        removeContext->click();
        if (!expect(waitUntil(application, [contextList]() {
                        return contextList->count() == 0;
                    }),
                    "context removal control did not clear the diagnostic")) {
            return 1;
        }
        QMetaObject::invokeMethod(languageDiagnostics, "itemActivated", Qt::DirectConnection,
                                  Q_ARG(QTreeWidgetItem *, diagnosticItem), Q_ARG(int, 0));
        if (!expect(waitUntil(application, [editorPath]() {
                        return editorPath->text().startsWith(QStringLiteral("source.cpp"));
                    }),
                    "language diagnostic result did not return to the editor")) {
            return 1;
        }
        editorSave->click();
        if (!expect(waitUntil(application, [diagnosticItem, languageStatus]() {
                        return diagnosticItem->text(3) == QStringLiteral("已过期")
                            && languageStatus->text().contains(QStringLiteral("已过期"));
                    }),
                    "workspace save did not mark observed diagnostics stale")) {
            return 1;
        }
        languageStop->click();
        if (!expect(waitUntil(application, [languageStatus]() {
                        return languageStatus->text().contains(QStringLiteral("已停止"));
                    }),
                    "language-server stop did not update lifecycle state")) {
            return 1;
        }
    } else if (!expect(waitUntil(application, [languageStatus]() {
                           return languageStatus->text().contains(QStringLiteral("not installed"));
                       }),
                       "missing clangd did not expose an actionable unavailable state")) {
        return 1;
    }
    QMetaObject::invokeMethod(fileTree, "itemActivated", Qt::DirectConnection,
                              Q_ARG(QTreeWidgetItem *, editableItem), Q_ARG(int, 0));
    if (!expect(waitUntil(application, [editorPath, editor]() {
                    return editorPath->text().startsWith(QStringLiteral("editable.txt"))
                        && !editor->isReadOnly()
                        && editor->toPlainText() == QStringLiteral("saved\nbody\n");
                }),
                "editor did not restore the conflict fixture after language analysis")) {
        return 1;
    }
    editor->selectAll();
    editor->insertPlainText(QStringLiteral("local\ncontent\n"));
    if (!expect(waitUntil(application, [editorSave]() {
                    return editorSave->isEnabled();
                }),
                "local edit did not enable conflict save attempt")) {
        return 1;
    }
    QFile external(editablePath);
    if (!expect(external.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "could not create external conflict fixture")) {
        return 1;
    }
    external.write("external\r\ncontent\r\n");
    external.close();
    editorSave->click();
    if (!expect(waitUntil(application, [editorPath]() {
                    return editorPath->text().contains(QStringLiteral("冲突"));
                }),
                "stale save did not surface an editor conflict")) {
        return 1;
    }
    external.setFileName(editablePath);
    if (!expect(external.open(QIODevice::ReadOnly)
                    && external.readAll() == QByteArray("external\r\ncontent\r\n"),
                "stale save overwrote external file content")) {
        return 1;
    }
#endif

    workbench.resize(820, 620);
    application.processEvents();
    if (!expect(splitter->widget(0)->width() >= splitter->widget(0)->minimumWidth(),
                "product rail clipped below its minimum")
            || !expect(splitter->widget(1)->width() >= splitter->widget(1)->minimumWidth(),
                       "agent surface clipped below its minimum")
            || !expect(splitter->widget(2)->width() >= splitter->widget(2)->minimumWidth(),
                       "work canvas clipped below its minimum")) {
        return 1;
    }

    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--snapshot")) {
#ifdef AEGISY_EXPECT_AGENTD
        const QList<QTreeWidgetItem *> snapshotFiles = fileTree->findItems(
            QStringLiteral("editable.txt"), Qt::MatchExactly | Qt::MatchRecursive);
        if (!snapshotFiles.isEmpty()) {
            fileTree->setCurrentItem(snapshotFiles.first());
            fileContextAction->trigger();
        }
        QTextCursor snapshotSelection = editor->textCursor();
        snapshotSelection.setPosition(0);
        snapshotSelection.setPosition(qMin(5, editor->toPlainText().size()),
                                      QTextCursor::KeepAnchor);
        editor->setTextCursor(snapshotSelection);
        editorContext->click();
#endif
        workbench.resize(1100, 700);
        int snapshotTab = 1;
        for (int index = 0; index < tabs->count(); ++index) {
            if (tabs->tabText(index) == QStringLiteral("结构")) snapshotTab = index;
        }
        tabs->setCurrentIndex(snapshotTab);
        application.processEvents();
        const QImage finalImage = workbench.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        if (!finalImage.save(QString::fromLocal8Bit(argv[2]))) {
            qCritical() << "failed to save Agent Workbench snapshot";
            return 1;
        }
    }
    return 0;
}
