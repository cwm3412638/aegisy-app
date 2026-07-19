#include "agent_runtime_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

namespace {
constexpr int kStartupTimeoutMs = 5000;
constexpr int kMaximumFrameBytes = 8 * 1024 * 1024;

bool sensitiveSidecarEnvironmentName(const QString &name)
{
    const QString upper = name.toUpper();
    if (upper == QStringLiteral("HTTP_PROXY")
        || upper == QStringLiteral("HTTPS_PROXY")
        || upper == QStringLiteral("ALL_PROXY")
        || upper == QStringLiteral("SSH_AUTH_SOCK")
        || upper == QStringLiteral("GPG_AGENT_INFO")
        || upper == QStringLiteral("GOOGLE_APPLICATION_CREDENTIALS")) {
        return true;
    }
    QString normalized;
    normalized.reserve(upper.size());
    for (const QChar character : upper) {
        if (character.isLetterOrNumber()) normalized.append(character);
    }
    if (normalized.contains(QStringLiteral("APIKEY"))
        || normalized.contains(QStringLiteral("PRIVATEKEY"))
        || normalized.contains(QStringLiteral("ACCESSKEY"))
        || normalized.contains(QStringLiteral("SECRETKEY"))) {
        return true;
    }
    const QStringList parts = upper.split(QRegularExpression(QStringLiteral("[^A-Z0-9]+")),
                                          Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        if (part == QStringLiteral("TOKEN")
            || part == QStringLiteral("SECRET")
            || part == QStringLiteral("PASSWORD")
            || part == QStringLiteral("PASSWD")
            || part == QStringLiteral("CREDENTIAL")
            || part == QStringLiteral("CREDENTIALS")
            || part == QStringLiteral("COOKIE")
            || part == QStringLiteral("AUTHORIZATION")
            || part == QStringLiteral("JWT")) {
            return true;
        }
    }
    return false;
}
}

AgentRuntimeClient::AgentRuntimeClient(QObject *parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_startupTimer(new QTimer(this))
{
    m_startupTimer->setSingleShot(true);
    connect(m_startupTimer, &QTimer::timeout, this, [this]() {
        if (!m_ready && m_process->state() != QProcess::NotRunning) {
            emit connectionStateChanged(false, QStringLiteral("运行时握手超时"));
            m_process->kill();
        }
    });
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &AgentRuntimeClient::processStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        const QString output = QString::fromUtf8(m_process->readAllStandardError()).trimmed();
        if (!output.isEmpty()) emit diagnosticMessage(output);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_ready = false;
        m_recoveryMode = false;
        const QString detail = QStringLiteral("运行时启动失败：%1").arg(m_process->errorString());
        failPending(detail);
        emit connectionStateChanged(false, detail);
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
        m_startupTimer->stop();
        const bool expected = m_stopping;
        m_stopping = false;
        m_ready = false;
        m_recoveryMode = false;
        const QString detail = expected
            ? QStringLiteral("运行时已停止")
            : QStringLiteral("运行时已退出（%1，代码 %2）")
                  .arg(status == QProcess::CrashExit ? QStringLiteral("异常")
                                                      : QStringLiteral("正常"))
                  .arg(exitCode);
        failPending(detail);
        emit connectionStateChanged(false, detail);
    });
}

AgentRuntimeClient::~AgentRuntimeClient()
{
    stop();
    if (m_process->state() != QProcess::NotRunning) {
        m_process->waitForFinished(500);
    }
}

bool AgentRuntimeClient::isReady() const
{
    return m_ready;
}

bool AgentRuntimeClient::isRecoveryMode() const
{
    return m_recoveryMode;
}

QString AgentRuntimeClient::runtimePath() const
{
    return m_runtimePath;
}

QProcessEnvironment AgentRuntimeClient::sanitizedSidecarEnvironment(
    const QProcessEnvironment &environment)
{
    QProcessEnvironment sanitized;
    for (const QString &name : environment.keys()) {
        if (!sensitiveSidecarEnvironmentName(name)) {
            sanitized.insert(name, environment.value(name));
        }
    }
    return sanitized;
}

