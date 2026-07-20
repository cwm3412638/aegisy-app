#include "agent_workbench_widget.h"
#include "agent_runtime_client.h"
#include "app_theme.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDebug>
#include <QDialog>
#include <QImage>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStandardPaths>
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

bool runGit(const QString &executable, const QString &root, const QStringList &arguments,
            QString *standardOutput = nullptr)
{
    QProcess process;
    process.setWorkingDirectory(root);
    process.start(executable, arguments);
    if (!process.waitForStarted(3000) || !process.waitForFinished(10000)
            || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qCritical() << "git fixture failed" << arguments << process.readAllStandardError();
        return false;
    }
    if (standardOutput) {
        *standardOutput = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }
    return true;
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
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
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
    QLabel *runtimeCapability = workbench.findChild<QLabel *>(
        QStringLiteral("agentRuntimeCapabilityStatus"));
    QLabel *executionContext = workbench.findChild<QLabel *>(
        QStringLiteral("agentExecutionContextStrip"));

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
    QTextEdit *operationComposer = workbench.findChild<QTextEdit *>(
        QStringLiteral("agentComposer"));
    QPushButton *attachContext = workbench.findChild<QPushButton *>(
        QStringLiteral("agentAttachContextButton"));
    QPushButton *contextInspect = workbench.findChild<QPushButton *>(
        QStringLiteral("agentContextInspectButton"));
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
    QLineEdit *sessionSearch = workbench.findChild<QLineEdit *>(
        QStringLiteral("agentSessionSearchInput"));
    QPushButton *retentionSettings = workbench.findChild<QPushButton *>(
        QStringLiteral("agentRetentionSettingsButton"));
    QPushButton *importSession = workbench.findChild<QPushButton *>(
        QStringLiteral("agentImportSessionButton"));
    QPushButton *sessionHistoryMore = workbench.findChild<QPushButton *>(
        QStringLiteral("agentSessionHistoryMoreButton"));
    QLabel *recoveryBanner = workbench.findChild<QLabel *>(
        QStringLiteral("agentRecoveryBanner"));
    QLabel *operationStatusBanner = workbench.findChild<QLabel *>(
        QStringLiteral("agentOperationStatusBanner"));
    QPushButton *operationStatusReview = workbench.findChild<QPushButton *>(
        QStringLiteral("agentOperationStatusReviewButton"));
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
    QPushButton *gitPinDiff = workbench.findChild<QPushButton *>(
        QStringLiteral("agentGitPinDiffButton"));
    QPushButton *gitPinCommit = workbench.findChild<QPushButton *>(
        QStringLiteral("agentGitPinCommitButton"));
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
    QAction *pinFileContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentPinFileContextAction"));
    QAction *pinSelectionContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentPinSelectionContextAction"));
    QAction *pinImageContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentPinImageContextAction"));
    QAction *searchContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentSearchResultContextAction"));
    QAction *diagnosticContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentDiagnosticContextAction"));
    QAction *terminalContextAction = workbench.findChild<QAction *>(
        QStringLiteral("agentTerminalExcerptContextAction"));
    QAction *pinTerminalExcerptAction = workbench.findChild<QAction *>(
        QStringLiteral("agentPinTerminalExcerptAction"));
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
            || !expect(executionContext
                           && executionContext->text().contains(QStringLiteral("权限 只读"))
                           && executionContext->text().contains(QStringLiteral("上下文 0")),
                       "execution-context strip did not expose the read-only empty state")
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
            || !expect(attachContext && contextInspect && contextPanel && contextSummary && contextList
                           && editorContext && terminalExcerpt && gitDiff
                           && fileContextAction && searchContextAction
                           && pinFileContextAction
                           && pinSelectionContextAction
                           && pinImageContextAction
                           && diagnosticContextAction && terminalContextAction
                           && pinTerminalExcerptAction
                           && gitContextAction && contextList->count() == 0
                           && contextPanel->isHidden() && !contextInspect->isEnabled()
                           && !editorContext->isEnabled(),
                       "structured turn-context controls are missing")
            || !expect(workspaceEditSummary && workspaceEditFiles && workspaceEditDiff
                           && workspaceEditMore && workspaceEditDiff->isReadOnly()
                           && !workspaceEditMore->isEnabled(),
                       "workspace edit preview controls are missing")
            || !expect(gitSummary && gitHistory && gitDiffScope && gitRefresh && gitPinDiff
                           && gitPinCommit && gitDiff
                           && gitHistory->columnCount() == 4
                           && gitDiffScope->count() == 3 && gitDiff->isReadOnly()
                           && !gitRefresh->icon().isNull()
                           && !gitPinDiff->icon().isNull() && !gitPinCommit->icon().isNull()
                           && !gitPinDiff->isEnabled() && !gitPinCommit->isEnabled(),
                       "read-only Git query workspace controls are missing")
            || !expect(terminalPicker && terminalStatus && terminalNew && terminalStop
                           && terminalRestart && terminalRemove && terminalContext
                           && terminalNewForeground && sessionList && sessionSearch
                           && sessionHistoryMore
                           && retentionSettings && importSession
                           && sessionSearch->placeholderText() == QStringLiteral("搜索会话")
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
                           && operationStatusBanner && operationStatusBanner->isHidden()
                           && operationStatusReview && !operationStatusReview->isEnabled()
                           && runtimeRestart && !runtimeRestart->isEnabled(),
                       "recovery banners must exist and start hidden for a healthy runtime")
            || !expect(runtimeCapability && !runtimeCapability->text().isEmpty(),
                       "runtime capability status must exist before negotiation")
            || !expect(newSession && openFolder && sendButton
                           && !newSession->icon().isNull()
                           && !openFolder->icon().isNull()
                           && !sendButton->icon().isNull()
                           && !contextInspect->icon().isNull()
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
    if (!expect(runtimeCapability->text() == QStringLiteral("能力未知")
                    && runtimeCapability->toolTip().contains(QStringLiteral("只读门控")),
                "runtime capability status did not fail closed in recovery mode")) {
        return 1;
    }
    runtimeClient->runtimeInitialized(QJsonObject{
        {QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("status"), QStringLiteral("ready")},
        }},
        {QStringLiteral("capabilities"), QJsonArray{
            QStringLiteral("session.compaction.checkpoint-review"),
            QStringLiteral("turn.context.pinned-selected"),
            QStringLiteral("workspace.git-context.read-only"),
            QStringLiteral("workspace.image.import-user"),
            QStringLiteral("workspace.image.preview"),
        }},
    });
    runtimeClient->runtimeDegradationsRead(QStringLiteral("degradation-fixture"), QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("runtime-degradations/0.1")},
        {QStringLiteral("degradations"), QJsonArray{
            QJsonObject{{QStringLiteral("feature"), QStringLiteral("agent-mutation")},
                         {QStringLiteral("state"), QStringLiteral("disabled")}},
            QJsonObject{{QStringLiteral("feature"), QStringLiteral("provider-thread-compact")},
                         {QStringLiteral("state"), QStringLiteral("blocked")}},
            QJsonObject{{QStringLiteral("feature"), QStringLiteral("provider-thread-delete")},
                         {QStringLiteral("state"), QStringLiteral("blocked")}},
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
    if (!expect(runtimeCapability->text().contains(QStringLiteral("Agent 只读"))
                    && runtimeCapability->text().contains(QStringLiteral("Compact 不可用"))
                    && runtimeCapability->text().contains(QStringLiteral("删除不可用"))
                    && runtimeCapability->toolTip().contains(QStringLiteral("不会显示为成功")),
                "runtime degradations were not projected into a fail-closed capability state")) {
        return 1;
    }
    runtimeClient->runtimeDegradationsRead(QStringLiteral("degradation-invalid"), QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("runtime-degradations/unknown")},
    });
    application.processEvents();
    if (!expect(runtimeCapability->text() == QStringLiteral("能力未知")
                    && runtimeCapability->toolTip().contains(QStringLiteral("只读门控")),
                "invalid runtime degradation schema did not fail closed")) {
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
    runtimeClient->operationStatusRead({}, QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("operation-reconciliation-status/0.1")},
        {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("blocked"), true},
        {QStringLiteral("operation"), QJsonObject{
            {QStringLiteral("operation_id"), QStringLiteral("turn-operation-fixture")},
            {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
            {QStringLiteral("kind"), QStringLiteral("turn")},
            {QStringLiteral("state"), QStringLiteral("unknown")},
            {QStringLiteral("decision"), QStringLiteral("explicit-review-required")},
            {QStringLiteral("blockers"), QJsonArray{QStringLiteral("no-authoritative-terminal-event")}},
            {QStringLiteral("review_id"), QStringLiteral("reconciliation-review:sha256:aaaaaaaa")},
        }},
        {QStringLiteral("recovery_action_available"), false},
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
    runtimeClient->operationStatusRead({}, QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("operation-reconciliation-status/0.1")},
        {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("blocked"), false},
        {QStringLiteral("operation"), QJsonValue::Null},
        {QStringLiteral("recovery_action_available"), false},
    });
    runtimeClient->projectionRecoveryStatusRead(QJsonObject{
        {QStringLiteral("startup"), QJsonObject{
            {QStringLiteral("rebuilt_sessions"), 0},
        }},
        {QStringLiteral("current_quarantined_sessions"), 0},
    });
    application.processEvents();
    if (!expect(recoveryBanner->isHidden() && operationStatusBanner->isHidden()
                    && sendButton->isEnabled(),
                "cleared recovery state did not restore the healthy composer")) {
        return 1;
    }
    runtimeClient->operationStatusRead({}, QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("operation-reconciliation-status/0.1")},
        {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("blocked"), true},
        {QStringLiteral("operation"), QJsonObject{
            {QStringLiteral("operation_id"), QStringLiteral("turn-operation-fixture")},
            {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
            {QStringLiteral("kind"), QStringLiteral("turn")},
            {QStringLiteral("state"), QStringLiteral("unknown")},
            {QStringLiteral("decision"), QStringLiteral("explicit-review-required")},
        }},
        {QStringLiteral("recovery_action_available"), false},
    });
    application.processEvents();
    if (!expect(operationStatusBanner && !operationStatusBanner->isHidden()
                    && sendButton->text() == QStringLiteral("操作暂停")
                    && operationStatusReview && operationStatusReview->isEnabled()
                    && !sendButton->isEnabled() && operationComposer
                    && operationComposer->isReadOnly(),
                "blocked operation status did not enforce a read-only composer")) {
        return 1;
    }
    operationStatusReview->click();
    runtimeClient->operationProbeRead({}, QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("operation-reconciliation-probe/0.1")},
        {QStringLiteral("operation_id"), QStringLiteral("turn-operation-fixture")},
        {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("kind"), QStringLiteral("turn")},
        {QStringLiteral("evidence"), QJsonObject{
            {QStringLiteral("event"), QStringLiteral("completed")},
            {QStringLiteral("process"), QStringLiteral("not-running")},
            {QStringLiteral("workspace"), QJsonObject{
                {QStringLiteral("state"), QStringLiteral("not-required")},
            }},
            {QStringLiteral("git"), QJsonObject{
                {QStringLiteral("state"), QStringLiteral("not-required")},
            }},
        }},
    });
    runtimeClient->operationReconciled({}, QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("operation-reconciliation-result/0.1")},
        {QStringLiteral("reconciliation"), QJsonObject{
            {QStringLiteral("operation_id"), QStringLiteral("turn-operation-fixture")},
            {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
            {QStringLiteral("kind"), QStringLiteral("turn")},
            {QStringLiteral("state"), QStringLiteral("completed")},
            {QStringLiteral("writes_blocked"), false},
        }},
    });
    runtimeClient->operationStatusRead({}, QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("operation-reconciliation-status/0.1")},
        {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("blocked"), false},
        {QStringLiteral("operation"), QJsonValue::Null},
        {QStringLiteral("recovery_action_available"), false},
    });
    application.processEvents();
    if (!expect(operationStatusBanner->isHidden()
                    && operationStatusReview->isHidden()
                    && sendButton->text() == QStringLiteral("发送")
                    && sendButton->isEnabled(),
                "successful operation review did not clear the read-only gate")) {
        return 1;
    }
    runtimeClient->operationStatusRead({}, QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("operation-reconciliation-status/0.1")},
        {QStringLiteral("session_id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("blocked"), false},
        {QStringLiteral("operation"), QJsonValue::Null},
        {QStringLiteral("recovery_action_available"), false},
    });
    application.processEvents();
    if (!expect(operationStatusBanner->isHidden()
                    && sendButton->text() == QStringLiteral("发送")
                    && sendButton->isEnabled() && !operationComposer->isReadOnly(),
                "cleared operation status did not restore the composer")) {
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

    const QList<QPair<QString, QString>> stableErrorClasses = {
        {QStringLiteral("protocol"), QStringLiteral("协议")},
        {QStringLiteral("sandbox"), QStringLiteral("沙箱")},
        {QStringLiteral("policy"), QStringLiteral("策略")},
        {QStringLiteral("tool"), QStringLiteral("工具")},
        {QStringLiteral("storage"), QStringLiteral("本地存储")},
        {QStringLiteral("workspace"), QStringLiteral("工作区")},
        {QStringLiteral("git"), QStringLiteral("Git")},
        {QStringLiteral("budget"), QStringLiteral("预算")},
        {QStringLiteral("adapter"), QStringLiteral("适配器")},
    };
    for (int index = 0; index < stableErrorClasses.size(); ++index) {
        const auto &errorClass = stableErrorClasses.at(index);
        runtimeClient->timelineEvent(QJsonObject{
            {QStringLiteral("session_id"), QStringLiteral("session-error-class-fixture")},
            {QStringLiteral("turn_id"), QStringLiteral("turn-error-class-%1").arg(index)},
            {QStringLiteral("event"), QStringLiteral("turn.failed")},
            {QStringLiteral("item"), QJsonObject{
                {QStringLiteral("id"), QStringLiteral("runtime-error-class-%1").arg(index)},
                {QStringLiteral("kind"), QStringLiteral("error")},
                {QStringLiteral("role"), QStringLiteral("system")},
                {QStringLiteral("state"), QStringLiteral("completed")},
                {QStringLiteral("content"), QStringLiteral("bounded failure")},
                {QStringLiteral("data"), QJsonObject{
                    {QStringLiteral("schema_version"), QStringLiteral("runtime-error/0.1")},
                    {QStringLiteral("class"), errorClass.first},
                    {QStringLiteral("retryable"), false},
                }},
            }},
        });
    }
    application.processEvents();
    for (const auto &errorClass : stableErrorClasses) {
        bool visible = false;
        const QString expected = QStringLiteral("任务失败 · 类型：%1 · 请检查配置或运行时状态")
            .arg(errorClass.second);
        for (QLabel *label : workbench.findChildren<QLabel *>()) {
            if (label->text().contains(expected)) {
                visible = true;
                break;
            }
        }
        if (!expect(visible, "Qt did not map a stable runtime error class")) return 1;
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
    QFile gitChange(project.filePath(QStringLiteral("git-change.txt")));
    if (!expect(gitChange.open(QIODevice::WriteOnly),
                "could not create Git context fixture")) {
        return 1;
    }
    gitChange.write("base\n");
    gitChange.close();
    const QString gitExecutable = QStandardPaths::findExecutable(QStringLiteral("git"));
    QString fixtureCommitOid;
    if (!expect(!gitExecutable.isEmpty()
                    && runGit(gitExecutable, project.path(), {QStringLiteral("init"),
                                                              QStringLiteral("-q")})
                    && runGit(gitExecutable, project.path(), {QStringLiteral("config"),
                                                              QStringLiteral("user.name"),
                                                              QStringLiteral("Aegisy Render Test")})
                    && runGit(gitExecutable, project.path(), {QStringLiteral("config"),
                                                              QStringLiteral("user.email"),
                                                              QStringLiteral("render@aegisy.invalid")})
                    && runGit(gitExecutable, project.path(), {QStringLiteral("add"),
                                                              QStringLiteral(".")})
                    && runGit(gitExecutable, project.path(), {QStringLiteral("commit"),
                                                              QStringLiteral("-q"),
                                                              QStringLiteral("-m"),
                                                              QStringLiteral("initial Git context")})
                    && runGit(gitExecutable, project.path(), {QStringLiteral("rev-parse"),
                                                              QStringLiteral("HEAD")},
                              &fixtureCommitOid)
                    && fixtureCommitOid.size() == 40,
                "could not initialize real Git context fixture")) {
        return 1;
    }
    if (!expect(gitChange.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "could not create worktree Git diff")) {
        return 1;
    }
    gitChange.write("base\nworktree change\n");
    gitChange.close();
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
    QString fixtureBranch;
    if (!expect(runGit(gitExecutable, project.path(), {QStringLiteral("branch"),
                                                       QStringLiteral("--show-current")},
                       &fixtureBranch)
                    && !fixtureBranch.isEmpty(),
                "could not read the Git fixture branch")) {
        return 1;
    }
    runtime->gitOverview(openedProjectId);
    if (!expect(waitUntil(application, [executionContext, &fixtureBranch]() {
                    return executionContext->text().contains(QStringLiteral("项目 "))
                        && executionContext->text().contains(
                            QStringLiteral("分支 %1").arg(fixtureBranch));
                }),
                "execution-context strip did not project the read-only Git branch")) {
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
    if (!expect(QMetaObject::invokeMethod(
                    &workbench, "beginBackgroundNotificationInspection",
                    Qt::DirectConnection, Q_ARG(QString, previewSessionId)),
                "background notification inspection entry is not invokable")) {
        return 1;
    }
    QDialog *notificationDialog = nullptr;
    if (!expect(waitUntil(application, [&workbench, &notificationDialog]() {
                    notificationDialog = workbench.findChild<QDialog *>(
                        QStringLiteral("agentBackgroundNotificationDialog"));
                    return notificationDialog != nullptr;
                }),
                "background notification dialog did not open from the durable AAP query")) {
        return 1;
    }
    QTableWidget *notificationTable = notificationDialog->findChild<QTableWidget *>(
        QStringLiteral("agentBackgroundNotificationTable"));
    QPushButton *notificationMore = notificationDialog->findChild<QPushButton *>(
        QStringLiteral("agentBackgroundNotificationMoreButton"));
    if (!expect(notificationTable && notificationMore
                    && notificationTable->rowCount() == 1
                    && notificationTable->item(0, 0)
                    && notificationTable->item(0, 0)->text()
                        == QStringLiteral("暂无后台通知记录")
                    && notificationMore->isHidden(),
                "background notification empty state or pagination gate is invalid")) {
        return 1;
    }
    notificationDialog->close();
    application.processEvents();
    if (!expect(QMetaObject::invokeMethod(
                    &workbench, "beginBackgroundRecoveryInspection",
                    Qt::DirectConnection, Q_ARG(QString, previewSessionId)),
                "background recovery inspection entry is not invokable")) {
        return 1;
    }
    QDialog *recoveryDialog = nullptr;
    if (!expect(waitUntil(application, [&workbench, &recoveryDialog]() {
                    recoveryDialog = workbench.findChild<QDialog *>(
                        QStringLiteral("agentBackgroundRecoveryDialog"));
                    return recoveryDialog != nullptr;
                }),
                "background recovery dialog did not open from the durable AAP query")) {
        return 1;
    }
    QTableWidget *recoveryTable = recoveryDialog->findChild<QTableWidget *>(
        QStringLiteral("agentBackgroundRecoveryTable"));
    QPushButton *recoveryMore = recoveryDialog->findChild<QPushButton *>(
        QStringLiteral("agentBackgroundRecoveryMoreButton"));
    if (!expect(recoveryTable && recoveryMore
                    && recoveryTable->rowCount() == 1
                    && recoveryTable->item(0, 0)
                    && recoveryTable->item(0, 0)->text()
                        == QStringLiteral("暂无后台恢复记录")
                    && recoveryMore->isHidden(),
                "background recovery empty state or pagination gate is invalid")) {
        return 1;
    }
    recoveryDialog->close();
    application.processEvents();
    int gitTabIndex = -1;
    for (int index = 0; index < tabs->count(); ++index) {
        if (tabs->tabText(index) == QStringLiteral("Git")) {
            gitTabIndex = index;
            break;
        }
    }
    if (!expect(gitTabIndex >= 0, "Git workspace tab is missing")) return 1;
    tabs->setCurrentIndex(gitTabIndex);
    if (!expect(waitUntil(application, [gitHistory, gitDiff, gitPinDiff]() {
                    return gitHistory->topLevelItemCount() > 0
                        && gitDiff->toPlainText().contains(QStringLiteral("worktree change"))
                        && gitPinDiff->isEnabled();
                }, 10000),
                "real Git worktree diff did not enable persistent pinning")) {
        return 1;
    }
    gitPinDiff->click();
    if (!expect(waitUntil(application, [&workbench, contextSummary]() {
                    return workbench.findChildren<QPushButton *>(
                               QStringLiteral("agentPinnedContextRemoveButton")).size() == 1
                        && contextSummary->text().contains(QStringLiteral("固定 1"));
                }),
                "Git diff pin did not persist through the metadata-only CAS")) {
        return 1;
    }
    workbench.findChildren<QPushButton *>(
        QStringLiteral("agentPinnedContextRemoveButton")).first()->click();
    if (!expect(waitUntil(application, [&workbench]() {
                    return workbench.findChildren<QPushButton *>(
                               QStringLiteral("agentPinnedContextRemoveButton")).isEmpty();
                }),
                "Git diff pin could not be removed")) {
        return 1;
    }
    gitHistory->setCurrentItem(gitHistory->topLevelItem(0));
    if (!expect(waitUntil(application, [gitDiffScope, gitPinCommit, &fixtureCommitOid,
                                        gitHistory]() {
                    QTreeWidgetItem *selected = gitHistory->currentItem();
                    return selected
                        && selected->data(0, Qt::UserRole + 7).toString() == fixtureCommitOid
                        && gitDiffScope->currentData().toString() == QStringLiteral("commit")
                        && gitPinCommit->isEnabled();
                }),
                "selected Git commit did not enable persistent commit pinning")) {
        return 1;
    }
    gitPinCommit->click();
    if (!expect(waitUntil(application, [&workbench]() {
                    const QList<QLabel *> labels = workbench.findChildren<QLabel *>();
                    return workbench.findChildren<QPushButton *>(
                               QStringLiteral("agentPinnedContextRemoveButton")).size() == 1
                        && std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                            return label->text().contains(QStringLiteral("initial Git context"));
                        });
                }),
                "Git commit pin did not persist and render its selected commit label")) {
        return 1;
    }
    workbench.findChildren<QPushButton *>(
        QStringLiteral("agentPinnedContextRemoveButton")).first()->click();
    if (!expect(waitUntil(application, [&workbench]() {
                    return workbench.findChildren<QPushButton *>(
                               QStringLiteral("agentPinnedContextRemoveButton")).isEmpty();
                }),
                "Git commit pin could not be removed")) {
        return 1;
    }
    const QJsonObject compactionSummary{
        {QStringLiteral("decisions"), QJsonArray{QStringLiteral("Keep history authoritative")}},
        {QStringLiteral("unresolved_tasks"), QJsonArray{QStringLiteral("Review activation")}},
        {QStringLiteral("changed_files"), QJsonArray{}},
        {QStringLiteral("commands"), QJsonArray{}},
        {QStringLiteral("tests"), QJsonArray{QStringLiteral("Qt client dispatch")}},
        {QStringLiteral("failures"), QJsonArray{}},
        {QStringLiteral("next_actions"), QJsonArray{QStringLiteral("Create revision")}},
    };
    QString compactionCreateRequest;
    QString compactionReviewId;
    QObject::connect(runtime, &AgentRuntimeClient::compactionCheckpointCreated,
                     &workbench, [&compactionCreateRequest, &compactionReviewId](
                         const QString &requestId, const QJsonObject &result) {
        if (requestId != compactionCreateRequest) return;
        compactionReviewId = result.value(QStringLiteral("review")).toObject()
            .value(QStringLiteral("review_id")).toString();
    });
    compactionCreateRequest = runtime->createCompactionCheckpoint(
        previewSessionId, QStringLiteral("qt-review-1"),
        QStringLiteral("Preserve reviewed decisions"), compactionSummary);
    if (!expect(!compactionCreateRequest.isEmpty()
                    && waitUntil(application, [&compactionReviewId]() {
                        return !compactionReviewId.isEmpty();
                    }),
                "Qt client did not dispatch compaction checkpoint creation")) {
        return 1;
    }
    QJsonObject revisedSummary = compactionSummary;
    revisedSummary.insert(
        QStringLiteral("next_actions"),
        QJsonArray{QStringLiteral("Keep revision lineage durable")});
    QString compactionRevisionRequest;
    QJsonObject compactionRevisionResult;
    QObject::connect(runtime, &AgentRuntimeClient::compactionCheckpointRevised,
                     &workbench, [&compactionRevisionRequest, &compactionRevisionResult](
                         const QString &requestId, const QJsonObject &result) {
        if (requestId == compactionRevisionRequest) compactionRevisionResult = result;
    });
    compactionRevisionRequest = runtime->reviseCompactionCheckpoint(
        previewSessionId, QStringLiteral("qt-review-1"), compactionReviewId,
        QStringLiteral("qt-review-2"), QStringLiteral("Preserve reviewed decisions"),
        revisedSummary);
    if (!expect(!compactionRevisionRequest.isEmpty()
                    && waitUntil(application, [&compactionRevisionResult]() {
                        return !compactionRevisionResult.isEmpty();
                    })
                    && compactionRevisionResult.value(QStringLiteral("schema_version")).toString()
                        == QStringLiteral("session-compaction-checkpoint-revise-result/0.1")
                    && compactionRevisionResult.value(QStringLiteral("supersedes")).toObject()
                        .value(QStringLiteral("review_id")).toString() == compactionReviewId
                    && !compactionRevisionResult
                        .value(QStringLiteral("activation_available")).toBool()
                    && !compactionRevisionResult
                        .value(QStringLiteral("provider_compact_invoked")).toBool(),
                "Qt client did not preserve immutable compaction revision lineage")) {
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
    sessionSearch->setText(QStringLiteral("Review"));
    if (!expect(waitUntil(application, [sessionList]() {
                    return sessionList->count() == 1
                        && sessionList->item(0)->text().contains(
                            QStringLiteral("Review session"));
                }),
                "session search did not return a title match")) {
        return 1;
    }
    sessionSearch->setText(QStringLiteral("no-such-session"));
    if (!expect(waitUntil(application, [sessionList]() {
                    return sessionList->count() == 1
                        && sessionList->item(0)->text() == QStringLiteral("暂无匹配会话");
                }),
                "session search did not render an explicit empty result")) {
        return 1;
    }
    sessionSearch->clear();
    if (!expect(waitUntil(application, [sessionList]() {
                    return sessionList->count() > 0
                        && sessionList->item(0)->text().contains(
                            QStringLiteral("Review session"));
                }),
                "clearing session search did not restore recent sessions")) {
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
    if (!expect(pinTerminalExcerptAction->isEnabled(),
                "terminal output did not enable its explicit pin action")) {
        return 1;
    }
    pinTerminalExcerptAction->trigger();
    if (!expect(waitUntil(application, [&workbench, contextSummary]() {
                    return workbench.findChildren<QPushButton *>(
                               QStringLiteral("agentPinnedContextRemoveButton")).size() == 1
                        && contextSummary->text().contains(QStringLiteral("固定 1"));
                }),
                "terminal excerpt pin did not persist a session-scoped descriptor")) {
        return 1;
    }
    terminalRemove->click();
    if (!expect(waitUntil(application, [terminalPicker, &workbench]() {
                    if (terminalPicker->count() != 0) return false;
                    const QList<QLabel *> labels = workbench.findChildren<QLabel *>();
                    return std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                        return label->text().contains(QStringLiteral("终端摘录"))
                            && label->text().contains(QStringLiteral("stale"));
                    });
                }),
                "terminal removal did not mark the pinned excerpt stale")) {
        return 1;
    }
    const QList<QPushButton *> terminalPinRemoveButtons = workbench.findChildren<QPushButton *>(
        QStringLiteral("agentPinnedContextRemoveButton"));
    if (!expect(terminalPinRemoveButtons.size() == 1,
                "stale terminal excerpt did not retain its unpin control")) return 1;
    terminalPinRemoveButtons.first()->click();
    if (!expect(waitUntil(application, [&workbench]() {
                    return workbench.findChildren<QPushButton *>(
                               QStringLiteral("agentPinnedContextRemoveButton")).isEmpty();
                }),
                "stale terminal excerpt unpin did not remove the persisted descriptor")) {
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
    if (!expect(waitUntil(application, [executionContext, modelPicker]() {
                    return executionContext->text().contains(
                               QStringLiteral("Runtime preview"))
                        && executionContext->text().contains(
                            QStringLiteral("模型 deterministic-echo"))
                        && executionContext->text().contains(QStringLiteral("权限 只读"))
                        && modelPicker->currentText()
                            == QStringLiteral("local / deterministic-echo")
                        && modelPicker->toolTip().contains(QStringLiteral("preview"))
                        && modelPicker->toolTip().contains(QStringLiteral("read-only"));
                }),
                "active Work session did not project its persisted Runtime binding")) {
        return 1;
    }
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
    const QByteArray artifactContent("complete command artifact\n");
    const QString artifactSha = QString::fromLatin1(QCryptographicHash::hash(
        artifactContent, QCryptographicHash::Sha256).toHex());
    const QString artifactReference = QStringLiteral("command-output:sha256:%1")
        .arg(artifactSha);
    const auto artifactResponse = [&](const QString &sessionId) {
        return QJsonObject{
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("reference"), artifactReference},
            {QStringLiteral("sha256"), artifactSha},
            {QStringLiteral("content_type"), QStringLiteral("text/plain; charset=utf-8")},
            {QStringLiteral("item_id"), QStringLiteral("command-render-fixture")},
            {QStringLiteral("total_bytes"), artifactContent.size()},
            {QStringLiteral("retained_bytes"), artifactContent.size()},
            {QStringLiteral("omitted_bytes"), 0},
            {QStringLiteral("content"), QString::fromUtf8(artifactContent)},
        };
    };
    runtime->commandArtifactRead(
        QStringLiteral("artifact-current-session"), artifactResponse(previewSessionId));
    QPushButton *artifactPin = nullptr;
    if (!expect(waitUntil(application, [&workbench, &artifactPin]() {
                    artifactPin = workbench.findChild<QPushButton *>(
                        QStringLiteral("commandArtifactPinButton"));
                    return artifactPin && artifactPin->isVisible();
                })
                    && artifactPin->isEnabled()
                    && artifactPin->toolTip().contains(QStringLiteral("不会自动发送")),
                "current Work command artifact did not expose an explicit pin action")) {
        return 1;
    }
    if (QDialog *dialog = qobject_cast<QDialog *>(artifactPin->window())) dialog->close();
    application.processEvents();
    runtime->commandArtifactRead(
        QStringLiteral("artifact-other-session"), artifactResponse(
            QStringLiteral("session-other-render-fixture")));
    artifactPin = nullptr;
    if (!expect(waitUntil(application, [&workbench, &artifactPin]() {
                    const QList<QPushButton *> buttons = workbench.findChildren<QPushButton *>(
                        QStringLiteral("commandArtifactPinButton"));
                    auto visible = std::find_if(buttons.cbegin(), buttons.cend(),
                        [](QPushButton *button) { return button->isVisible(); });
                    artifactPin = visible == buttons.cend() ? nullptr : *visible;
                    return artifactPin != nullptr;
                })
                    && !artifactPin->isEnabled()
                    && artifactPin->toolTip().contains(QStringLiteral("当前 Work 会话")),
                "cross-session command artifact pinning did not fail closed")) {
        return 1;
    }
    if (QDialog *dialog = qobject_cast<QDialog *>(artifactPin->window())) dialog->close();
    fileTree->setCurrentItem(editableItem);
    fileContextAction->trigger();
    if (!expect(waitUntil(application, [contextInspect]() {
                    return contextInspect->isEnabled();
                }),
                "context inspector did not become available for an active Work session")) {
        return 1;
    }
#ifdef AEGISY_EXPECT_AGENTD
    contextInspect->click();
    QTreeWidget *inspectionTable = nullptr;
    if (!expect(waitUntil(application, [&workbench, &inspectionTable]() {
                    inspectionTable = workbench.findChild<QTreeWidget *>(
                        QStringLiteral("agentContextInspectionTable"));
                    return inspectionTable && inspectionTable->topLevelItemCount() > 0;
                }),
                "context inspector did not render the read-only manifest")) {
        return 1;
    }
    QLabel *inspectionSummary = workbench.findChild<QLabel *>(
        QStringLiteral("agentContextInspectionSummary"));
    if (!expect(inspectionSummary
                    && inspectionSummary->text().contains(QStringLiteral("预算")),
                "context inspector did not render budget metadata")) {
        return 1;
    }
    if (QDialog *dialog = qobject_cast<QDialog *>(inspectionTable->window())) dialog->close();
#endif
    const QList<QPushButton *> inspectionRemoveButtons = workbench.findChildren<QPushButton *>(
        QStringLiteral("agentContextRemoveButton"));
    auto inspectionRemove = std::find_if(inspectionRemoveButtons.cbegin(),
                                         inspectionRemoveButtons.cend(),
                                         [](QPushButton *button) { return button->isVisible(); });
    if (inspectionRemove != inspectionRemoveButtons.cend()) (*inspectionRemove)->click();
    if (!expect(waitUntil(application, [contextList]() {
                    return contextList->count() == 0;
                }),
                "context inspector fixture did not clean up its context item")) {
        return 1;
    }
    fileTree->setCurrentItem(editableItem);
    if (!expect(pinFileContextAction->isEnabled(), "file pin action was not enabled after project open")) {
        return 1;
    }
    pinFileContextAction->trigger();
    if (!expect(waitUntil(application, [&workbench, contextSummary]() {
                    return !workbench.findChildren<QPushButton *>(
                                QStringLiteral("agentPinnedContextRemoveButton")).isEmpty()
                        && contextSummary->text().contains(QStringLiteral("固定 1"));
                }),
                "file pin action did not persist and render a fixed context row")) {
        return 1;
    }
    const QList<QPushButton *> pinMoveUpButtons = workbench.findChildren<QPushButton *>(
        QStringLiteral("agentPinnedContextMoveUpButton"));
    const QList<QPushButton *> pinMoveDownButtons = workbench.findChildren<QPushButton *>(
        QStringLiteral("agentPinnedContextMoveDownButton"));
    if (!expect(pinMoveUpButtons.size() == 1 && pinMoveDownButtons.size() == 1
                    && !pinMoveUpButtons.first()->isEnabled()
                    && !pinMoveDownButtons.first()->isEnabled(),
                "single fixed context row did not expose bounded order controls")) {
        return 1;
    }
    QList<QCheckBox *> pinChecks = workbench.findChildren<QCheckBox *>(
        QStringLiteral("agentContextIncludeCheck"));
    pinChecks.erase(std::remove_if(pinChecks.begin(), pinChecks.end(),
                                   [](QCheckBox *check) { return !check->isVisible(); }),
                    pinChecks.end());
    if (!expect(pinChecks.size() == 1, "fixed context row did not expose inclusion control")) {
        return 1;
    }
    pinChecks.first()->setChecked(false);
    if (!expect(waitUntil(application, [contextSummary]() {
                    return contextSummary->text().contains(QStringLiteral("发送 0/16 项"));
                }),
                "fixed context inclusion toggle did not update turn selection")) {
        return 1;
    }
    pinChecks = workbench.findChildren<QCheckBox *>(QStringLiteral("agentContextIncludeCheck"));
    pinChecks.erase(std::remove_if(pinChecks.begin(), pinChecks.end(),
                                   [](QCheckBox *check) { return !check->isVisible(); }),
                    pinChecks.end());
    pinChecks.first()->setChecked(true);
    if (!expect(waitUntil(application, [contextSummary]() {
                    return contextSummary->text().contains(QStringLiteral("发送 1/16 项"));
                }),
                "fixed context inclusion toggle did not restore turn selection")) {
        return 1;
    }
#ifdef AEGISY_EXPECT_AGENTD
    contextInspect->click();
    QTreeWidget *pinnedInspection = nullptr;
    if (!expect(waitUntil(application, [&workbench, &pinnedInspection]() {
                    pinnedInspection = workbench.findChild<QTreeWidget *>(
                        QStringLiteral("agentContextInspectionTable"));
                    return pinnedInspection && pinnedInspection->topLevelItemCount() > 0;
                }),
                "fixed context selection was not sent through the inspect AAP path")) {
        return 1;
    }
    if (QDialog *dialog = qobject_cast<QDialog *>(pinnedInspection->window())) dialog->close();
#endif
    const QList<QPushButton *> pinRemoveButtons = workbench.findChildren<QPushButton *>(
        QStringLiteral("agentPinnedContextRemoveButton"));
    if (!expect(pinRemoveButtons.size() == 1, "fixed context row did not expose unpin control")) {
        return 1;
    }
    pinRemoveButtons.first()->click();
    if (!expect(waitUntil(application, [&workbench]() {
                    return workbench.findChildren<QPushButton *>(
                               QStringLiteral("agentPinnedContextRemoveButton")).isEmpty();
                }),
                "fixed context unpin did not remove the persisted row")) {
        return 1;
    }
    if (!expect(pinSelectionContextAction->isEnabled(),
                "selection pin action was not enabled for the editor selection")) {
        return 1;
    }
    pinSelectionContextAction->trigger();
    if (!expect(waitUntil(application, [&workbench, contextSummary]() {
                    const QList<QLabel *> labels = workbench.findChildren<QLabel *>();
                    return workbench.findChildren<QPushButton *>(
                               QStringLiteral("agentPinnedContextRemoveButton")).size() == 1
                        && contextSummary->text().contains(QStringLiteral("固定 1"))
                        && std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                            return label->text().contains(QStringLiteral("editable.txt:1:1-1:9"));
                        });
                }),
                "selection pin action did not persist and render its range")) {
        return 1;
    }
    editor->selectAll();
    editor->insertPlainText(QStringLiteral("saved\ncontent\n"));
    if (!expect(!pinSelectionContextAction->isEnabled(),
                "selection pin action stayed enabled for unsaved editor content")) {
        return 1;
    }
    editorSave->click();
    if (!expect(waitUntil(application, [&workbench, editor]() {
                    const QList<QLabel *> labels = workbench.findChildren<QLabel *>();
                    return !editor->document()->isModified()
                        && std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                            return label->text().contains(QStringLiteral("editable.txt:1:1-1:9"))
                                && label->text().contains(QStringLiteral("stale"));
                        });
                }),
                "saving a pinned selection did not mark the local pin stale")) {
        return 1;
    }
    const QList<QPushButton *> selectionPinRemoveButtons = workbench.findChildren<QPushButton *>(
        QStringLiteral("agentPinnedContextRemoveButton"));
    selectionPinRemoveButtons.first()->click();
    if (!expect(waitUntil(application, [&workbench]() {
                    return workbench.findChildren<QPushButton *>(
                               QStringLiteral("agentPinnedContextRemoveButton")).isEmpty();
                }),
                "selection pin unpin did not remove the persisted row")) {
        return 1;
    }
    QImage pinnedImageFixture(96, 64, QImage::Format_RGBA8888);
    pinnedImageFixture.fill(QColor(QStringLiteral("#165DFF")));
    QByteArray pinnedImageBytes;
    QBuffer pinnedImageBuffer(&pinnedImageBytes);
    pinnedImageBuffer.open(QIODevice::WriteOnly);
    if (!expect(pinnedImageFixture.save(&pinnedImageBuffer, "PNG"),
                "could not encode pinned image render fixture")) {
        return 1;
    }
    const QString pinnedImagePath = project.filePath(QStringLiteral("layout.png"));
    QFile pinnedImageFile(pinnedImagePath);
    if (!expect(pinnedImageFile.open(QIODevice::WriteOnly)
                    && pinnedImageFile.write(pinnedImageBytes) == pinnedImageBytes.size(),
                "could not write pinned image render fixture")) {
        return 1;
    }
    pinnedImageFile.close();
    QMenu *attachMenu = attachContext->menu();
    attachMenu->popup(attachContext->mapToGlobal(QPoint(0, attachContext->height())));
    if (!expect(waitUntil(application, [pinImageContextAction]() {
                    return pinImageContextAction->isEnabled();
                }),
                "image pin action did not enable for the active Work session")) {
        return 1;
    }
    attachMenu->hide();
    QTimer::singleShot(0, &workbench, [&]() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            QFileDialog *dialog = qobject_cast<QFileDialog *>(widget);
            if (!dialog) continue;
            dialog->selectFile(pinnedImagePath);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        }
    });
    pinImageContextAction->trigger();
    if (!expect(waitUntil(application, [&workbench]() {
                        return workbench.findChildren<QPushButton *>(
                                   QStringLiteral("agentPinnedImagePreviewButton")).size() == 1;
                    }),
                "image import did not persist and render a previewable fixed row")) {
        return 1;
    }
    const QList<QLabel *> imageLabels = workbench.findChildren<QLabel *>();
    if (!expect(std::any_of(imageLabels.cbegin(), imageLabels.cend(), [](QLabel *label) {
                    return label->text().contains(QStringLiteral("layout.png"))
                        && label->text().contains(QStringLiteral("96 × 64"))
                        && label->text().contains(QStringLiteral("image/png"));
                }),
                "fixed image row did not show label, dimensions, and media type")) {
        return 1;
    }
    bool imagePreviewSeen = false;
    QTimer previewCloser;
    previewCloser.setInterval(10);
    QObject::connect(&previewCloser, &QTimer::timeout, &workbench, [&]() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            QDialog *dialog = qobject_cast<QDialog *>(widget);
            if (!dialog || dialog->windowTitle() != QStringLiteral("固定图片预览")) continue;
            const QList<QLabel *> labels = dialog->findChildren<QLabel *>();
            imagePreviewSeen = std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                return label->text().contains(QStringLiteral("96 × 64"));
            });
            dialog->accept();
        }
    });
    previewCloser.start();
    workbench.findChildren<QPushButton *>(
        QStringLiteral("agentPinnedImagePreviewButton")).first()->click();
    if (!expect(waitUntil(application, [&imagePreviewSeen]() { return imagePreviewSeen; }),
                "fixed image preview did not render the scoped thumbnail")) {
        return 1;
    }
    previewCloser.stop();
    workbench.findChildren<QPushButton *>(
        QStringLiteral("agentPinnedContextRemoveButton")).first()->click();
    const bool imageUnpinned = waitUntil(application, [&workbench]() {
        return workbench.findChildren<QPushButton *>(
                   QStringLiteral("agentPinnedContextRemoveButton")).isEmpty();
    });
    if (!imageUnpinned) {
        for (QLabel *label : workbench.findChildren<QLabel *>()) {
            if (label->text().contains(QStringLiteral("固定上下文"))) {
                qWarning().noquote() << label->text();
            }
        }
    }
    if (!expect(imageUnpinned,
                "fixed image unpin did not remove the persisted descriptor")) {
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
        QAction *pinDiagnosticContext = workbench.findChild<QAction *>(
            QStringLiteral("agentPinDiagnosticContextTableAction"));
        if (!expect(pinDiagnosticContext,
                    "diagnostic table did not expose an explicit pin action")) {
            return 1;
        }
        pinDiagnosticContext->trigger();
        if (!expect(waitUntil(application, [&workbench, contextSummary]() {
                        return workbench.findChildren<QPushButton *>(
                                   QStringLiteral("agentPinnedContextRemoveButton")).size() == 1
                            && contextSummary->text().contains(QStringLiteral("固定 1"));
                    }),
                    "diagnostic pin action did not persist a project/root-scoped descriptor")) {
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
        if (!expect(waitUntil(application, [diagnosticItem, languageStatus, &workbench]() {
                        return diagnosticItem->text(3) == QStringLiteral("已过期")
                            && languageStatus->text().contains(QStringLiteral("已过期"))
                            && [&workbench]() {
                        const QList<QLabel *> labels = workbench.findChildren<QLabel *>();
                        return std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                            return label->text().contains(QStringLiteral("source.cpp"))
                                && label->text().contains(QStringLiteral("stale"));
                        });
                    }();
                    }),
                    "workspace save did not mark observed diagnostics and pinned context stale")) {
            return 1;
        }
        const QList<QPushButton *> diagnosticPinRemoveButtons = workbench.findChildren<QPushButton *>(
            QStringLiteral("agentPinnedContextRemoveButton"));
        if (!expect(diagnosticPinRemoveButtons.size() == 1,
                    "stale diagnostic pin did not retain its unpin control")) return 1;
        diagnosticPinRemoveButtons.first()->click();
        if (!expect(waitUntil(application, [&workbench]() {
                        return workbench.findChildren<QPushButton *>(
                                   QStringLiteral("agentPinnedContextRemoveButton")).isEmpty();
                    }),
                    "stale diagnostic pin unpin did not remove the persisted descriptor")) {
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
