#ifndef AGENT_RUNTIME_CLIENT_H
#define AGENT_RUNTIME_CLIENT_H

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QProcessEnvironment>
#include <QSet>
#include <QStringList>

#include "aap_transport_types_generated.h"

#include <variant>

class QProcess;
class QLocalSocket;
class QTimer;

class AgentRuntimeClient : public QObject
{
    Q_OBJECT

public:
    enum class TransportMode {
        Stdio,
        VerifiedUnixSocket,
    };

    enum class ReconnectState {
        Idle,
        Waiting,
        Restarting,
        Exhausted,
    };
    Q_ENUM(ReconnectState)

    explicit AgentRuntimeClient(QObject *parent = nullptr,
                                int heartbeatIntervalMs = 5000,
                                int heartbeatDeadlineMs = 15000,
                                QList<int> reconnectBackoffMs = {
                                    0, 500, 2000,
                                },
                                TransportMode transportMode = TransportMode::Stdio);
    ~AgentRuntimeClient() override;

    bool isReady() const;
    bool isHeartbeatHealthy() const;
    bool isControlAvailable() const;
    bool isReconnectRecoveryAvailable() const;
    bool isRecoveryMode() const;
    ReconnectState reconnectState() const;
    int reconnectAttempt() const;
    int maximumReconnectAttempts() const;
    quint64 processGeneration() const;
    QString runtimePath() const;
    bool emergencyDisabled() const { return m_emergencyDisabled; }

    // The sidecar must not inherit credential-bearing desktop environment values.
    static QProcessEnvironment sanitizedSidecarEnvironment(
        const QProcessEnvironment &environment);
    static QString timelineEventIdentity(const QJsonObject &event);
    static QString timelineSnapshotItemIdentity(const QString &sessionId,
                                                const QJsonObject &itemPage);
    static quint64 timelineSnapshotItemCanonicalBytes(const QString &sessionId,
                                                      const QJsonObject &itemPage);
    static QString timelineSnapshotIdentity(const QString &sessionId,
                                            const QJsonObject &floor,
                                            const QJsonObject &watermark,
                                            const QJsonObject &activeTurn,
                                            quint64 totalItems,
                                            quint64 totalCanonicalBytes,
                                            const QStringList &orderedItemIdentities);
    static QString timelineSnapshotPageIdentity(const QJsonObject &page);
    static QString timelineSubscriptionRequestIdentity(const QString &stage,
                                                       const QJsonObject &request);
    static QString durableMutationOperationIdentity(const QString &sessionId,
                                                    const QString &mutationKind,
                                                    const QString &idempotencyKey,
                                                    const QString &requestFingerprint);
    static QString workspaceEditProposalPreviewIdentity(const QJsonObject &proposal);
    static QString workspaceEditProposalArtifactPageIdentity(const QJsonObject &page);
    static QString commandArtifactPageBindingIdentity(const QJsonObject &result);
    static QString contentPreviewIdentity(const QJsonObject &preview);
    static QString contentInlineLimitsIdentity(const QJsonObject &limits);
    static QString contentReferenceCursorIdentity(const QJsonObject &cursor);
    static QString contentReferencePageIdentity(const QJsonObject &page);
    static bool isValidCommandArtifactPage(const QJsonObject &result,
                                           const QJsonObject &request);
    static bool isValidCompleteCommandArtifact(const QJsonObject &result,
                                               const QByteArray &content);