void AgentRuntimeClient::start()
{
    if (m_process->state() != QProcess::NotRunning) return;
    m_runtimePath = locateRuntime();
    if (m_runtimePath.isEmpty()) {
        emit connectionStateChanged(
            false, QStringLiteral("未找到 aegisy-agentd，请先构建 agent-runtime"));
        return;
    }

    m_ready = false;
    m_recoveryMode = false;
    m_stopping = false;
    m_stdoutBuffer.clear();
    m_pendingMethods.clear();
    emit connectionStateChanged(false, QStringLiteral("正在连接本地运行时…"));
    QProcessEnvironment environment = sanitizedSidecarEnvironment(
        QProcessEnvironment::systemEnvironment());
    if (environment.value(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT")).isEmpty()) {
        const QString dataRoot = QDir(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .absoluteFilePath(QStringLiteral("workbench"));
        if (!QDir().mkpath(dataRoot)) {
            emit connectionStateChanged(false, QStringLiteral("无法创建工作台数据目录"));
            return;
        }
        environment.insert(QStringLiteral("AEGISY_WORKBENCH_DATA_ROOT"), dataRoot);
    }
    m_process->setProgram(m_runtimePath);
    m_process->setArguments({});
    m_process->setProcessEnvironment(environment);
    m_process->start();
    if (!m_process->waitForStarted(1000)) return;
    m_startupTimer->start(kStartupTimeoutMs);

    QJsonObject client;
    client.insert(QStringLiteral("name"), QStringLiteral("aegisy-client"));
    client.insert(QStringLiteral("version"), QStringLiteral(AEGISY_APP_VERSION));
    QJsonObject params;
    params.insert(QStringLiteral("protocol_version"), QStringLiteral("0.1"));
    params.insert(QStringLiteral("client"), client);
    sendRequest(QStringLiteral("initialize"), params);
}

void AgentRuntimeClient::stop()
{
    if (m_process->state() == QProcess::NotRunning) return;
    m_stopping = true;
    if (m_ready) sendRequest(QStringLiteral("shutdown"));
    else m_process->terminate();
}

QString AgentRuntimeClient::runtimeHealth()
{
    return sendRequest(QStringLiteral("runtime/health"));
}

QString AgentRuntimeClient::runtimeDegradations()
{
    return sendRequest(QStringLiteral("runtime/degradations"));
}

QString AgentRuntimeClient::restartRuntime()
{
    return sendRequest(QStringLiteral("runtime/restart"));
}

QString AgentRuntimeClient::listProjects(int limit)
{
    return sendRequest(QStringLiteral("project/list"), {
        {QStringLiteral("limit"), limit},
    });
}

QString AgentRuntimeClient::updateProjectNavigation(const QString &projectId, bool pinned)
{
    return sendRequest(QStringLiteral("project/navigation"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("pinned"), pinned},
    });
}

QString AgentRuntimeClient::openProject(const QString &root)
{
    return sendRequest(QStringLiteral("project/open"),
                       {{QStringLiteral("root"), root}});
}

QString AgentRuntimeClient::relinkProject(const QString &projectId, const QString &rootId,
                                          const QString &root,
                                          const QString &expectedRootIdentity)
{
    return sendRequest(QStringLiteral("project/relink"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("root_id"), rootId},
        {QStringLiteral("root"), root},
        {QStringLiteral("expected_root_identity"), expectedRootIdentity},
    });
}

QString AgentRuntimeClient::previewProjectTrustReview(const QString &root)
{
    return sendRequest(QStringLiteral("project/trust-review"),
                       {{QStringLiteral("root"), root}});
}

QString AgentRuntimeClient::acknowledgeProjectTrustReview(
    const QString &projectId, const QString &rootId, const QString &rootIdentity,
    const QString &reviewId)
{
    return sendRequest(QStringLiteral("project/trust-acknowledge"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("root_id"), rootId},
        {QStringLiteral("root_identity"), rootIdentity},
        {QStringLiteral("review_id"), reviewId},
    });
}

QString AgentRuntimeClient::listProjectRoots(const QString &projectId)
{
    return sendRequest(QStringLiteral("project/root-list"),
                       {{QStringLiteral("project_id"), projectId}});
}

QString AgentRuntimeClient::addProjectRoot(const QString &projectId,
                                           const QString &root,
                                           const QString &access)
{
    return sendRequest(QStringLiteral("project/root-add"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("root"), root},
        {QStringLiteral("access"), access},
    });
}

QString AgentRuntimeClient::removeProjectRoot(const QString &projectId,
                                              const QString &rootId)
{
    return sendRequest(QStringLiteral("project/root-remove"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("root_id"), rootId},
    });
}

QString AgentRuntimeClient::startSession(const QString &mode, const QString &projectId)
{
    QJsonObject params{{QStringLiteral("mode"), mode},
                       {QStringLiteral("title"), mode == QStringLiteral("work")
                            ? QStringLiteral("Project work") : QStringLiteral("New chat")}};
    if (!projectId.isEmpty()) params.insert(QStringLiteral("project_id"), projectId);
    return sendRequest(QStringLiteral("session/start"), params);
}

QString AgentRuntimeClient::resumeSession(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/resume"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::forkSession(const QString &sessionId,
                                        const QString &lastTurnId,
                                        const QString &title)
{
    QJsonObject params{{QStringLiteral("session_id"), sessionId}};
    if (!lastTurnId.isEmpty()) params.insert(QStringLiteral("last_turn_id"), lastTurnId);
    if (!title.isEmpty()) params.insert(QStringLiteral("title"), title);
    return sendRequest(QStringLiteral("session/fork"), params);
}

QString AgentRuntimeClient::listSessions(const QString &projectId, const QString &mode,
                                         bool includeArchived, int limit)
{
    QJsonObject params{
        {QStringLiteral("include_archived"), includeArchived},
        {QStringLiteral("limit"), qBound(1, limit, 200)},
    };
    if (!projectId.isEmpty()) params.insert(QStringLiteral("project_id"), projectId);
    if (!mode.isEmpty()) params.insert(QStringLiteral("mode"), mode);
    return sendRequest(QStringLiteral("session/list"), params);
}

QString AgentRuntimeClient::searchSessions(const QString &query, const QString &projectId,
                                           bool includeArchived, int limit)
{
    QJsonObject params{
        {QStringLiteral("text"), query},
        {QStringLiteral("include_archived"), includeArchived},
        {QStringLiteral("limit"), qBound(1, limit, 100)},
    };
    if (!projectId.isEmpty()) params.insert(QStringLiteral("project_id"), projectId);
    return sendRequest(QStringLiteral("session/search"), params);
}

QString AgentRuntimeClient::renameSession(const QString &sessionId, const QString &title)
{
    return sendRequest(QStringLiteral("session/title"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("title"), title},
    });
}

