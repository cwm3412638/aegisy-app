#ifndef AGENT_WORKBENCH_WIDGET_H
#define AGENT_WORKBENCH_WIDGET_H

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <functional>

class AgentRuntimeClient;
#ifdef AEGISY_HAS_MONACO
class MonacoEditorBridge;
class TerminalWebBridge;
class QStackedWidget;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineView;
#endif
class QButtonGroup;
class QAction;
class QCheckBox;
class QComboBox;
class QDialog;
class QLineEdit;
class QListWidgetItem;
class QPlainTextEdit;
class QLabel;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTabBar;
class QTabWidget;
class QTableWidget;
class QTextEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

class AgentWorkbenchWidget : public QWidget
{
    Q_OBJECT

    friend class AgentWorkbenchWidgetTestAccess;

public:
    explicit AgentWorkbenchWidget(bool emergencyDisabled = false,
                                  QWidget *parent = nullptr);
    ~AgentWorkbenchWidget() override;
    void setEmergencyDisabled(bool disabled, const QString &detailCode = QString(),
                              bool verifiedPolicy = false);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class TimelineRecoveryState {
        Untracked,
        Subscribing,
        RecoveringSync,
        RecoveringSnapshot,
        AwaitingActivation,
        Active,
        Failed,
        Frozen,
        Unrecoverable,
    };
    enum class TimelineAnchorConfirmation {
        Pending,
        Confirmed,
        Drifted,
    };
    struct TimelineProjection {
        quint64 sequence = 0;
        quint64 timestampMs = 0;
        bool timestampKnown = false;
        QString eventId;
        QHash<quint64, QString> eventIds;
        QHash<QString, QString> turnStates;
        QHash<QString, QString> itemKinds;
        QHash<QString, QString> itemRoles;
        QHash<QString, QString> itemStates;
        QHash<QString, quint64> itemRevisions;
    };
    struct TimelineStagedEvent {
        QJsonObject event;
        QJsonObject item;
        bool recognized = false;
    };
    struct TimelineSessionState {
        TimelineRecoveryState recovery = TimelineRecoveryState::Untracked;
        TimelineProjection projection;
        TimelineProjection syncProjection;
        bool syncProjectionValid = false;
        quint64 subscriptionGeneration = 0;
        QString subscriptionId;
        QString subscribeRequestId;
        QString activationRequestId;
        QString activationSource;
        QJsonObject subscriptionCursor;
        QJsonObject subscriptionWatermark;
        QString syncRequestId;
        QString snapshotRequestId;
        bool snapshotRecoveryRequired = false;
        QString snapshotIdentity;
        QJsonObject snapshotFloor;
        QJsonObject snapshotWatermark;
        QJsonObject snapshotAfter;
        QJsonObject snapshotActiveTurn;
        quint64 snapshotTotalItems = 0;
        quint64 snapshotExpectedCanonicalBytes = 0;
        quint64 snapshotTotalCanonicalBytes = 0;
        QList<QJsonObject> stagedSnapshotItems;
        qsizetype stagedSnapshotBytes = 0;
        quint64 requestedAfterSequence = 0;
        QString requestedAfterEventId;
        QJsonObject watermark;
        QList<TimelineStagedEvent> stagedSyncEvents;
        qsizetype stagedSyncBytes = 0;
        QList<QJsonObject> queuedLiveEvents;
        qsizetype queuedLiveBytes = 0;
        int subscriptionRetryCount = 0;
        bool retryOnReconnect = false;
    };
    struct DurableMutationListRequest {
        QString sessionId;
        QJsonObject after;
        quint64 generation = 0;
        bool reconnectRecovery = false;
    };
    struct DurableMutationConsumeRequest {
        QString sessionId;
        QString operationIdentity;
        QString target;
        QJsonObject confirmedAnchor;
        quint64 expectedRevision = 0;
        quint64 generation = 0;
        bool reconnectRecovery = false;
    };