    void start();
    void stop();
    void setEmergencyDisabled(bool disabled);
    void abandonTimelineSubscriptionConnection(const QString &detail);
    bool completeReconnectRecovery(quint64 generation, bool success,
                                   const QString &detail = QString());
    QString runtimeHealth();
    QString runtimeDegradations();
    QString modelCatalog();
    QString modelCatalogCache();
    QString modelCatalogRefreshStatus();
    QString checkModelCapabilities(const QString &modelId,
                                   const QJsonObject &requirements);
    QString listModelProfiles(const QString &projectId = QString());
    QString readModelProfile(const QString &profileId);
    QString restartRuntime();
    QString listProjects(int limit = 50);
    QString updateProjectNavigation(const QString &projectId, bool pinned);
    QString openProject(const QString &root);
    QString relinkProject(const QString &projectId, const QString &rootId,
                          const QString &root, const QString &expectedRootIdentity);
    QString previewProjectTrustReview(const QString &root);
    QString acknowledgeProjectTrustReview(const QString &projectId, const QString &rootId,
                                           const QString &rootIdentity,
                                           const QString &reviewId);
    QString listProjectRoots(const QString &projectId);
    QString addProjectRoot(const QString &projectId, const QString &root,
                           const QString &access);
    QString removeProjectRoot(const QString &projectId, const QString &rootId);
    QString startSession(const QString &mode, const QString &projectId = QString());
    QString resumeSession(const QString &sessionId);
    QString forkSession(const QString &sessionId, const QString &lastTurnId = QString(),
                        const QString &title = QString());
    QString listSessions(const QString &projectId = QString(),
                         const QString &mode = QString(),
                         bool includeArchived = false, int limit = 50);
    QString searchSessions(const QString &query, const QString &projectId = QString(),
                           bool includeArchived = true, int limit = 100);
    QString renameSession(const QString &sessionId, const QString &title);
    QString archiveSession(const QString &sessionId);
    QString unarchiveSession(const QString &sessionId);
    QString previewSessionDeletion(const QString &sessionId, const QString &scope);
    QString scheduleSessionDeletion(const QString &sessionId, const QString &scope,
                                    const QJsonObject &planHash, qint64 undoWindowMs);
    QString undoSessionDeletion(const QString &deletionId);
    QString sessionDeletionStatus(const QString &sessionId);
    QString previewPortableSessionExport(const QString &sessionId);
    QString exportPortableSession(const QString &sessionId,
                                  const QJsonObject &packageHash);
    QString previewPortableSessionImport(const QJsonObject &package,
                                         const QString &targetProjectId,
                                         const QString &collisionStrategy);
    QString importPortableSession(const QJsonObject &package,
                                  const QString &targetProjectId,
                                  const QString &collisionStrategy);
    QString readRetentionPolicy(const QString &scopeKind, const QString &scopeId);
    QString setRetentionPolicy(const QJsonObject &policy);
    QString removeRetentionPolicy(const QString &scopeKind, const QString &scopeId);
    QString runRetentionMaintenance();
    QString startTurn(const QString &sessionId, const QString &input,
                      const QJsonArray &context = {},
                      const QString &pinnedContextSetIdentity = QString(),
                      const QStringList &pinnedContextIds = {},
                      const QString &idempotencyKey = QString());
    QString inspectTurnContext(const QString &sessionId,
                               const QJsonArray &context = {},
                               const QString &pinnedContextSetIdentity = QString(),
                               const QStringList &pinnedContextIds = {});
    QString cancelTurn(const QString &sessionId, const QString &turnId);
    QString readSession(const QString &sessionId, const QString &cursor = QString(),
                        int limit = 100);
    QString syncTimeline(const QString &sessionId, quint64 afterSequence,
                         const QString &afterEventId = QString(),
                         const QJsonObject &watermark = QJsonObject(),
                         int limit = 100);
    QString timelineSnapshot(const QString &sessionId,
                             const QString &snapshotIdentity = QString(),
                             const QJsonObject &watermark = QJsonObject(),
                             const QJsonObject &after = QJsonObject(),
                             int limit = 100);
    QString subscribeTimeline(const QString &sessionId, quint64 connectionGeneration,
                              quint64 cursorSequence,
                              const QString &cursorEventId = QString(),
                              const QJsonObject &watermark = QJsonObject(),
                              QString *subscriptionId = nullptr);
    QString syncTimelineSubscription(quint64 connectionGeneration,
                                     const QString &sessionId,
                                     const QString &subscriptionId,
                                     quint64 afterSequence,
                                     const QString &afterEventId,
                                     const QJsonObject &watermark,
                                     int limit = 100);
    QString snapshotTimelineSubscription(quint64 connectionGeneration,
                                         const QString &sessionId,
                                         const QString &subscriptionId,
                                         const QJsonObject &subscriptionCursor,
                                         const QString &snapshotIdentity = QString(),
                                         const QJsonObject &watermark = QJsonObject(),
                                         const QJsonObject &after = QJsonObject(),
                                         int limit = 100);
    QString activateTimelineSubscription(quint64 connectionGeneration,
                                         const QString &sessionId,
                                         const QString &subscriptionId,
                                         const QString &source,
                                         const QJsonObject &cursor,
                                         const QJsonObject &watermark,
                                         const QString &snapshotIdentity = QString());
    QString listMutationAcknowledgements(const QString &sessionId,
                                         const QJsonObject &after = QJsonObject(),
                                         int limit = 100);
    QString consumeMutationAcknowledgement(const QString &sessionId,
                                            const QString &operationIdentity,
                                            quint64 expectedRevision,
                                            const QString &target,
                                            const QJsonObject &confirmedAnchor);
    QString backgroundNotifications(const QString &sessionId,
                                    const QJsonObject &cursor = QJsonObject(),
                                    int limit = 100);
    QString backgroundRecovery(const QString &sessionId,
                               const QJsonObject &cursor = QJsonObject(),
                               int limit = 100);
    QString projectionRecoveryStatus();
    QString sessionRecoveryStatus(const QString &sessionId);
    QString operationStatus(const QString &sessionId);
    QString operationProbe(const QJsonObject &params);
    QString operationReconcile(const QJsonObject &params);
    QString createCompactionCheckpoint(const QString &sessionId, const QString &checkpointId,
                                       const QString &preservationInstructions,
                                       const QJsonObject &summary);
    QString readCompactionCheckpoint(const QString &sessionId, const QString &checkpointId);
    QString reviseCompactionCheckpoint(const QString &sessionId,
                                       const QString &sourceCheckpointId,
                                       const QString &sourceReviewId,
                                       const QString &checkpointId,
                                       const QString &preservationInstructions,
                                       const QJsonObject &summary);
    QString runtimeRecoveryStatus();
    QString listPinnedContext(const QString &projectId);
    QString savePinnedContext(const QString &projectId, const QJsonArray &items,
                              const QString &expectedSetIdentity = QString());
    QString removePinnedContext(const QString &projectId, const QString &itemId,
                                const QString &expectedSetIdentity = QString());
    QString importPinnedImage(const QString &sessionId, const QString &rootId,
                              const QString &label, const QString &mediaType,
                              const QByteArray &content);
    QString readPinnedImage(const QString &sessionId, const QString &reference);
    QString listWorkspace(const QString &projectId, const QString &path = QString(),
                          const QString &rootId = QString());
    QString readWorkspaceFile(const QString &projectId, const QString &path,
                              const QString &rootId = QString());
    QString saveWorkspaceFile(const QString &projectId, const QString &path,
                              const QString &content, const QString &expectedRevision,
                              const QString &encoding, const QString &newline,
                              const QString &rootId = QString());
    QString workspaceMetadata(const QString &projectId, const QString &path,
                              const QString &rootId = QString());
    QString workspaceGitStatus(const QString &projectId);
    QString gitOverview(const QString &projectId);
    QString gitLog(const QString &projectId, int limit = 50,
                   const QString &cursor = QString());
    QString gitCommit(const QString &projectId, const QString &oid);
    QString gitDiff(const QString &projectId, const QString &scope,
                    const QString &oid = QString(), const QString &path = QString());
    QString gitContext(const QString &projectId, const QString &kind,
                       const QString &scope = QString(), const QString &oid = QString());
    QString searchWorkspace(const QString &projectId, const QString &searchId,
                            const QString &query, const QString &mode,
                            bool caseSensitive, const QString &cursor = QString(),
                            int limit = 50,
                            const QString &rootId = QString());
    QString cancelWorkspaceSearch(const QString &searchId,
                                  const QString &projectId = QString(),
                                  const QString &rootId = QString());
    QString indexWorkspace(const QString &projectId, const QString &indexId = QString(),
                           const QString &rootId = QString());
    QString cancelWorkspaceIndex(const QString &projectId, const QString &indexId,
                                 const QString &rootId = QString());
    QString repositoryMap(const QString &projectId, int tokenBudget,
                          const QStringList &focusPaths = {},
                          const QString &rootId = QString());
    QString languageServers(const QString &projectId, const QString &rootId = QString());
    QString startLanguageServer(const QString &projectId, const QString &path,
                                const QString &rootId = QString());
    QString stopLanguageServer(const QString &projectId, const QString &path,
                               const QString &rootId = QString());
    QString workspaceDefinition(const QString &projectId, const QString &path,
                                const QString &content, const QString &revision,
                                int line, int column,
                                const QString &rootId = QString());
    QString workspaceReferences(const QString &projectId, const QString &path,
                                const QString &content, const QString &revision,
                                int line, int column,
                                const QString &rootId = QString());
    QString workspaceDiagnostics(const QString &projectId, const QString &path,
                                 const QString &content, const QString &revision,
                                 const QString &rootId = QString());
    QString observedDiagnostics(const QString &projectId, const QString &path = QString(),
                                bool includeStale = true,
                                const QString &rootId = QString());
    QString diagnosticRaw(const QString &projectId, const QString &reference,
                          const QString &rootId = QString());
    QString previewWorkspaceEdit(const QString &sessionId, const QJsonObject &edit,
                                 const QJsonArray &contents);
    QString readWorkspaceEditArtifact(const QString &sessionId, const QString &projectId,
                                      const QString &editId, const QString &reference,
                                      qint64 offset = 0, int limit = 64 * 1024);
    QString latestWorkspaceEditProposal(const QString &sessionId);
    QString readWorkspaceEditProposal(const QString &sessionId, const QString &proposalId);
    QString readWorkspaceEditProposalArtifact(const QString &sessionId,
                                              const QString &proposalId,
                                              const QString &reference,
                                              qint64 offset = 0,
                                              int limit = 64 * 1024);
    QString watchWorkspace(const QString &projectId, const QStringList &paths,
                           const QString &watchId = QString(),
                           const QString &rootId = QString());
    QString pollWorkspaceWatch(const QString &watchId);
    QString openUserTerminal(const QString &sessionId, const QString &kind,
                             const QString &name, int rows = 24, int cols = 80);
    QString listTerminals(const QString &sessionId);
    QString attachTerminal(const QString &sessionId, const QString &terminalId,
                           quint64 after = 0);
    QString readTerminalExcerpt(const QString &sessionId, const QString &terminalId,
                                int maxBytes = 16 * 1024);
    QString inputUserTerminal(const QString &sessionId, const QString &terminalId,
                              const QByteArray &data);
    QString resizeTerminal(const QString &sessionId, const QString &terminalId,
                           int rows, int cols);
    QString signalUserTerminal(const QString &sessionId, const QString &terminalId,
                               const QString &signal);
    QString stopUserTerminal(const QString &sessionId, const QString &terminalId);
    QString restartUserTerminal(const QString &sessionId, const QString &terminalId,
                                int rows = 0, int cols = 0);
    QString removeUserTerminal(const QString &sessionId, const QString &terminalId);
    QString readCommandArtifact(const QString &sessionId, const QString &reference);
    QString readCommandArtifactPage(const QString &sessionId, const QString &itemId,
                                    const QString &reference,
                                    const QJsonObject &cursor = {});

signals:
    void connectionStateChanged(bool ready, const QString &detail);
    void runtimeLivenessChanged(bool healthy, const QString &detail);
    void runtimeReconnectStateChanged(AgentRuntimeClient::ReconnectState state,
                                      int attempt, int maximumAttempts,
                                      int nextDelayMs, const QString &detail);
    void reconnectHandshakeReady(quint64 generation, const QJsonObject &result);
    void heartbeatRecoveryExhausted(int attempts, const QString &detail);
    void runtimeInitialized(const QJsonObject &result);
    void runtimeHealthRead(const QJsonObject &health);
    void runtimeDegradationRequestCreated(const QString &requestId);
    void runtimeDegradationsRead(const QString &requestId, const QJsonObject &result);
    void modelCatalogRead(const QString &requestId, const QJsonObject &result);
    void modelCatalogCacheRead(const QString &requestId, const QJsonObject &result);
    void modelCatalogRefreshStatusRead(const QString &requestId, const QJsonObject &result);
    void modelCapabilityChecked(const QString &requestId, const QJsonObject &result);
    void modelProfilesListed(const QString &requestId, const QJsonObject &result);
    void modelProfileRead(const QString &requestId, const QJsonObject &result);
    void runtimeRestarted(const QString &requestId, const QJsonObject &result);
    void projectsListed(const QString &requestId, const QJsonObject &result);
    void projectNavigationChanged(const QString &requestId, const QJsonObject &result);
    void projectOpened(const QString &requestId, const QJsonObject &project);
    void projectRelinkRequired(const QString &requestId, const QJsonObject &project,
                               const QJsonObject &identity);
    void projectTrustReviewRequired(const QString &requestId,
                                    const QJsonObject &project,
                                    const QJsonObject &review);
    void projectTrustAcknowledged(const QString &requestId, const QJsonObject &result);
    void projectRootsListed(const QString &requestId, const QJsonObject &result);
    void projectRootChanged(const QString &requestId, const QString &method,
                            const QJsonObject &result);
    void sessionStarted(const QString &requestId, const QJsonObject &session);
    void sessionResumed(const QString &requestId, const QJsonObject &result);
    void sessionForked(const QString &requestId, const QJsonObject &result);
    void sessionsListed(const QString &requestId, const QJsonObject &result);
    void sessionChanged(const QString &requestId, const QString &method,
                        const QJsonObject &result);
    void sessionDeletionPreviewed(const QString &requestId, const QJsonObject &preview);
    void sessionDeletionChanged(const QString &requestId, const QString &method,
                                const QJsonObject &result);
    void sessionDeletionStatusRead(const QString &requestId, const QJsonObject &status);
    void portableSessionExportPreviewed(const QString &requestId,
                                        const QJsonObject &preview);
    void portableSessionExported(const QString &requestId, const QJsonObject &result);
    void portableSessionImportPreviewed(const QString &requestId,
                                        const QJsonObject &preview);
    void portableSessionImported(const QString &requestId, const QJsonObject &result);
    void retentionPolicyRead(const QString &requestId, const QJsonObject &result);
    void retentionPolicyChanged(const QString &requestId, const QString &method,
                                const QJsonObject &result);
    void retentionMaintenanceCompleted(const QString &requestId, const QJsonObject &result);
    void sessionRead(const QString &requestId, const QJsonObject &snapshot);
    void timelineSynced(const QString &requestId, const QJsonObject &page);
    void timelineSnapshotReceived(const QString &requestId, const QJsonObject &page);
    void timelineSubscribed(const QString &requestId, const QJsonObject &result);
    void timelineSubscriptionSynced(const QString &requestId, const QJsonObject &page);
    void timelineSubscriptionSnapshotReceived(const QString &requestId,
                                              const QJsonObject &page);
    void timelineSubscriptionActivated(const QString &requestId,
                                       const QJsonObject &result);
    void timelineSubscriptionEvent(const QJsonObject &event);
    void timelineSubscriptionFailed(const QString &requestId,
                                    const QJsonObject &failure);
    void turnStarted(const QString &requestId, quint64 processGeneration,
                     const QJsonObject &result);
    void mutationAcknowledgementsListed(const QString &requestId,
                                        const QJsonObject &page);
    void mutationAcknowledgementConsumed(const QString &requestId,
                                         const QJsonObject &result);
    void backgroundNotificationsRead(const QString &requestId, const QJsonObject &result);
    void backgroundRecoveryRead(const QString &requestId, const QJsonObject &result);
    void projectionRecoveryStatusRead(const QJsonObject &status);
    void sessionRecoveryStatusRead(const QJsonObject &status);
    void operationStatusRead(const QString &requestId, const QJsonObject &status);
    void operationProbeRead(const QString &requestId, const QJsonObject &result);
    void operationReconciled(const QString &requestId, const QJsonObject &result);
    void compactionCheckpointCreated(const QString &requestId, const QJsonObject &result);
    void compactionCheckpointRead(const QString &requestId, const QJsonObject &result);
    void compactionCheckpointRevised(const QString &requestId, const QJsonObject &result);
    void runtimeRecoveryStatusRead(const QJsonObject &status);
    void timelineEvent(const QJsonObject &event);
    void turnContextInspected(const QString &requestId, const QJsonObject &result);
    void pinnedContextListed(const QString &requestId, const QJsonObject &result);
    void pinnedContextChanged(const QString &requestId, const QString &method,
                              const QJsonObject &result);
    void pinnedImageImported(const QString &requestId, const QJsonObject &result);
    void pinnedImageRead(const QString &requestId, const QJsonObject &result);
    void turnCancellationRequested(const QString &requestId, const QJsonObject &result);
    void workspaceListed(const QString &requestId, const QJsonObject &listing);
    void workspaceFileRead(const QString &requestId, const QJsonObject &file);
    void workspaceFileSaved(const QString &requestId, const QJsonObject &file);
    void workspaceMetadataRead(const QString &requestId, const QJsonObject &metadata);
    void workspaceGitStatusRead(const QString &requestId, const QJsonObject &status);
    void gitOverviewRead(const QString &requestId, const QJsonObject &overview);
    void gitLogRead(const QString &requestId, const QJsonObject &log);
    void gitCommitRead(const QString &requestId, const QJsonObject &commit);
    void gitDiffRead(const QString &requestId, const QJsonObject &diff);
    void gitContextRead(const QString &requestId, const QJsonObject &context);
    void workspaceSearchCompleted(const QString &requestId, const QJsonObject &result);
    void workspaceSearchCancelled(const QString &requestId, const QJsonObject &result);
    void workspaceIndexed(const QString &requestId, const QJsonObject &result);
    void workspaceIndexCancelled(const QString &requestId, const QJsonObject &result);
    void repositoryMapRead(const QString &requestId, const QJsonObject &result);
    void languageServersRead(const QString &requestId, const QJsonObject &result);
    void languageServerStarted(const QString &requestId, const QJsonObject &result);
    void languageServerStopped(const QString &requestId, const QJsonObject &result);
    void workspaceDefinitionsRead(const QString &requestId, const QJsonObject &result);
    void workspaceReferencesRead(const QString &requestId, const QJsonObject &result);
    void workspaceDiagnosticsRead(const QString &requestId, const QJsonObject &result);
    void observedDiagnosticsRead(const QString &requestId, const QJsonObject &result);
    void diagnosticRawRead(const QString &requestId, const QJsonObject &result);
    void workspaceEditPreviewed(const QString &requestId, const QJsonObject &preview);
    void workspaceEditArtifactRead(const QString &requestId, const QJsonObject &page);
    void workspaceEditProposalLatestRead(const QString &requestId,
                                         const QJsonObject &result);
    void workspaceEditProposalRead(const QString &requestId, const QJsonObject &result);
    void workspaceEditProposalArtifactRead(const QString &requestId,
                                           const QJsonObject &page);
    void workspaceWatchConfigured(const QString &requestId, const QJsonObject &watch);
    void workspaceChanged(const QString &requestId, const QJsonObject &result);
    void terminalOpened(const QString &requestId, const QJsonObject &terminal);
    void terminalsListed(const QString &requestId, const QJsonObject &result);
    void terminalAttached(const QString &requestId, const QJsonObject &terminal);
    void terminalExcerptRead(const QString &requestId, const QJsonObject &excerpt);
    void terminalInputAccepted(const QString &requestId, const QJsonObject &result);
    void terminalResized(const QString &requestId, const QJsonObject &result);
    void terminalSignalled(const QString &requestId, const QJsonObject &result);
    void terminalStopped(const QString &requestId, const QJsonObject &terminal);
    void terminalRestarted(const QString &requestId, const QJsonObject &terminal);
    void terminalRemoved(const QString &requestId, const QJsonObject &result);
    void commandArtifactRead(const QString &requestId, const QJsonObject &artifact);
    void commandArtifactPageRead(const QString &requestId, const QJsonObject &page);
    void timelineRetentionGap(const QString &requestId, const QJsonObject &data);
    // Compatibility signal: emitted only when the canonical wire code fits int.
    void requestFailed(const QString &requestId, const QString &method,
                       const QString &message, int code);
    // Lossless error-code contract for production consumers.
    void requestFailedExact(const QString &requestId, const QString &method,
                            const QString &message, const QString &canonicalCode);
    void diagnosticMessage(const QString &message);

private:
    struct TimelineSyncValidation {
        QJsonObject request;
    };
    struct TimelineSubscriptionValidation {
        QJsonObject request;
        QJsonObject subscriptionCursor;
    };
    struct TurnStartValidation {
        QString sessionId;
        QString idempotencyKey;
        quint64 generation = 0;
    };
    struct MutationListValidation {
        QJsonObject request;
    };
    struct MutationConsumeValidation {
        QJsonObject request;
    };
    struct CommandArtifactPageValidation {
        QJsonObject request;
    };
    using PendingValidation = std::variant<
        std::monostate,
        TimelineSyncValidation,
        TimelineSubscriptionValidation,
        TurnStartValidation,
        MutationListValidation,
        MutationConsumeValidation,
        CommandArtifactPageValidation>;
    struct PendingRequestContext {
        aegisy::aap::transport_generated::TransportPendingRequest transport;
        quint64 processGeneration = 0;
        PendingValidation validation;
    };