QString AgentRuntimeClient::archiveSession(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/archive"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::unarchiveSession(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/unarchive"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::previewSessionDeletion(const QString &sessionId,
                                                    const QString &scope)
{
    return sendRequest(QStringLiteral("session/delete/preview"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("scope"), scope},
    });
}

QString AgentRuntimeClient::scheduleSessionDeletion(const QString &sessionId,
                                                     const QString &scope,
                                                     const QJsonObject &planHash,
                                                     qint64 undoWindowMs)
{
    return sendRequest(QStringLiteral("session/delete/schedule"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("scope"), scope},
        {QStringLiteral("plan_hash"), planHash},
        {QStringLiteral("undo_window_ms"), undoWindowMs},
    });
}

QString AgentRuntimeClient::undoSessionDeletion(const QString &deletionId)
{
    return sendRequest(QStringLiteral("session/delete/undo"), {
        {QStringLiteral("deletion_id"), deletionId},
    });
}

QString AgentRuntimeClient::sessionDeletionStatus(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/deletion/status"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::previewPortableSessionExport(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/export/preview"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::exportPortableSession(const QString &sessionId,
                                                  const QJsonObject &packageHash)
{
    return sendRequest(QStringLiteral("session/export"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("package_hash"), packageHash},
    });
}

QString AgentRuntimeClient::previewPortableSessionImport(
    const QJsonObject &package, const QString &targetProjectId,
    const QString &collisionStrategy)
{
    QJsonObject params{
        {QStringLiteral("package"), package},
        {QStringLiteral("collision_strategy"), collisionStrategy},
    };
    if (!targetProjectId.isEmpty()) {
        params.insert(QStringLiteral("target_project_id"), targetProjectId);
    }
    return sendRequest(QStringLiteral("session/import/preview"), params);
}

QString AgentRuntimeClient::importPortableSession(const QJsonObject &package,
                                                  const QString &targetProjectId,
                                                  const QString &collisionStrategy)
{
    QJsonObject params{
        {QStringLiteral("package"), package},
        {QStringLiteral("collision_strategy"), collisionStrategy},
    };
    if (!targetProjectId.isEmpty()) {
        params.insert(QStringLiteral("target_project_id"), targetProjectId);
    }
    return sendRequest(QStringLiteral("session/import"), params);
}

QString AgentRuntimeClient::readRetentionPolicy(const QString &scopeKind,
                                                const QString &scopeId)
{
    return sendRequest(QStringLiteral("retention/policy/read"), {
        {QStringLiteral("scope_kind"), scopeKind},
        {QStringLiteral("scope_id"), scopeId},
    });
}

QString AgentRuntimeClient::setRetentionPolicy(const QJsonObject &policy)
{
    return sendRequest(QStringLiteral("retention/policy/set"), policy);
}

QString AgentRuntimeClient::removeRetentionPolicy(const QString &scopeKind,
                                                   const QString &scopeId)
{
    return sendRequest(QStringLiteral("retention/policy/remove"), {
        {QStringLiteral("scope_kind"), scopeKind},
        {QStringLiteral("scope_id"), scopeId},
    });
}

QString AgentRuntimeClient::runRetentionMaintenance()
{
    return sendRequest(QStringLiteral("retention/maintenance/run"));
}

QString AgentRuntimeClient::startTurn(const QString &sessionId, const QString &input,
                                      const QJsonArray &context,
                                      const QString &pinnedContextSetIdentity,
                                      const QStringList &pinnedContextIds)
{
    ++m_nextTurnKey;
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("input"), input},
        {QStringLiteral("context"), context},
        {QStringLiteral("idempotency_key"),
         QStringLiteral("qt-turn-%1").arg(m_nextTurnKey)},
    };
    if (!pinnedContextIds.isEmpty()) {
        params.insert(QStringLiteral("pinned_context_set_identity"),
                      pinnedContextSetIdentity);
        params.insert(QStringLiteral("pinned_context_ids"),
                      QJsonArray::fromStringList(pinnedContextIds));
    }
    return sendRequest(QStringLiteral("turn/start"), params);
}

QString AgentRuntimeClient::inspectTurnContext(const QString &sessionId,
                                                const QJsonArray &context,
                                                const QString &pinnedContextSetIdentity,
                                                const QStringList &pinnedContextIds)
{
    if (sessionId.isEmpty()) return {};
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("context"), context},
    };
    if (!pinnedContextIds.isEmpty()) {
        params.insert(QStringLiteral("pinned_context_set_identity"),
                      pinnedContextSetIdentity);
        params.insert(QStringLiteral("pinned_context_ids"),
                      QJsonArray::fromStringList(pinnedContextIds));
    }
    return sendRequest(QStringLiteral("turn/context/inspect"), params);
}

QString AgentRuntimeClient::listPinnedContext(const QString &projectId)
{
    if (projectId.isEmpty()) return {};
    return sendRequest(QStringLiteral("workspace/pinned-context/list"), {
        {QStringLiteral("project_id"), projectId},
    });
}

QString AgentRuntimeClient::savePinnedContext(const QString &projectId,
                                               const QJsonArray &items,
                                               const QString &expectedSetIdentity)
{
    if (projectId.isEmpty()) return {};
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("set"), QJsonObject{
            {QStringLiteral("schema_version"), QStringLiteral("pinned-context/0.1")},
            {QStringLiteral("project_id"), projectId},
            {QStringLiteral("items"), items},
        }},
    };
    if (!expectedSetIdentity.isEmpty()) {
        params.insert(QStringLiteral("expected_set_identity"), expectedSetIdentity);
    }
    return sendRequest(QStringLiteral("workspace/pinned-context/save"), params);
}