    void buildUi();
    QWidget *buildProductRail();
    QWidget *buildAgentSurface();
    QWidget *buildWorkCanvas();
    QWidget *buildGitPage();
    void setMode(const QString &mode);
    void chooseProject();
    void requestProjectList();
    void populateProjectList(const QJsonObject &result);
    void openProjectFromList(QListWidgetItem *item);
    void beginProjectRootManagement();
    void showProjectRootsDialog(const QJsonObject &result);
    void requestSessionList();
    void populateSessionList(const QJsonObject &result);
    void beginSessionDeletion(QListWidgetItem *item);
    void confirmSessionDeletion(const QJsonObject &preview);
    void beginPortableSessionExport(QListWidgetItem *item);
    void confirmPortableSessionExport(const QJsonObject &preview);
    void beginPortableSessionImport();
    void confirmPortableSessionImport(const QJsonObject &preview);
    void beginRetentionPolicy(const QString &scopeKind, const QString &scopeId,
                              const QString &scopeLabel);
    void showRetentionPolicyDialog(const QJsonObject &result);
    void updateRecoveryUi();
    void requestOperationStatus();
    void updateOperationStatusUi(const QJsonObject &status);
    void reviewOperationStatus();
    void updateOperationReviewButton();
    void showOperationReviewFailure(const QString &message);
    void beginCompactionCheckpoint(const QString &sessionId);
    void beginCompactionCheckpointRead(const QString &sessionId);
    void beginCompactionCheckpointRevision(const QString &sessionId,
                                           const QJsonObject &sourceReview);
    void showCompactionReview(const QJsonObject &result, bool replayed);
    Q_INVOKABLE void beginBackgroundNotificationInspection(const QString &sessionId);
    void showBackgroundNotificationPage(const QJsonObject &result);
    Q_INVOKABLE void beginBackgroundRecoveryInspection(const QString &sessionId);
    void showBackgroundRecoveryPage(const QJsonObject &result);
    bool currentOperationStatusBlocked() const;
    bool currentSessionRecoveryRequired() const;
    bool currentSessionDeletionPending() const;
    void loadSessionFromList(QListWidgetItem *item);
    void loadOlderSessionHistory();
    QString requestSessionHistory(const QString &sessionId,
                                  const QString &cursor = QString(),
                                  int limit = 100,
                                  bool appending = false);
    void clearSessionReadRequest();
    void resetSessionHistoryPagination();
    void submitPrompt();
    void cancelActiveTurn();
    void updateTurnAction();
    void inspectContext();
    void showContextInspection(const QJsonObject &result);
    void startPendingTurnIfReady();
    void ensureSessionAndSubmit(const QString &prompt, const QJsonArray &context,
                                const QString &pinnedContextSetIdentity,
                                const QStringList &pinnedContextIds,
                                const QString &idempotencyKey);
    void addContextItem(QJsonObject item);
    void addSelectedFileContext();
    void pinSelectedFileContext();
    void pinEditorSelectionContext();
    bool canPinCommandArtifact(const QJsonObject &artifact, QString *reason = nullptr) const;
    void pinCommandArtifact(const QJsonObject &artifact);
    void finishPinnedFileRead(const QJsonObject &file);
    void pinImageContext();
    void finishPinnedImageImport(const QJsonObject &image);
    void previewPinnedImage(const QJsonObject &item);
    void showPinnedImagePreview(const QJsonObject &preview);
    void requestPinnedContext();
    void applyPinnedContextResult(const QJsonObject &result);
    void applyPinnedContextInvalidation(const QJsonObject &result);
    void savePinnedContextOrder();
    QJsonArray persistedPinnedContextItems() const;
    QStringList includedPinnedContextIds() const;
    void addEditorSelectionContext();
    void addSearchResultContext();
    void addDiagnosticContext();
    void pinSelectedDiagnosticContext();
    void finishPinnedDiagnosticRaw(const QJsonObject &raw);
    void addTextExcerptContext(const QString &kind, const QString &origin,
                               const QString &label, QPlainTextEdit *source);
    void markPinnedContextStale(const QSet<QString> &paths);
    void markPinnedTerminalStale(const QString &terminalId);
    void rebuildContextPanel();
    void clearContextItems();
    QJsonArray includedTurnContext() const;
    void addTimelineItem(const QJsonObject &item, bool prepend = false,
                         const QString &presentationKey = QString());
    bool validateTimelineItem(const QJsonObject &item,
                              const QString &expectedSessionId,
                              const QString &expectedTurnId,
                              QHash<QString, QString> *itemKinds,
                              QHash<QString, QString> *itemRoles) const;
    bool validateTimelineEvent(const QJsonObject &event,
                               TimelineProjection *projection,
                               QJsonObject *validatedItem = nullptr,
                               bool *recognizedEvent = nullptr) const;
    void handleLiveTimelineEvent(const QJsonObject &event,
                                 bool subscriptionOwned = false);
    void handleTimelineSyncPage(const QString &requestId, const QJsonObject &page);
    void handleTimelineSnapshotPage(const QString &requestId, const QJsonObject &page);
    void handleTimelineSubscribed(const QString &requestId, const QJsonObject &result);
    void handleTimelineSubscriptionEvent(const QJsonObject &event);
    void handleTimelineSubscriptionFailure(const QString &requestId,
                                           const QJsonObject &failure);
    void handleTimelineSubscriptionActivated(const QString &requestId,
                                             const QJsonObject &result);
    void handleTurnStarted(const QString &requestId, quint64 generation,
                           const QJsonObject &result);
    void handleMutationAcknowledgementPage(const QString &requestId,
                                           const QJsonObject &page);
    void handleMutationAcknowledgementConsumed(const QString &requestId,
                                               const QJsonObject &result);
    void beginMutationAcknowledgementRecovery(
        const QString &sessionId, const QJsonObject &after = QJsonObject(),
        bool reconnectRecovery = false);
    bool acceptDurableMutationOperation(const QJsonObject &operation);
    void processSessionMutationAcknowledgements(const QString &sessionId,
                                                bool reconnectRecovery = false);
    TimelineAnchorConfirmation timelineAnchorConfirmation(
        const QString &sessionId, const QJsonObject &anchor) const;
    void freezeSessionForMutationReconciliation(const QString &sessionId,
                                                const QString &detail);
    void finishReconnectMutationRecovery(const QString &sessionId);
    TimelineSessionState *ensureTimelineSession(const QString &sessionId);
    void beginTimelineSync(const QString &sessionId);
    void beginTimelineSubscription(const QString &sessionId);
    void beginTimelineSnapshot(const QString &sessionId);
    void suspendTimelinesForDisconnect();
    void markRuntimeBackedStateUnverified();
    void recoverRuntimeBackedStateAfterHandshake();
    void revalidateWorkspaceEditProposalAfterTimelineRecovery(
        const QString &sessionId);
    void beginRuntimeReconnectRecovery(quint64 generation,
                                       const QJsonObject &result);
    void continueRuntimeReconnectRecovery();
    void finishRuntimeReconnectTimeline(const QString &sessionId);
    bool runtimeRecoveryRequestsAllowed() const;
    void releaseTimelinePendingAccounting(const TimelineSessionState &state);
    void clearTimelinePending(TimelineSessionState &state);
    void clearTimelineSubscriptionAuthority(TimelineSessionState &state);
    void recoverTimelineSubscriptionOnNewConnection(const QString &sessionId,
                                                    const QString &detail);
    void commitTimelineSyncRecovery(const QString &sessionId);
    void commitTimelineSnapshotRecovery(const QString &sessionId);
    void freezeTimelineForSnapshotRecovery(const QString &sessionId,
                                           bool retryOnReconnect);
    void freezeTimelineSession(const QString &sessionId, bool unrecoverable,
                               bool retryOnReconnect = false);
    void publishTimelineProjection(const QString &sessionId,
                                   const TimelineProjection &projection);
    void restoreActiveTurnFromTimeline();
    void applyTimelineEventPresentation(const QJsonObject &event,
                                        const QJsonObject &item,
                                        bool recognizedEvent);
    QString currentTimelineSessionId() const;
    bool currentTimelineSessionFrozen() const;
    void addNotice(const QString &text, bool error = false);
    bool storeSessionRuntimeBinding(const QString &sessionId, const QJsonObject &runtime);
    bool storeSessionContextThreshold(const QString &sessionId,
                                      const QJsonObject &summary);
    bool storeSessionWorkspaceBinding(const QString &sessionId, const QJsonObject &workspace);
    QJsonObject activeSessionRuntimeBinding() const;
    QJsonObject activeSessionWorkspaceBinding() const;
    void updateSessionRuntimePresentation();
    void updateContextStrip();
    void populateDirectory(const QJsonObject &listing);
    void removeTreeItemMappings(QTreeWidgetItem *item);
    void requestDirectoryListing(const QString &directory);
    void markDirectoryUnavailable(const QString &directory, const QString &message);
    void updateWorkspaceWatch(const QString &directory);
    void applyFileFilter();
    bool filterTreeItem(QTreeWidgetItem *item, const QString &query);
    void refreshGitStatus();
    void applyGitDecorations();
    QString gitStatusForPath(const QString &path, bool directory) const;
    void refreshGitWorkspace();
    void requestGitDiff();
    void populateGitOverview(const QJsonObject &overview);
    void populateGitLog(const QJsonObject &log);
    void populateGitDiff(const QJsonObject &diff);
    void pinCurrentGitDiffContext();
    void pinSelectedGitCommitContext();
    void finishPinnedGitContext(const QJsonObject &context);
    void updateGitPinControls();
    void startWorkspaceSearch(bool nextPage = false);
    void cancelWorkspaceSearch();
    void appendWorkspaceSearchResults(const QJsonObject &result);
    void openWorkspaceSearchResult(QTreeWidgetItem *item);
    void refreshRepositoryIndex();
    void cancelRepositoryIndex();
    void populateRepositoryIndex(const QJsonObject &result);
    void requestRepositoryMap();
    void openRepositorySymbol(QTreeWidgetItem *item);
    void markRepositoryIndexStale();
    void requestLanguageServers();
    void requestLanguageAction(const QString &action);
    void populateLanguageLocations(const QJsonObject &result, const QString &type);
    void populateLanguageDiagnostics(const QJsonObject &result, bool activateView = true);
    void openLanguageResult(QTreeWidgetItem *item);
    void markLanguageResultsStale();
    void openWorkspaceFile(QTreeWidgetItem *item);
    void requestEditorFile(const QString &path, bool restoring = false);
    void activateEditorBuffer(const QString &path);
    void storeActiveEditorState();
    void closeEditorTab(int index);
    int editorTabIndex(const QString &path) const;
    void updateEditorTab(const QString &path);
    void addRecentFile(const QString &path);
    void updateRecentFilePicker();
    void loadEditorViewState();
    void saveEditorViewState();
    QString editorSettingsKey() const;
    void setEditorSplitEnabled(bool enabled);
    void activateEditorGroup(int group, const QString &path);
    void restoreEditorGroups();
    void setEditorSearchVisible(bool visible, bool replace = false);
    void findEditorText(bool backwards = false);
    void replaceEditorSelection();
    void replaceAllEditorText();
    void updateResponsiveEditorChrome();
    QWidget *buildTerminalPage();
    QWidget *buildWorkspaceEditPage();
    void populateWorkspaceEditPreview(const QJsonObject &preview, bool activate = true);
    void requestLatestWorkspaceEditProposal(const QString &sessionId);
    void requestWorkspaceEditProposalReference(const QString &itemKey,
                                               const QJsonObject &reference);
    void acceptWorkspaceEditProposalResult(const QString &requestId,
                                           const QJsonObject &result,
                                           bool latest);
    void showWorkspaceEditProposal(const QJsonObject &proposal, bool activate);
    void showConfirmedWorkspaceEditProposal(const QString &sessionId,
                                            bool activate);
    void clearWorkspaceEditProposalPending();
    void invalidateWorkspaceEditProposalArtifactRequests();
    void updateWorkspaceEditUnreadMarker();
    void showWorkspaceEditFile(QTreeWidgetItem *item);
    void loadMoreWorkspaceEditDiff();
    void requestTerminalList();
    void openTerminal(const QString &kind, const QString &name = QString());
    void activateTerminal(const QString &terminalId);
    void applyTerminalSnapshot(const QJsonObject &terminal, bool resetOutput = false);
    void pollActiveTerminal();
    void updateTerminalControls();
    void addTerminalSelectionContext();
    void pinRecentTerminalExcerpt();
    void finishPinnedTerminalExcerpt(const QJsonObject &excerpt);
#ifdef AEGISY_HAS_MONACO
    void initializeMonacoEditor(QWidget *parent);
    void syncMonacoModel();
    void syncMonacoModel(int group, const QString &path);
    void handleMonacoContent(const QString &path, const QString &content,
                             int cursorPosition, int anchorPosition);
    void handleMonacoView(int group, const QString &path,
                          int cursorPosition, int anchorPosition,
                          int verticalScroll, int horizontalScroll);
    void initializeTerminalWeb(QWidget *parent);
#endif
    void saveOpenFile();
    void reloadOpenFile();
    void resetEditorModel();
    void updateEditorActions();
    void showEditorFallback(const QString &path, const QJsonObject &metadata,
                            const QString &message);
    void updateRuntimeCapabilityUi();
    void requestModelCapabilityCheck();
    void loadWorkbenchLayout();
    void saveWorkbenchLayout();