    QString locateRuntime() const;
    QString sendRequest(const QString &method, const QJsonObject &params = {},
                        PendingValidation validation = {});
    bool sendNotification(const QString &method, const QJsonObject &params = {});
    int writeMessage(const QJsonObject &message);
    void processStdout();
    void processSocketInput();
    void processTransportBytes(const QByteArray &bytes);
    bool usesVerifiedUnixSocket() const;
    bool prepareUnixSocketEndpoint();
    void cleanupUnixSocketEndpoint();
    void scheduleUnixSocketConnect(quint64 generation);
    void connectUnixSocket(quint64 generation);
    bool verifyUnixSocketPeer() const;
    void handleUnixSocketDisconnected();
    void terminateOwnedProcessGeneration(quint64 generation);
    void sendInitializeRequest();
    void closeTransportWrite();
    QJsonObject expectedTransportSecurity() const;
    void processMessage(
        const aegisy::aap::transport_generated::TransportMessage &message);
    void acceptInitializeResponse(const QJsonObject &result);
    void rejectInitializeResponse(const QString &reasonCode);
    void rejectProtocolMessage(const QString &reasonCode);
    void reportRequestFailure(const QString &requestId, const QString &method,
                              const QString &message, int code);
    void clearNegotiationState();
    void failPending(const QString &message);
    void failOrdinaryPending(const QString &message);
    void abandonAmbiguousTimelineSubscriptionConnection(const QString &detail);
    void sendHeartbeat(bool recoveryProbe = false);
    void handleHeartbeatTimeout();
    void scheduleHeartbeatRecoveryProbe();
    bool launchProcess(bool reconnectAttempt);
    void handleRetryableProcessFailure(const QString &detail);
    void scheduleReconnect(const QString &detail);
    void beginReconnectAttempt();
    void suppressAutomaticReconnect();
    void setReconnectState(ReconnectState state, int nextDelayMs,
                           const QString &detail);
    void retireResponseId(const QString &requestId);
    void removePendingRequest(const QString &requestId);
    static bool emergencyRequestAllowed(const QString &method);