QString AgentRuntimeClient::removePinnedContext(const QString &projectId,
                                                 const QString &itemId,
                                                 const QString &expectedSetIdentity)
{
    if (projectId.isEmpty() || itemId.isEmpty()) return {};
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("item_id"), itemId},
    };
    if (!expectedSetIdentity.isEmpty()) {
        params.insert(QStringLiteral("expected_set_identity"), expectedSetIdentity);
    }
    return sendRequest(QStringLiteral("workspace/pinned-context/remove"), params);
}

QString AgentRuntimeClient::cancelTurn(const QString &sessionId, const QString &turnId)
{
    if (sessionId.isEmpty() || turnId.isEmpty()) return {};
    return sendRequest(QStringLiteral("turn/cancel"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("turn_id"), turnId},
    });
}

QString AgentRuntimeClient::readSession(const QString &sessionId, const QString &cursor,
                                        int limit)
{
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("limit"), qBound(1, limit, 200)},
    };
    if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
    return sendRequest(QStringLiteral("session/read"), params);
}

QString AgentRuntimeClient::projectionRecoveryStatus()
{
    return sendRequest(QStringLiteral("runtime/projection-recovery/status"));
}

QString AgentRuntimeClient::sessionRecoveryStatus(const QString &sessionId)
{
    return sendRequest(QStringLiteral("session/recovery/status"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::operationStatus(const QString &sessionId)
{
    if (sessionId.isEmpty()) return {};
    return sendRequest(QStringLiteral("operation/status"), {
        {QStringLiteral("session_id"), sessionId},
    });
}

QString AgentRuntimeClient::operationProbe(const QJsonObject &params)
{
    return sendRequest(QStringLiteral("operation/probe"), params);
}

QString AgentRuntimeClient::operationReconcile(const QJsonObject &params)
{
    return sendRequest(QStringLiteral("operation/reconcile"), params);
}

QString AgentRuntimeClient::createCompactionCheckpoint(
    const QString &sessionId, const QString &checkpointId,
    const QString &preservationInstructions, const QJsonObject &summary)
{
    if (sessionId.isEmpty() || checkpointId.isEmpty()) return {};
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("checkpoint_id"), checkpointId},
        {QStringLiteral("summary"), summary},
    };
    if (!preservationInstructions.isEmpty()) {
        params.insert(QStringLiteral("preservation_instructions"), preservationInstructions);
    }
    return sendRequest(QStringLiteral("session/compaction/checkpoint/create"), params);
}

QString AgentRuntimeClient::readCompactionCheckpoint(const QString &sessionId,
                                                     const QString &checkpointId)
{
    if (sessionId.isEmpty() || checkpointId.isEmpty()) return {};
    return sendRequest(QStringLiteral("session/compaction/checkpoint/read"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("checkpoint_id"), checkpointId},
    });
}

QString AgentRuntimeClient::reviseCompactionCheckpoint(
    const QString &sessionId, const QString &sourceCheckpointId,
    const QString &sourceReviewId, const QString &checkpointId,
    const QString &preservationInstructions, const QJsonObject &summary)
{
    if (sessionId.isEmpty() || sourceCheckpointId.isEmpty() || sourceReviewId.isEmpty()
            || checkpointId.isEmpty() || sourceCheckpointId == checkpointId) {
        return {};
    }
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("source_checkpoint_id"), sourceCheckpointId},
        {QStringLiteral("source_review_id"), sourceReviewId},
        {QStringLiteral("checkpoint_id"), checkpointId},
        {QStringLiteral("summary"), summary},
    };
    if (!preservationInstructions.isEmpty()) {
        params.insert(QStringLiteral("preservation_instructions"), preservationInstructions);
    }
    return sendRequest(QStringLiteral("session/compaction/checkpoint/revise"), params);
}

QString AgentRuntimeClient::runtimeRecoveryStatus()
{
    return sendRequest(QStringLiteral("runtime/recovery/status"));
}

QString AgentRuntimeClient::listWorkspace(const QString &projectId, const QString &path,
                                          const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/list"), params);
}

QString AgentRuntimeClient::readWorkspaceFile(const QString &projectId, const QString &path,
                                              const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/read"), params);
}

QString AgentRuntimeClient::saveWorkspaceFile(const QString &projectId, const QString &path,
                                              const QString &content,
                                              const QString &expectedRevision,
                                              const QString &encoding,
                                              const QString &newline,
                                              const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), content},
        {QStringLiteral("expected_revision"), expectedRevision},
        {QStringLiteral("encoding"), encoding},
        {QStringLiteral("newline"), newline},
        {QStringLiteral("origin"), QStringLiteral("user")},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/save-user-text"), params);
}

QString AgentRuntimeClient::workspaceMetadata(const QString &projectId, const QString &path,
                                              const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/metadata"), params);
}

QString AgentRuntimeClient::workspaceGitStatus(const QString &projectId)
{
    return sendRequest(QStringLiteral("workspace/git-status"),
                       {{QStringLiteral("project_id"), projectId}});
}

QString AgentRuntimeClient::gitOverview(const QString &projectId)
{
    return sendRequest(QStringLiteral("workspace/git/overview"),
                       {{QStringLiteral("project_id"), projectId}});
}

QString AgentRuntimeClient::gitLog(const QString &projectId, int limit,
                                   const QString &cursor)
{
    QJsonObject params{{QStringLiteral("project_id"), projectId},
                       {QStringLiteral("limit"), limit}};
    if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
    return sendRequest(QStringLiteral("workspace/git/log"), params);
}

