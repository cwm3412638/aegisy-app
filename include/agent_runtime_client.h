#ifndef AGENT_RUNTIME_CLIENT_H
#define AGENT_RUNTIME_CLIENT_H

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>

class QProcess;
class QTimer;

class AgentRuntimeClient : public QObject
{
    Q_OBJECT

public:
    explicit AgentRuntimeClient(QObject *parent = nullptr);
    ~AgentRuntimeClient() override;

    bool isReady() const;
    bool isRecoveryMode() const;
    QString runtimePath() const;

    // The sidecar must not inherit credential-bearing desktop environment values.
    static QProcessEnvironment sanitizedSidecarEnvironment(
        const QProcessEnvironment &environment);

    void start();
    void stop();
    QString runtimeHealth();
    QString runtimeDegradations();
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
                      const QJsonArray &context = {});
    QString cancelTurn(const QString &sessionId, const QString &turnId);
    QString readSession(const QString &sessionId, const QString &cursor = QString(),
                        int limit = 100);
    QString projectionRecoveryStatus();
    QString sessionRecoveryStatus(const QString &sessionId);
    QString runtimeRecoveryStatus();
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
    QString watchWorkspace(const QString &projectId, const QStringList &paths,
                           const QString &watchId = QString(),
                           const QString &rootId = QString());
    QString pollWorkspaceWatch(const QString &watchId);
    QString openUserTerminal(const QString &sessionId, const QString &kind,
                             const QString &name, int rows = 24, int cols = 80);
    QString listTerminals(const QString &sessionId);
    QString attachTerminal(const QString &sessionId, const QString &terminalId,
                           quint64 after = 0);
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

signals:
    void connectionStateChanged(bool ready, const QString &detail);
    void runtimeInitialized(const QJsonObject &result);
    void runtimeHealthRead(const QJsonObject &health);
    void runtimeDegradationsRead(const QString &requestId, const QJsonObject &result);
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
    void projectionRecoveryStatusRead(const QJsonObject &status);
    void sessionRecoveryStatusRead(const QJsonObject &status);
    void runtimeRecoveryStatusRead(const QJsonObject &status);
    void timelineEvent(const QJsonObject &event);
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
    void workspaceWatchConfigured(const QString &requestId, const QJsonObject &watch);
    void workspaceChanged(const QString &requestId, const QJsonObject &result);
    void terminalOpened(const QString &requestId, const QJsonObject &terminal);
    void terminalsListed(const QString &requestId, const QJsonObject &result);
    void terminalAttached(const QString &requestId, const QJsonObject &terminal);
    void terminalInputAccepted(const QString &requestId, const QJsonObject &result);
    void terminalResized(const QString &requestId, const QJsonObject &result);
    void terminalSignalled(const QString &requestId, const QJsonObject &result);
    void terminalStopped(const QString &requestId, const QJsonObject &terminal);
    void terminalRestarted(const QString &requestId, const QJsonObject &terminal);
    void terminalRemoved(const QString &requestId, const QJsonObject &result);
    void commandArtifactRead(const QString &requestId, const QJsonObject &artifact);
    void requestFailed(const QString &requestId, const QString &method,
                       const QString &message, int code);
    void diagnosticMessage(const QString &message);

private:
    QString locateRuntime() const;
    QString sendRequest(const QString &method, const QJsonObject &params = {});
    void sendNotification(const QString &method, const QJsonObject &params = {});
    void writeMessage(const QJsonObject &message);
    void processStdout();
    void processMessage(const QJsonObject &message);
    void failPending(const QString &message);

    QProcess *m_process = nullptr;
    QTimer *m_startupTimer = nullptr;
    QByteArray m_stdoutBuffer;
    QHash<QString, QString> m_pendingMethods;
    quint64 m_nextRequestId = 0;
    quint64 m_nextTurnKey = 0;
    bool m_ready = false;
    bool m_recoveryMode = false;
    bool m_stopping = false;
    QString m_runtimePath;
};

#endif