    AgentRuntimeClient *m_runtime = nullptr;
    QButtonGroup *m_modeGroup = nullptr;
    QLabel *m_runtimeStatus = nullptr;
    QLabel *m_emergencyPolicyBanner = nullptr;
    QLabel *m_runtimeCapabilityStatus = nullptr;
    QPushButton *m_runtimeRestartButton = nullptr;
    QSplitter *m_workbenchSplitter = nullptr;
    bool m_modelProfileReadOnlyAvailable = false;
    bool m_modelProfileSnapshotValid = false;
    int m_modelProfileCount = 0;
    bool m_modelCapabilityReadOnlyAvailable = false;
    QString m_modelCapabilityRequestId;
    QString m_modelCapabilityModelId;
    QString m_modelCapabilityRuntime;
    QString m_modelCapabilityRuntimeVersion;
    QString m_modelCapabilityState;
    QString m_modelCatalogState;
    QString m_modelCatalogCacheState;
    QString m_modelCatalogRefreshState;
    QLabel *m_recoveryBanner = nullptr;
    QLabel *m_operationStatusBanner = nullptr;
    QWidget *m_operationStatusRow = nullptr;
    QPushButton *m_operationStatusReviewButton = nullptr;
    QLabel *m_projectLabel = nullptr;
    QString m_workspaceRootId = QStringLiteral("root-1");
    QString m_workspaceRootPath;
    QLabel *m_contextStrip = nullptr;
    QLabel *m_emptyTimeline = nullptr;
    QPushButton *m_sessionHistoryMoreButton = nullptr;
    QPushButton *m_newSessionButton = nullptr;
    QPushButton *m_openProjectButton = nullptr;
    QPushButton *m_retentionSettingsButton = nullptr;
    QPushButton *m_importSessionButton = nullptr;
    QString m_projectRootsRequestId;
    QString m_projectRootMutationRequestId;
    QString m_projectTrustRequestId;
    QJsonObject m_projectTrustReview;
    QString m_projectListRequestId;
    QString m_projectNavigationRequestId;
    QString m_sessionResumeRequestId;
    QString m_sessionForkRequestId;
    QComboBox *m_modelPicker = nullptr;
    QListWidget *m_projectList = nullptr;
    QLineEdit *m_sessionSearchInput = nullptr;
    QListWidget *m_sessionList = nullptr;
    QScrollArea *m_timelineScroll = nullptr;
    QWidget *m_timelineContent = nullptr;
    QVBoxLayout *m_timelineLayout = nullptr;
    QTextEdit *m_composer = nullptr;
    QWidget *m_contextPanel = nullptr;
    QLabel *m_contextSummary = nullptr;
    QListWidget *m_contextList = nullptr;
    QPushButton *m_attachContextButton = nullptr;
    QPushButton *m_contextInspectButton = nullptr;
    QAction *m_pinFileContextAction = nullptr;
    QAction *m_pinSelectionContextAction = nullptr;
    QAction *m_pinDiagnosticContextAction = nullptr;
    QAction *m_pinImageContextAction = nullptr;
    QPushButton *m_sendButton = nullptr;
    QTabWidget *m_workspaceTabs = nullptr;
    QTreeWidget *m_fileTree = nullptr;
    QLineEdit *m_fileFilter = nullptr;
    QLineEdit *m_workspaceSearchInput = nullptr;
    QComboBox *m_workspaceSearchMode = nullptr;
    QCheckBox *m_workspaceSearchCase = nullptr;
    QPushButton *m_workspaceSearchButton = nullptr;
    QPushButton *m_workspaceSearchCancelButton = nullptr;
    QPushButton *m_workspaceSearchMoreButton = nullptr;
    QTreeWidget *m_workspaceSearchResults = nullptr;
    QLabel *m_workspaceSearchStatus = nullptr;
    QComboBox *m_repositoryMapBudget = nullptr;
    QPushButton *m_repositoryRefreshButton = nullptr;
    QPushButton *m_repositoryCancelButton = nullptr;
    QTabWidget *m_repositoryViews = nullptr;
    QTreeWidget *m_repositorySymbols = nullptr;
    QTreeWidget *m_repositoryDependencies = nullptr;
    QTreeWidget *m_languageResults = nullptr;
    QTreeWidget *m_languageDiagnostics = nullptr;
    QPlainTextEdit *m_diagnosticRawPreview = nullptr;
    QPlainTextEdit *m_repositoryMapPreview = nullptr;
    QLabel *m_repositoryStatus = nullptr;
    QLabel *m_languageStatus = nullptr;
    QPushButton *m_languageDefinitionButton = nullptr;
    QPushButton *m_languageReferencesButton = nullptr;
    QPushButton *m_languageDiagnosticsButton = nullptr;
    QPushButton *m_languageStopButton = nullptr;
    QPushButton *m_languageRawButton = nullptr;
    QPlainTextEdit *m_editor = nullptr;
#ifdef AEGISY_HAS_MONACO
    QStackedWidget *m_editorStack = nullptr;
    QWebEngineView *m_monacoView = nullptr;
    QWebEnginePage *m_monacoPage = nullptr;
    QWebEngineProfile *m_monacoProfile = nullptr;
    MonacoEditorBridge *m_monacoBridge = nullptr;
#endif
    QTabBar *m_editorTabs = nullptr;
    QComboBox *m_recentFilePicker = nullptr;
    QWidget *m_editorSearchBar = nullptr;
    QLineEdit *m_editorFind = nullptr;
    QLineEdit *m_editorReplace = nullptr;
    QCheckBox *m_editorCaseSensitive = nullptr;
    QLabel *m_editorSearchStatus = nullptr;
    QLabel *m_editorPath = nullptr;
    QLabel *m_editorMeta = nullptr;
    QPushButton *m_editorSaveButton = nullptr;
    QPushButton *m_editorReloadButton = nullptr;
    QPushButton *m_editorSplitButton = nullptr;
    QPushButton *m_editorContextButton = nullptr;
    QPlainTextEdit *m_terminalExcerptPreview = nullptr;
    QLabel *m_workspaceEditSummary = nullptr;
    QTreeWidget *m_workspaceEditFiles = nullptr;
    QPlainTextEdit *m_workspaceEditDiff = nullptr;
    QPushButton *m_workspaceEditMoreButton = nullptr;
    QComboBox *m_terminalPicker = nullptr;
    QLabel *m_terminalStatus = nullptr;
    QPushButton *m_terminalNewButton = nullptr;
    QPushButton *m_terminalStopButton = nullptr;
    QPushButton *m_terminalRestartButton = nullptr;
    QPushButton *m_terminalRemoveButton = nullptr;
    QPushButton *m_terminalContextButton = nullptr;
    QAction *m_terminalSelectionContextAction = nullptr;
    QAction *m_pinTerminalExcerptAction = nullptr;
    QLabel *m_gitSummary = nullptr;
    QString m_gitCurrentBranch;
    QTreeWidget *m_gitHistory = nullptr;
    QComboBox *m_gitDiffScope = nullptr;
    QPushButton *m_gitRefreshButton = nullptr;
    QPushButton *m_gitPinDiffButton = nullptr;
    QPushButton *m_gitPinCommitButton = nullptr;
    QPlainTextEdit *m_gitDiffPreview = nullptr;
    QLabel *m_fileStatus = nullptr;
    QTimer *m_workspaceWatchTimer = nullptr;
    QTimer *m_gitStatusTimer = nullptr;
    QTimer *m_terminalPollTimer = nullptr;
    QTimer *m_runtimeHealthTimer = nullptr;
    QHash<QString, QLabel *> m_itemLabels;
    QHash<QString, QPushButton *> m_itemArtifactButtons;
    QHash<QString, QPushButton *> m_itemProposalButtons;
    QHash<QString, QLabel *> m_itemProposalStatusLabels;
    QHash<QString, QJsonObject> m_itemProposalReferences;
    QHash<QString, QString> m_itemKinds;
    QHash<QString, QString> m_itemRoles;
    QHash<QString, QString> m_itemStates;
    QHash<QString, quint64> m_itemRevisions;
    QHash<QString, QString> m_turnStates;
    QHash<QString, quint64> m_unknownTimelineEventCounts;
    quint64 m_unknownTimelineEventOverflowCount = 0;
    QHash<QString, TimelineSessionState> m_timelineSessions;
    QHash<QString, QJsonObject> m_durableMutationOperations;
    QHash<QString, QSet<QString>> m_durableMutationSessionOperations;
    QHash<QString, DurableMutationListRequest> m_mutationListRequests;
    QHash<QString, DurableMutationConsumeRequest> m_mutationConsumeRequests;
    QHash<QString, QString> m_mutationSessionListRequests;
    QHash<QString, int> m_mutationConsumeRecoveryAttempts;
    QSet<QString> m_mutationReconciliationSessionIds;
    qsizetype m_timelineTrackedEventCount = 0;
    qsizetype m_timelinePendingEventCount = 0;
    qsizetype m_timelinePendingBytes = 0;
    bool m_timelineTrackingExhausted = false;
    bool m_timelineSyncAvailable = false;
    bool m_timelineSnapshotAvailable = false;
    bool m_timelineSubscriptionAvailable = false;
    bool m_mutationAcknowledgementAvailable = false;
    std::function<QString(const QString &, const QString &, const QJsonObject &,
                          const QJsonObject &, int)> m_timelineSnapshotRequester;
    std::function<QString(quint64, const QString &, const QString &, const QString &,
                          const QJsonObject &, const QJsonObject &, const QString &)>
        m_timelineSubscriptionActivationRequester;
    std::function<void(const QString &)> m_timelineSubscriptionConnectionAbandoner;
    std::function<void(const QJsonObject &, const QJsonObject &, bool)> m_timelinePresenter;
    QHash<QString, QString> m_commandArtifactRequests;
    QHash<QString, QTreeWidgetItem *> m_treeItems;
    QHash<QString, QString> m_workspaceListRequests;
    QHash<QString, QString> m_workspaceReadRequests;
    QHash<QString, QString> m_workspaceMetadataRequests;
    QHash<QString, QString> m_workspaceMetadataMessages;
    QHash<QString, QString> m_gitStatuses;
    QSet<QString> m_watchedDirectories;
    QSet<QString> m_archivedSessionIds;
    QSet<QString> m_recoverySessionIds;
    QSet<QString> m_pendingDeletionSessionIds;
    struct EditorBuffer {
        QString content;
        QString revision;
        QString encoding;
        QString newline;
        QString metadata;
        int cursorPosition = 0;
        int anchorPosition = 0;
        int verticalScroll = 0;
        int horizontalScroll = 0;
        bool saveSupported = false;
        bool modified = false;
        bool conflict = false;
        bool fallback = false;
    };
    struct EditorViewState {
        int cursorPosition = 0;
        int anchorPosition = 0;
        int verticalScroll = 0;
        int horizontalScroll = 0;
    };
    QHash<QString, EditorBuffer> m_editorBuffers;
    QHash<QString, EditorViewState> m_restoredEditorViews;
    QStringList m_recentFiles;
    QSet<QString> m_editorRestoreRequests;
    QString m_editorRestoreActivePath;
    QString m_editorRestoreGroupPaths[2];
    QString m_mode = QStringLiteral("chat");
    QString m_projectId;
    QString m_projectRoot;
    QString m_workspaceWatchId;
    QString m_workspaceSearchRequestId;
    QString m_workspaceSearchId;
    QString m_workspaceSearchCursor;
    QString m_repositoryIndexRequestId;
    QString m_repositoryIndexCancelRequestId;
    QString m_repositoryIndexId;
    QString m_repositoryMapRequestId;
    QString m_repositoryIndexSummary;
    QString m_languageServersRequestId;
    QString m_languageRequestId;
    QString m_diagnosticRawRequestId;
    QString m_diagnosticRawReference;
    QString m_workspaceEditRequestId;
    QString m_workspaceEditArtifactRequestId;
    QString m_workspaceEditId;
    QString m_workspaceEditReference;
    struct WorkspaceEditProposalRequest {
        QString sessionId;
        QString proposalId;
        quint64 generation = 0;
        QString itemKey;
        QJsonObject reference;
    };
    QHash<QString, WorkspaceEditProposalRequest> m_workspaceEditProposalRequests;
    QHash<QString, quint64> m_workspaceEditProposalGenerations;
    quint64 m_workspaceEditProposalReferenceGeneration = 0;
    QString m_workspaceEditReferenceSelectionSessionId;
    QString m_workspaceEditReferenceSelectionProposalId;
    QString m_workspaceEditDisplayedReferenceSessionId;
    QString m_workspaceEditDisplayedReferenceProposalId;
    QHash<QString, QJsonObject> m_confirmedWorkspaceEditProposals;
    QStringList m_workspaceEditProposalRecency;
    QSet<QString> m_unreadWorkspaceEditProposalSessions;
    QSet<QString> m_unverifiedWorkspaceEditProposalSessions;
    struct WorkspaceEditProposalArtifactRequest {
        QString sessionId;
        QString proposalId;
        QString projectId;
        QString editId;
        QString reference;
        QString sha256;
        qint64 bytes = 0;
        qint64 offset = 0;
        quint64 generation = 0;
        QByteArray accumulatedBytes;
    };
    QHash<QString, WorkspaceEditProposalArtifactRequest>
        m_workspaceEditProposalArtifactRequests;
    QByteArray m_workspaceEditProposalArtifactBytes;
    quint64 m_workspaceEditProposalArtifactGeneration = 0;
    QString m_workspaceEditProposalSessionId;
    QString m_workspaceEditProposalId;
    QString m_workspaceEditExpectedArtifactSha256;
    qint64 m_workspaceEditExpectedArtifactBytes = 0;
    bool m_workspaceEditDurableProposal = false;
    QString m_gitOverviewRequestId;
    QString m_gitLogRequestId;
    QString m_gitDiffRequestId;
    QString m_gitDiffRequestedScope;
    QString m_gitDiffRequestedOid;
    QString m_gitContextRequestId;
    QString m_gitContextRequestedKind;
    QString m_gitContextRequestedScope;
    QString m_gitContextRequestedOid;
    QString m_gitContextRequestedLabel;
    QString m_selectedGitOid;
    QString m_pendingSearchPath;
    QString m_openEditorPath;
    QString m_editorRevision;
    QString m_editorEncoding;
    QString m_editorNewline;
    QString m_editorSaveRequestId;
    QString m_directoryStatus;
    QString m_sessionListRequestId;
    QString m_sessionReadRequestId;
    QString m_operationStatusRequestId;
    QString m_operationStatusSessionId;
    QString m_operationProbeRequestId;
    QString m_operationReconcileRequestId;
    QJsonObject m_operationReview;
    QString m_compactionRequestId;
    QString m_compactionSessionId;
    QString m_compactionOperation;
    QString m_backgroundNotificationRequestId;
    QString m_backgroundNotificationSessionId;
    QJsonObject m_backgroundNotificationCursor;
    QDialog *m_backgroundNotificationDialog = nullptr;
    QTableWidget *m_backgroundNotificationTable = nullptr;
    QPushButton *m_backgroundNotificationMoreButton = nullptr;
    QString m_backgroundRecoveryRequestId;
    QString m_backgroundRecoverySessionId;
    QJsonObject m_backgroundRecoveryCursor;
    QDialog *m_backgroundRecoveryDialog = nullptr;
    QTableWidget *m_backgroundRecoveryTable = nullptr;
    QPushButton *m_backgroundRecoveryMoreButton = nullptr;
    QString m_sessionMutationRequestId;
    QString m_sessionDeletionRequestId;
    QString m_portableSessionRequestId;
    QString m_portableSessionOperation;
    QString m_portableSessionId;
    QString m_portableSessionPath;
    QString m_portableCollisionStrategy;
    QString m_portableTargetProjectId;
    QJsonObject m_portableSessionPackage;
    QString m_retentionPolicyRequestId;
    QString m_retentionScopeKind;
    QString m_retentionScopeId;
    QString m_retentionScopeLabel;
    QString m_sessionHistoryId;
    QString m_sessionHistoryCursor;
    quint64 m_sessionHistoryFirstSequence = 0;
    quint64 m_sessionHistoryLatestSequence = 0;
    QString m_sessionReadSessionId;
    QString m_sessionReadCursor;
    int m_sessionReadLimit = 0;
    quint64 m_sessionReadExpectedFirstSequence = 0;
    quint64 m_sessionReadExpectedLatestSequence = 0;
    QString m_chatSessionId;
    QString m_chatSessionProjectId;
    QString m_workSessionId;
    QString m_activeTerminalId;
    QString m_terminalAttachRequestId;
    QString m_terminalListRequestId;
    QString m_terminalExcerptRequestId;
    QString m_pendingTerminalKind;
    QString m_pendingTerminalName;
    QString m_terminalSelection;
    QString m_pendingPrompt;
    QString m_pendingTurnIdempotencyKey;
    QString m_activeTurnSessionId;
    QString m_activeTurnId;
    QHash<QString, quint64> m_lastTimelineEventSequences;
    QHash<QString, quint64> m_lastTimelineEventTimestamps;
    QString m_turnCancelRequestId;
    QString m_contextInspectRequestId;
    QString m_pinnedContextListRequestId;
    QString m_pinnedContextMutationRequestId;
    QString m_pinnedFileReadRequestId;
    QString m_pinnedImageImportRequestId;
    QString m_pinnedImagePreviewRequestId;
    QString m_pinnedImagePreviewReference;
    QJsonObject m_pendingPinnedSelection;
    QJsonObject m_pendingPinnedDiagnostic;
    QString m_pendingPinnedIncludeId;
    QString m_pendingPinnedDiagnosticRequestId;
    QString m_pinnedContextSetIdentity;
    QJsonArray m_pendingContext;
    QString m_pendingPinnedContextSetIdentity;
    QStringList m_pendingPinnedContextIds;
    QHash<QString, QJsonObject> m_sessionRuntimeBindings;
    QHash<QString, QJsonObject> m_sessionContextThresholds;
    QStringList m_sessionContextThresholdRecency;
    QHash<QString, QJsonObject> m_sessionWorkspaceBindings;
    QList<QJsonObject> m_contextItems;
    QList<QJsonObject> m_pinnedContextItems;
    bool m_gitStatusPending = false;
    bool m_sessionListRefreshPending = false;
    bool m_sessionHistoryAppending = false;
    bool m_runtimeRecoveryMode = false;
    bool m_emergencyDisabled = false;
    bool m_emergencyPolicyVerified = false;
    bool m_runtimeReconnectActive = false;
    bool m_runtimeReconnectExhausted = false;
    bool m_runtimeStateUnverified = false;
    bool m_activeTurnControlUnverified = false;
    quint64 m_runtimeReconnectRecoveryGeneration = 0;
    QSet<QString> m_runtimeReconnectTimelinePending;
    QSet<QString> m_runtimeReconnectMutationSessions;
    QHash<QString, QString> m_runtimeReconnectProposalRequests;
    QString m_runtimeReconnectSessionReadId;
    QString m_runtimeReconnectTerminalListId;
    QString m_runtimeReconnectTerminalAttachId;
    QString m_runtimeReconnectTerminalId;
    quint64 m_runtimeReconnectTerminalGeneration = 0;
    bool m_runtimeReconnectSecondPhaseStarted = false;
    bool m_runtimeReconnectMutationRecoveryStarted = false;
    bool m_terminalStateUnverified = false;
    bool m_operationStatusKnown = true;
    bool m_operationStatusBlocked = false;
    bool m_compactionAvailable = false;
    bool m_backgroundNotificationInspectionAvailable = false;
    bool m_backgroundRecoveryInspectionAvailable = false;
    bool m_pinnedContextAvailable = false;
    bool m_imageContextAvailable = false;
    bool m_gitContextAvailable = false;
    bool m_workspaceEditProposalAvailable = false;
    enum class RuntimeDegradationState {
        NotRequested,
        Pending,
        Valid,
        Invalid,
    };
    RuntimeDegradationState m_runtimeDegradationState =
        RuntimeDegradationState::NotRequested;
    QString m_runtimeDegradationRequestId;
    QHash<QString, QString> m_runtimeDegradationStates;
    quint64 m_startupRebuiltSessionCount = 0;
    quint64 m_quarantinedSessionCount = 0;
    bool m_workspaceSearchAppending = false;
    bool m_workspaceSearchStale = false;
    bool m_repositoryIndexLoaded = false;
    bool m_repositoryIndexStale = false;
    bool m_languageResultsStale = false;
    int m_structureWorkspaceTab = -1;
    int m_editorWorkspaceTab = -1;
    int m_terminalWorkspaceTab = -1;
    int m_workspaceEditTab = -1;
    int m_gitWorkspaceTab = -1;
    int m_languageResultsView = -1;
    int m_languageDiagnosticsView = -1;
    int m_diagnosticRawView = -1;
    int m_pendingSearchLine = 0;
    int m_pendingSearchColumn = 0;
    quint64 m_workspaceSearchSequence = 0;
    quint64 m_repositoryIndexSequence = 0;
    quint64 m_terminalOutputOffset = 0;
    quint64 m_terminalGeneration = 0;
    qint64 m_workspaceEditOffset = 0;
    int m_sessionHistoryScrollValue = 0;
    int m_sessionHistoryScrollMaximum = 0;
    bool m_terminalRunning = false;
    bool m_terminalStopping = false;
    bool m_turnRunning = false;
    bool m_turnCancelling = false;
    bool m_runtimeRestartRequired = false;
    bool m_terminalWebReady = false;
    int m_terminalRenderRestartAttempts = 0;
    bool m_editorSaveSupported = false;
    bool m_editorConflict = false;
    bool m_editorLoading = false;
    bool m_switchingEditorTab = false;
    bool m_editorSplitEnabled = false;
    bool m_editorRestoreSplitEnabled = false;
    int m_activeEditorGroup = 0;
    int m_editorRestoreActiveGroup = 0;
    QString m_editorGroupPaths[2];
#ifdef AEGISY_HAS_MONACO
    bool m_monacoReady = false;
    bool m_monacoContentWithinLimit = true;
    QStackedWidget *m_terminalStack = nullptr;
    QWebEngineView *m_terminalView = nullptr;
    QWebEnginePage *m_terminalPage = nullptr;
    QWebEngineProfile *m_terminalProfile = nullptr;
    TerminalWebBridge *m_terminalBridge = nullptr;
#endif
};

#endif