QString AgentRuntimeClient::gitCommit(const QString &projectId, const QString &oid)
{
    return sendRequest(QStringLiteral("workspace/git/commit"), {
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("oid"), oid},
    });
}

QString AgentRuntimeClient::gitDiff(const QString &projectId, const QString &scope,
                                    const QString &oid, const QString &path)
{
    QJsonObject params{{QStringLiteral("project_id"), projectId},
                       {QStringLiteral("scope"), scope}};
    if (!oid.isEmpty()) params.insert(QStringLiteral("oid"), oid);
    if (!path.isEmpty()) params.insert(QStringLiteral("path"), path);
    return sendRequest(QStringLiteral("workspace/git/diff"), params);
}

QString AgentRuntimeClient::searchWorkspace(const QString &projectId,
                                            const QString &searchId,
                                            const QString &query,
                                            const QString &mode,
                                            bool caseSensitive,
                                            const QString &cursor,
                                            int limit,
                                            const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("search_id"), searchId},
        {QStringLiteral("query"), query},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("case_sensitive"), caseSensitive},
        {QStringLiteral("limit"), limit},
    };
    if (!cursor.isEmpty()) params.insert(QStringLiteral("cursor"), cursor);
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/search"), params);
}

QString AgentRuntimeClient::cancelWorkspaceSearch(const QString &searchId,
                                                  const QString &projectId,
                                                  const QString &rootId)
{
    QJsonObject params{{QStringLiteral("search_id"), searchId}};
    if (!projectId.isEmpty()) params.insert(QStringLiteral("project_id"), projectId);
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/search/cancel"), params);
}

QString AgentRuntimeClient::indexWorkspace(const QString &projectId, const QString &indexId,
                                           const QString &rootId)
{
    QJsonObject params{{QStringLiteral("project_id"), projectId}};
    if (!indexId.isEmpty()) params.insert(QStringLiteral("index_id"), indexId);
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/index"), params);
}

QString AgentRuntimeClient::cancelWorkspaceIndex(const QString &projectId,
                                                 const QString &indexId,
                                                 const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("index_id"), indexId},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/index/cancel"), params);
}

QString AgentRuntimeClient::repositoryMap(const QString &projectId, int tokenBudget,
                                          const QStringList &focusPaths,
                                          const QString &rootId)
{
    QJsonArray paths;
    for (const QString &path : focusPaths) paths.append(path);
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("token_budget"), tokenBudget},
        {QStringLiteral("focus_paths"), paths},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/repository-map"), params);
}

QString AgentRuntimeClient::languageServers(const QString &projectId, const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/language-servers"), params);
}

QString AgentRuntimeClient::startLanguageServer(const QString &projectId,
                                                const QString &path,
                                                const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/language-server/start"), params);
}

QString AgentRuntimeClient::stopLanguageServer(const QString &projectId,
                                               const QString &path,
                                               const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/language-server/stop"), params);
}

QString AgentRuntimeClient::workspaceDefinition(const QString &projectId,
                                                const QString &path,
                                                const QString &content,
                                                const QString &revision,
                                                int line, int column,
                                                const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), content},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("line"), line},
        {QStringLiteral("column"), column},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/definition"), params);
}

QString AgentRuntimeClient::workspaceReferences(const QString &projectId,
                                                const QString &path,
                                                const QString &content,
                                                const QString &revision,
                                                int line, int column,
                                                const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), content},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("line"), line},
        {QStringLiteral("column"), column},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/references"), params);
}

QString AgentRuntimeClient::workspaceDiagnostics(const QString &projectId,
                                                 const QString &path,
                                                 const QString &content,
                                                 const QString &revision,
                                                 const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("path"), path},
        {QStringLiteral("content"), content},
        {QStringLiteral("revision"), revision},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/diagnostics"), params);
}

QString AgentRuntimeClient::observedDiagnostics(const QString &projectId,
                                                const QString &path,
                                                bool includeStale,
                                                const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("include_stale"), includeStale},
    };
    if (!path.isEmpty()) params.insert(QStringLiteral("path"), path);
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/observed-diagnostics"), params);
}

QString AgentRuntimeClient::diagnosticRaw(const QString &projectId,
                                          const QString &reference,
                                          const QString &rootId)
{
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("reference"), reference},
    };
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/diagnostics/raw"), params);
}

QString AgentRuntimeClient::previewWorkspaceEdit(const QString &sessionId,
                                                 const QJsonObject &edit,
                                                 const QJsonArray &contents)
{
    return sendRequest(QStringLiteral("workspace/edit/preview"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("edit"), edit},
        {QStringLiteral("contents"), contents},
    });
}

QString AgentRuntimeClient::readWorkspaceEditArtifact(const QString &sessionId,
                                                      const QString &projectId,
                                                      const QString &editId,
                                                      const QString &reference,
                                                      qint64 offset, int limit)
{
    return sendRequest(QStringLiteral("workspace/edit/artifact/read"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("edit_id"), editId},
        {QStringLiteral("reference"), reference},
        {QStringLiteral("offset"), offset},
        {QStringLiteral("limit"), limit},
    });
}