    QProcess *m_process = nullptr;
    QLocalSocket *m_localSocket = nullptr;
    TransportMode m_transportMode = TransportMode::Stdio;
    QTimer *m_startupTimer = nullptr;
    QTimer *m_heartbeatIntervalTimer = nullptr;
    QTimer *m_heartbeatDeadlineTimer = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    QTimer *m_reconnectStabilityTimer = nullptr;
    QByteArray m_stdoutBuffer;
    QString m_unixSocketDirectory;
    QString m_unixSocketPath;
    quint64 m_unixSocketDirectoryDevice = 0;
    quint64 m_unixSocketDirectoryInode = 0;
    quint64 m_unixSocketDevice = 0;
    quint64 m_unixSocketInode = 0;
    bool m_unixSocketIdentityCaptured = false;
    int m_unixSocketCleanupRetryCount = 0;
    QHash<QString, PendingRequestContext> m_pendingRequests;
    QSet<QString> m_retiredResponseIds;
    QStringList m_retiredResponseOrder;
    quint64 m_nextRequestId = 0;
    quint64 m_nextTimelineSubscriptionId = 0;
    quint64 m_processGeneration = 0;
    quint64 m_initializeGeneration = 0;
    quint64 m_startupGeneration = 0;
    quint64 m_heartbeatGeneration = 0;
    quint64 m_heartbeatDeadlineGeneration = 0;
    quint64 m_reconnectScheduledGeneration = 0;
    quint64 m_reconnectTerminationGeneration = 0;
    quint64 m_reconnectStabilityGeneration = 0;
    quint64 m_nextHeartbeatNonce = 0;
    QString m_initializeRequestId;
    QString m_heartbeatRequestId;
    QString m_heartbeatDeadlineRequestId;
    QString m_heartbeatNonce;
    QSet<QString> m_negotiatedStableCapabilities;
    int m_negotiatedMaximumFrameBytes = 0;
    QList<int> m_reconnectBackoffMs;
    int m_reconnectAttempt = 0;
    int m_heartbeatRecoveryAttempt = 0;
    ReconnectState m_reconnectState = ReconnectState::Idle;
    quint64 m_unixSocketConnectGeneration = 0;
    quint64 m_unixSocketDisconnectGeneration = 0;
    quint64 m_unixSocketPeerVerifiedGeneration = 0;
    quint64 m_ownedTerminationGeneration = 0;
    bool m_ready = false;
    bool m_heartbeatNegotiated = false;
    bool m_heartbeatHealthy = false;
    bool m_recoveryMode = false;
    bool m_handshakeComplete = false;
    bool m_stopping = false;
    bool m_autoReconnectSuppressed = false;
    bool m_reconnectCycleActive = false;
    bool m_reconnectTerminationPending = false;
    bool m_processTerminationPending = false;
    bool m_reconnectRecoveryPending = false;
    bool m_discardProcessOutput = false;
    bool m_emergencyDisabled = false;
    bool m_processStartedEmergency = false;
    bool m_policyRestartPending = false;
    QJsonObject m_reconnectInitializeResult;
    QString m_runtimePath;
};

#endif