QString AgentRuntimeClient::watchWorkspace(const QString &projectId, const QStringList &paths,
                                           const QString &watchId,
                                           const QString &rootId)
{
    QJsonArray pathValues;
    for (const QString &path : paths) pathValues.append(path);
    QJsonObject params{
        {QStringLiteral("project_id"), projectId},
        {QStringLiteral("paths"), pathValues},
    };
    if (!watchId.isEmpty()) params.insert(QStringLiteral("watch_id"), watchId);
    if (!rootId.isEmpty()) params.insert(QStringLiteral("root_id"), rootId);
    return sendRequest(QStringLiteral("workspace/watch"), params);
}

QString AgentRuntimeClient::pollWorkspaceWatch(const QString &watchId)
{
    return sendRequest(QStringLiteral("workspace/watch/poll"),
                       {{QStringLiteral("watch_id"), watchId}});
}

QString AgentRuntimeClient::openUserTerminal(const QString &sessionId,
                                             const QString &kind,
                                             const QString &name,
                                             int rows, int cols)
{
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("kind"), kind},
        {QStringLiteral("rows"), rows},
        {QStringLiteral("cols"), cols},
    };
    if (!name.isEmpty()) params.insert(QStringLiteral("name"), name);
    return sendRequest(QStringLiteral("terminal/open-user"), params);
}

QString AgentRuntimeClient::listTerminals(const QString &sessionId)
{
    return sendRequest(QStringLiteral("terminal/list"),
                       {{QStringLiteral("session_id"), sessionId}});
}

QString AgentRuntimeClient::attachTerminal(const QString &sessionId,
                                           const QString &terminalId,
                                           quint64 after)
{
    return sendRequest(QStringLiteral("terminal/attach"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
        {QStringLiteral("after"), static_cast<qint64>(after)},
    });
}

QString AgentRuntimeClient::readTerminalExcerpt(const QString &sessionId,
                                                const QString &terminalId,
                                                int maxBytes)
{
    return sendRequest(QStringLiteral("terminal/excerpt/read"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
        {QStringLiteral("max_bytes"), maxBytes},
    });
}

QString AgentRuntimeClient::inputUserTerminal(const QString &sessionId,
                                              const QString &terminalId,
                                              const QByteArray &data)
{
    return sendRequest(QStringLiteral("terminal/input-user"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
        {QStringLiteral("data_base64"), QString::fromLatin1(data.toBase64())},
    });
}

QString AgentRuntimeClient::resizeTerminal(const QString &sessionId,
                                           const QString &terminalId,
                                           int rows, int cols)
{
    return sendRequest(QStringLiteral("terminal/resize"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
        {QStringLiteral("rows"), rows},
        {QStringLiteral("cols"), cols},
    });
}

QString AgentRuntimeClient::signalUserTerminal(const QString &sessionId,
                                               const QString &terminalId,
                                               const QString &signal)
{
    return sendRequest(QStringLiteral("terminal/signal-user"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
        {QStringLiteral("signal"), signal},
    });
}

QString AgentRuntimeClient::stopUserTerminal(const QString &sessionId,
                                             const QString &terminalId)
{
    return sendRequest(QStringLiteral("terminal/stop-user"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
    });
}

QString AgentRuntimeClient::restartUserTerminal(const QString &sessionId,
                                                const QString &terminalId,
                                                int rows, int cols)
{
    QJsonObject params{
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
    };
    if (rows > 0) params.insert(QStringLiteral("rows"), rows);
    if (cols > 0) params.insert(QStringLiteral("cols"), cols);
    return sendRequest(QStringLiteral("terminal/restart-user"), params);
}

QString AgentRuntimeClient::removeUserTerminal(const QString &sessionId,
                                               const QString &terminalId)
{
    return sendRequest(QStringLiteral("terminal/remove-user"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("terminal_id"), terminalId},
    });
}

QString AgentRuntimeClient::readCommandArtifact(const QString &sessionId,
                                                const QString &reference)
{
    return sendRequest(QStringLiteral("artifact/read-command-output"), {
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("reference"), reference},
    });
}

QString AgentRuntimeClient::locateRuntime() const
{
    const QString executableName =
#ifdef Q_OS_WIN
        QStringLiteral("aegisy-agentd.exe");
#else
        QStringLiteral("aegisy-agentd");
#endif
    const QString fromEnvironment = qEnvironmentVariable("AEGISY_AGENTD_PATH");
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        fromEnvironment,
        QDir(appDir).filePath(executableName),
        QDir(appDir).filePath(QStringLiteral("../Resources/") + executableName),
#ifdef AEGISY_AGENTD_DEV_PATH
        QStringLiteral(AEGISY_AGENTD_DEV_PATH),
#endif
    };
    for (const QString &candidate : candidates) {
        if (candidate.isEmpty()) continue;
        const QFileInfo info(candidate);
        if (info.isFile() && info.isExecutable()) return info.absoluteFilePath();
    }
    return {};
}

QString AgentRuntimeClient::sendRequest(const QString &method, const QJsonObject &params)
{
    if (m_process->state() == QProcess::NotRunning) {
        emit requestFailed({}, method, QStringLiteral("本地运行时未启动"), -1);
        return {};
    }
    const QString id = QString::number(++m_nextRequestId);
    m_pendingMethods.insert(id, method);
    writeMessage({
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    });
    return id;
}

void AgentRuntimeClient::sendNotification(const QString &method, const QJsonObject &params)
{
    writeMessage({
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    });
}

void AgentRuntimeClient::writeMessage(const QJsonObject &message)
{
    QByteArray frame = QJsonDocument(message).toJson(QJsonDocument::Compact);
    frame.append('\n');
    m_process->write(frame);
}

void AgentRuntimeClient::processStdout()
{
    m_stdoutBuffer.append(m_process->readAllStandardOutput());
    if (m_stdoutBuffer.size() > kMaximumFrameBytes) {
        emit connectionStateChanged(false, QStringLiteral("运行时返回了超限消息"));
        m_process->kill();
        return;
    }
    qsizetype newline = -1;
    while ((newline = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            emit diagnosticMessage(QStringLiteral("忽略无效 AAP 消息：%1").arg(error.errorString()));
            continue;
        }
        processMessage(document.object());
    }
}

void AgentRuntimeClient::processMessage(const QJsonObject &message)
{
    const QString method = message.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("event")) {
        emit timelineEvent(message.value(QStringLiteral("params")).toObject());
        return;
    }

    const QString id = message.value(QStringLiteral("id")).toVariant().toString();
    if (id.isEmpty()) return;
    const QString pendingMethod = m_pendingMethods.take(id);
    if (pendingMethod.isEmpty()) return;
    const QJsonObject error = message.value(QStringLiteral("error")).toObject();
    if (!error.isEmpty()) {
        emit requestFailed(id, pendingMethod,
                           error.value(QStringLiteral("message")).toString(),
                           error.value(QStringLiteral("code")).toInt(-1));
        return;
    }
    const QJsonObject result = message.value(QStringLiteral("result")).toObject();
    if (pendingMethod == QStringLiteral("initialize")) {
        m_startupTimer->stop();
        sendNotification(QStringLiteral("initialized"));
        const QJsonObject runtime = result.value(QStringLiteral("runtime")).toObject();
        const QJsonObject backend = result.value(QStringLiteral("backend")).toObject();
        const QString backendStatus = backend.value(QStringLiteral("status")).toString();
        m_recoveryMode = backendStatus == QStringLiteral("read-only-recovery");
        m_ready = backendStatus == QStringLiteral("ready") || m_recoveryMode;
        const QString detail = QStringLiteral("%1 %2 · %3 %4 · AAP %5")
            .arg(runtime.value(QStringLiteral("name")).toString(),
                 runtime.value(QStringLiteral("version")).toString(),
                 backend.value(QStringLiteral("adapter")).toString(),
                 backend.value(QStringLiteral("version")).toString(),
                 result.value(QStringLiteral("protocol_version")).toString());
        emit runtimeInitialized(result);
        emit connectionStateChanged(m_ready, detail);
        runtimeHealth();
        runtimeDegradations();
        if (m_recoveryMode) runtimeRecoveryStatus();
        else projectionRecoveryStatus();
    } else if (pendingMethod == QStringLiteral("project/list")) {
        emit projectsListed(id, result);
    } else if (pendingMethod == QStringLiteral("project/navigation")) {
        emit projectNavigationChanged(id, result);
    } else if (pendingMethod == QStringLiteral("project/open")) {
        const QJsonObject project = result.value(QStringLiteral("project")).toObject();
        const QJsonObject identity = result.value(QStringLiteral("identity")).toObject();
        if (!identity.value(QStringLiteral("relink_required")).toBool()) {
            emit projectOpened(id, project);
        } else {
            emit projectRelinkRequired(id, project, identity);
        }
        const QJsonObject trustReview = result.value(QStringLiteral("trust_review")).toObject();
        if (!trustReview.isEmpty() && !identity.value(QStringLiteral("relink_required")).toBool()) {
            emit projectTrustReviewRequired(id, project, trustReview);
        }
    } else if (pendingMethod == QStringLiteral("project/relink")) {
        emit projectOpened(id, result.value(QStringLiteral("project")).toObject());
    } else if (pendingMethod == QStringLiteral("project/trust-acknowledge")) {
        emit projectTrustAcknowledged(id, result);
    } else if (pendingMethod == QStringLiteral("project/root-list")) {
        emit projectRootsListed(id, result);
    } else if (pendingMethod == QStringLiteral("project/root-add")
               || pendingMethod == QStringLiteral("project/root-remove")) {
        emit projectRootChanged(id, pendingMethod, result);
    } else if (pendingMethod == QStringLiteral("session/start")) {
        QJsonObject session = result.value(QStringLiteral("session")).toObject();
        session.insert(QStringLiteral("runtime"), result.value(QStringLiteral("runtime")));
        emit sessionStarted(id, session);
    } else if (pendingMethod == QStringLiteral("session/resume")) {
        emit sessionResumed(id, result);
    } else if (pendingMethod == QStringLiteral("session/fork")) {
        emit sessionForked(id, result);
    } else if (pendingMethod == QStringLiteral("session/list")) {
        emit sessionsListed(id, result);
    } else if (pendingMethod == QStringLiteral("session/search")) {
        emit sessionsListed(id, result);
    } else if (pendingMethod == QStringLiteral("session/title")
               || pendingMethod == QStringLiteral("session/archive")
               || pendingMethod == QStringLiteral("session/unarchive")) {
        emit sessionChanged(id, pendingMethod, result);
    } else if (pendingMethod == QStringLiteral("session/delete/preview")) {
        emit sessionDeletionPreviewed(id, result);
    } else if (pendingMethod == QStringLiteral("session/delete/schedule")
               || pendingMethod == QStringLiteral("session/delete/undo")) {
        emit sessionDeletionChanged(id, pendingMethod, result);
    } else if (pendingMethod == QStringLiteral("session/deletion/status")) {
        emit sessionDeletionStatusRead(id, result);
    } else if (pendingMethod == QStringLiteral("session/export/preview")) {
        emit portableSessionExportPreviewed(id, result);
    } else if (pendingMethod == QStringLiteral("session/export")) {
        emit portableSessionExported(id, result);
    } else if (pendingMethod == QStringLiteral("session/import/preview")) {
        emit portableSessionImportPreviewed(id, result);
    } else if (pendingMethod == QStringLiteral("session/import")) {
        emit portableSessionImported(id, result);
    } else if (pendingMethod == QStringLiteral("retention/maintenance/run")) {
        emit retentionMaintenanceCompleted(id, result);
    } else if (pendingMethod == QStringLiteral("retention/policy/read")) {
        emit retentionPolicyRead(id, result);
    } else if (pendingMethod == QStringLiteral("retention/policy/set")
               || pendingMethod == QStringLiteral("retention/policy/remove")) {
        emit retentionPolicyChanged(id, pendingMethod, result);
    } else if (pendingMethod == QStringLiteral("session/read")) {
        emit sessionRead(id, result);
    } else if (pendingMethod == QStringLiteral("runtime/projection-recovery/status")) {
        emit projectionRecoveryStatusRead(result);
    } else if (pendingMethod == QStringLiteral("runtime/health")) {
        emit runtimeHealthRead(result);
    } else if (pendingMethod == QStringLiteral("runtime/degradations")) {
        emit runtimeDegradationsRead(id, result);
    } else if (pendingMethod == QStringLiteral("runtime/restart")) {
        emit runtimeRestarted(id, result);
    } else if (pendingMethod == QStringLiteral("session/recovery/status")) {
        emit sessionRecoveryStatusRead(result);
    } else if (pendingMethod == QStringLiteral("operation/status")) {
        emit operationStatusRead(id, result);
    } else if (pendingMethod == QStringLiteral("operation/probe")) {
        emit operationProbeRead(id, result);
    } else if (pendingMethod == QStringLiteral("operation/reconcile")) {
        emit operationReconciled(id, result);
    } else if (pendingMethod == QStringLiteral("session/compaction/checkpoint/create")) {
        emit compactionCheckpointCreated(id, result);
    } else if (pendingMethod == QStringLiteral("session/compaction/checkpoint/read")) {
        emit compactionCheckpointRead(id, result);
    } else if (pendingMethod == QStringLiteral("session/compaction/checkpoint/revise")) {
        emit compactionCheckpointRevised(id, result);
    } else if (pendingMethod == QStringLiteral("runtime/recovery/status")) {
        emit runtimeRecoveryStatusRead(result);
    } else if (pendingMethod == QStringLiteral("turn/cancel")) {
        emit turnCancellationRequested(id, result);
    } else if (pendingMethod == QStringLiteral("turn/context/inspect")) {
        emit turnContextInspected(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/pinned-context/list")) {
        emit pinnedContextListed(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/pinned-context/save")
               || pendingMethod == QStringLiteral("workspace/pinned-context/remove")) {
        emit pinnedContextChanged(id, pendingMethod, result);
    } else if (pendingMethod == QStringLiteral("workspace/list")) {
        emit workspaceListed(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/read")) {
        emit workspaceFileRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/save-user-text")) {
        emit workspaceFileSaved(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/metadata")) {
        emit workspaceMetadataRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git-status")) {
        emit workspaceGitStatusRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git/overview")) {
        emit gitOverviewRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git/log")) {
        emit gitLogRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git/commit")) {
        emit gitCommitRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/git/diff")) {
        emit gitDiffRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/search")) {
        emit workspaceSearchCompleted(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/search/cancel")) {
        emit workspaceSearchCancelled(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/index")) {
        emit workspaceIndexed(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/index/cancel")) {
        emit workspaceIndexCancelled(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/repository-map")) {
        emit repositoryMapRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/language-servers")) {
        emit languageServersRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/language-server/start")) {
        emit languageServerStarted(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/language-server/stop")) {
        emit languageServerStopped(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/definition")) {
        emit workspaceDefinitionsRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/references")) {
        emit workspaceReferencesRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/diagnostics")) {
        emit workspaceDiagnosticsRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/observed-diagnostics")) {
        emit observedDiagnosticsRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/diagnostics/raw")) {
        emit diagnosticRawRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/edit/preview")) {
        emit workspaceEditPreviewed(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/edit/artifact/read")) {
        emit workspaceEditArtifactRead(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/watch")) {
        emit workspaceWatchConfigured(id, result);
    } else if (pendingMethod == QStringLiteral("workspace/watch/poll")) {
        emit workspaceChanged(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/open-user")) {
        emit terminalOpened(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/list")) {
        emit terminalsListed(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/read")
               || pendingMethod == QStringLiteral("terminal/attach")) {
        emit terminalAttached(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/excerpt/read")) {
        emit terminalExcerptRead(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/input-user")) {
        emit terminalInputAccepted(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/resize")) {
        emit terminalResized(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/signal-user")) {
        emit terminalSignalled(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/close-user")
               || pendingMethod == QStringLiteral("terminal/stop-user")) {
        emit terminalStopped(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/restart-user")) {
        emit terminalRestarted(id, result);
    } else if (pendingMethod == QStringLiteral("terminal/remove-user")) {
        emit terminalRemoved(id, result);
    } else if (pendingMethod == QStringLiteral("artifact/read-command-output")) {
        emit commandArtifactRead(id, result);
    }
}

void AgentRuntimeClient::failPending(const QString &message)
{
    const auto pending = m_pendingMethods;
    m_pendingMethods.clear();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        emit requestFailed(it.key(), it.value(), message, -1);
    }
}
