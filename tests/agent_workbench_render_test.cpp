#include "agent_workbench_widget.h"
#include "agent_runtime_client.h"
#include "app_theme.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDebug>
#include <QDialog>
#include <QImage>
#include <QJsonDocument>
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
#include <QVBoxLayout>
#include <algorithm>
#include <iterator>

class AgentWorkbenchWidgetTestAccess
{
public:
    static void setProposalContext(AgentWorkbenchWidget &widget,
                                   const QString &sessionId,
                                   const QString &projectId)
    {
        widget.m_mode = QStringLiteral("work");
        widget.m_workSessionId = sessionId;
        widget.m_projectId = projectId;
        widget.m_workspaceEditProposalAvailable = true;
        widget.m_sessionRuntimeBindings.insert(sessionId, QJsonObject{
            {QStringLiteral("adapter"), QStringLiteral("codex-app-server")},
            {QStringLiteral("version"), QStringLiteral("codex-cli 0.144.5")},
            {QStringLiteral("provider"), QStringLiteral("openai")},
            {QStringLiteral("model"), QStringLiteral("test-model")},
            {QStringLiteral("permission_profile"), QStringLiteral("read-only")},
        });
        widget.m_sessionWorkspaceBindings.insert(sessionId, QJsonObject{
            {QStringLiteral("project_id"), projectId},
            {QStringLiteral("root_id"), QStringLiteral("root-1")},
        });
    }

    static void prepareProposalRequest(AgentWorkbenchWidget &widget,
                                       const QString &requestId,
                                       const QString &sessionId,
                                       quint64 generation)
    {
        if (!widget.m_sessionRuntimeBindings.contains(sessionId)) {
            widget.m_sessionRuntimeBindings.insert(sessionId, QJsonObject{
                {QStringLiteral("adapter"), QStringLiteral("codex-app-server")},
                {QStringLiteral("version"), QStringLiteral("codex-cli 0.144.5")},
                {QStringLiteral("provider"), QStringLiteral("openai")},
                {QStringLiteral("model"), QStringLiteral("test-model")},
                {QStringLiteral("permission_profile"), QStringLiteral("read-only")},
            });
        }
        if (!widget.m_sessionWorkspaceBindings.contains(sessionId)) {
            widget.m_sessionWorkspaceBindings.insert(sessionId, QJsonObject{
                {QStringLiteral("project_id"), widget.m_projectId},
                {QStringLiteral("root_id"), QStringLiteral("root-1")},
            });
        }
        widget.m_workspaceEditProposalGenerations.insert(sessionId, generation);
        widget.m_workspaceEditProposalRequests.insert(
            requestId, {sessionId, QString(), generation});
    }

    static void prepareProposalReferenceRequest(
        AgentWorkbenchWidget &widget, const QString &requestId,
        const QString &itemId, const QJsonObject &reference,
        quint64 generation)
    {
        const QString sessionId = reference.value(QStringLiteral("session_id")).toString();
        const QString proposalId = reference.value(QStringLiteral("proposal_id")).toString();
        const QString itemKey = sessionId + QChar(0x1f)
            + reference.value(QStringLiteral("turn_id")).toString()
            + QChar(0x1f) + itemId;
        widget.m_workspaceEditProposalReferenceGeneration = generation;
        widget.m_workspaceEditReferenceSelectionSessionId = sessionId;
        widget.m_workspaceEditReferenceSelectionProposalId = proposalId;
        widget.m_workspaceEditProposalRequests.insert(
            requestId, {sessionId, proposalId, generation, itemKey, reference});
    }

    static void setWorkSession(AgentWorkbenchWidget &widget,
                               const QString &sessionId)
    {
        widget.m_mode = QStringLiteral("work");
        widget.m_workSessionId = sessionId;
    }

    static bool hasConfirmedProposal(const AgentWorkbenchWidget &widget,
                                     const QString &sessionId,
                                     const QString &proposalId)
    {
        return widget.m_confirmedWorkspaceEditProposals.value(sessionId)
            .value(QStringLiteral("proposal_id")).toString() == proposalId;
    }

    static bool proposalUnread(const AgentWorkbenchWidget &widget,
                               const QString &sessionId)
    {
        return widget.m_unreadWorkspaceEditProposalSessions.contains(sessionId);
    }

    static qsizetype proposalPendingCount(const AgentWorkbenchWidget &widget)
    {
        return widget.m_workspaceEditProposalRequests.size();
    }

    static void clearProposalPending(AgentWorkbenchWidget &widget)
    {
        widget.clearWorkspaceEditProposalPending();
    }

    static void prepareCurrentProposalArtifactRequest(AgentWorkbenchWidget &widget,
                                                      const QString &requestId)
    {
        widget.m_workspaceEditProposalArtifactRequests.insert(requestId, {
            widget.m_workspaceEditProposalSessionId,
            widget.m_workspaceEditProposalId,
            widget.m_projectId,
            widget.m_workspaceEditId,
            widget.m_workspaceEditReference,
            widget.m_workspaceEditExpectedArtifactSha256,
            widget.m_workspaceEditExpectedArtifactBytes,
            widget.m_workspaceEditOffset,
            widget.m_workspaceEditProposalArtifactGeneration,
            widget.m_workspaceEditProposalArtifactBytes,
        });
    }

    static void prepareCommandArtifactWorkflow(
        AgentWorkbenchWidget &widget, const QString &requestId,
        const QString &sessionId, const QString &itemId,
        const QString &reference)
    {
        widget.invalidateCommandArtifactWorkflow();
        widget.m_pinnedContextAvailable = true;
        auto *dialog = new QDialog(&widget);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        auto *layout = new QVBoxLayout(dialog);
        auto *status = new QLabel(QStringLiteral("正在读取首个安全分页…"), dialog);
        status->setObjectName(QStringLiteral("commandArtifactStatus"));
        auto *preview = new QPlainTextEdit(dialog);
        preview->setObjectName(QStringLiteral("commandArtifactPreview"));
        preview->setReadOnly(true);
        auto *loadMore = new QPushButton(QStringLiteral("加载更多"), dialog);
        loadMore->setObjectName(QStringLiteral("commandArtifactLoadMoreButton"));
        loadMore->setEnabled(false);
        auto *pin = new QPushButton(QStringLiteral("固定完整输出"), dialog);
        pin->setObjectName(QStringLiteral("commandArtifactPinButton"));
        pin->setEnabled(false);
        layout->addWidget(status);
        layout->addWidget(preview);
        layout->addWidget(loadMore);
        layout->addWidget(pin);
        dialog->show();
        const quint64 generation = ++widget.m_commandArtifactWorkflowGeneration;
        AgentWorkbenchWidget::CommandArtifactWorkflow workflow;
        workflow.state = AgentWorkbenchWidget::CommandArtifactWorkflowState::LoadingFirst;
        workflow.generation = generation;
        workflow.processGeneration = widget.m_runtime->processGeneration();
        workflow.requestId = requestId;
        workflow.sessionId = sessionId;
        workflow.itemId = itemId;
        workflow.reference = reference;
        workflow.itemKey = itemId;
        workflow.dialog = dialog;
        workflow.status = status;
        workflow.preview = preview;
        workflow.loadMore = loadMore;
        workflow.pin = pin;
        widget.m_commandArtifactWorkflow = workflow;
        widget.m_commandArtifactPageRequests.insert(requestId, {
            generation, workflow.processGeneration, 0, {},
        });
    }

    static void prepareCommandArtifactContinuation(
        AgentWorkbenchWidget &widget, const QString &requestId,
        const QJsonObject &cursor)
    {
        auto &workflow = widget.m_commandArtifactWorkflow;
        workflow.state = AgentWorkbenchWidget::CommandArtifactWorkflowState::LoadingNext;
        workflow.requestId = requestId;
        widget.m_commandArtifactPageRequests.insert(requestId, {
            workflow.generation, workflow.processGeneration,
            workflow.accumulated.size(), cursor,
        });
    }

    static void deliverCommandArtifactPage(AgentWorkbenchWidget &widget,
                                           const QString &requestId,
                                           const QJsonObject &page)
    {
        widget.handleCommandArtifactPage(requestId, page);
    }

    static void invalidateCommandArtifactWorkflow(AgentWorkbenchWidget &widget)
    {
        widget.invalidateCommandArtifactWorkflow();
    }

    static QByteArray proposalArtifactBytes(const AgentWorkbenchWidget &widget)
    {
        return widget.m_workspaceEditProposalArtifactBytes;
    }

    static qint64 proposalArtifactOffset(const AgentWorkbenchWidget &widget)
    {
        return widget.m_workspaceEditOffset;
    }

    static QString proposalArtifactReference(const AgentWorkbenchWidget &widget)
    {
        return widget.m_workspaceEditReference;
    }

    static QString currentProposalId(const AgentWorkbenchWidget &widget)
    {
        return widget.m_workspaceEditProposalId;
    }

    static quint64 proposalArtifactGeneration(const AgentWorkbenchWidget &widget)
    {
        return widget.m_workspaceEditProposalArtifactGeneration;
    }

    static qsizetype proposalArtifactPendingCount(const AgentWorkbenchWidget &widget)
    {
        return widget.m_workspaceEditProposalArtifactRequests.size();
    }

    static bool proposalUnverified(const AgentWorkbenchWidget &widget,
                                   const QString &sessionId)
    {
        return widget.m_unverifiedWorkspaceEditProposalSessions.contains(sessionId);
    }

    static int workspaceEditTab(const AgentWorkbenchWidget &widget)
    {
        return widget.m_workspaceEditTab;
    }

    static bool storeContextThreshold(AgentWorkbenchWidget &widget,
                                      const QString &sessionId,
                                      const QJsonObject &summary)
    {
        return widget.storeSessionContextThreshold(sessionId, summary);
    }

    static qsizetype contextThresholdCacheSize(const AgentWorkbenchWidget &widget)
    {
        return widget.m_sessionContextThresholds.size();
    }

    static bool containsContextThreshold(const AgentWorkbenchWidget &widget,
                                         const QString &sessionId)
    {
        return widget.m_sessionContextThresholds.contains(sessionId);
    }

    static void setCurrentChatSession(AgentWorkbenchWidget &widget,
                                      const QString &sessionId)
    {
        widget.m_chatSessionId = sessionId;
    }

    static bool sessionListRefreshIdle(const AgentWorkbenchWidget &widget)
    {
        return widget.m_sessionListRequestId.isEmpty()
            && !widget.m_sessionListRefreshPending;
    }

    static bool operationStatusRequestIdle(const AgentWorkbenchWidget &widget)
    {
        return widget.m_operationStatusRequestId.isEmpty();
    }

    static bool operationStatusAllowsSession(const AgentWorkbenchWidget &widget,
                                             const QString &sessionId)
    {
        return widget.m_operationStatusSessionId == sessionId
            && widget.m_operationStatusKnown && !widget.m_operationStatusBlocked;
    }

    static bool activateMode(AgentWorkbenchWidget &widget, const QString &mode)
    {
        if (!widget.m_modeGroup) return false;
        for (QAbstractButton *button : widget.m_modeGroup->buttons()) {
            if (button->property("mode").toString() != mode) continue;
            button->click();
            return widget.m_mode == mode;
        }
        return false;
    }

    static QString currentMode(const AgentWorkbenchWidget &widget)
    {
        return widget.m_mode;
    }

    static void prepareOperationStatusSession(AgentWorkbenchWidget &widget,
                                              const QString &sessionId)
    {
        widget.m_operationStatusSessionId = sessionId;
        widget.m_operationStatusKnown = false;
        widget.m_operationStatusBlocked = false;
    }

    static bool activeTurnSubmitIsInert(AgentWorkbenchWidget &widget,
                                        QTextEdit *composer,
                                        QPushButton *sendButton)
    {
        if (!composer || !sendButton) return false;
        const bool previousRunning = widget.m_turnRunning;
        const bool previousCancelling = widget.m_turnCancelling;
        const QString previousSessionId = widget.m_activeTurnSessionId;
        const QString previousTurnId = widget.m_activeTurnId;
        const QString previousText = composer->toPlainText();
        widget.m_turnRunning = true;
        widget.m_turnCancelling = false;
        widget.m_activeTurnSessionId = QStringLiteral("active-session");
        widget.m_activeTurnId = QStringLiteral("active-turn");
        composer->setPlainText(QStringLiteral("must-not-start-another-turn"));
        widget.updateTurnAction();
        const bool expectedStopEnabled = widget.m_runtime
            && widget.m_runtime->isControlAvailable()
            && !widget.m_activeTurnControlUnverified;
        widget.submitPrompt();
        const bool inert = composer->toPlainText()
                == QStringLiteral("must-not-start-another-turn")
            && sendButton->text() == QStringLiteral("停止")
            && sendButton->isEnabled() == expectedStopEnabled
            && widget.m_activeTurnSessionId == QStringLiteral("active-session")
            && widget.m_activeTurnId == QStringLiteral("active-turn");
        widget.m_turnRunning = previousRunning;
        widget.m_turnCancelling = previousCancelling;
        widget.m_activeTurnSessionId = previousSessionId;
        widget.m_activeTurnId = previousTurnId;
        composer->setPlainText(previousText);
        widget.updateTurnAction();
        return inert;
    }

    static void setPendingPrompt(AgentWorkbenchWidget &widget, const QString &prompt)
    {
        widget.m_pendingPrompt = prompt;
    }

    static QString pendingPrompt(const AgentWorkbenchWidget &widget)
    {
        return widget.m_pendingPrompt;
    }

    static void prepareRuntimeDegradationRequest(AgentWorkbenchWidget &widget,
                                                 const QString &requestId)
    {
        widget.m_runtimeDegradationState =
            AgentWorkbenchWidget::RuntimeDegradationState::Pending;
        widget.m_runtimeDegradationRequestId = requestId;
        widget.m_runtimeDegradationStates.clear();
        widget.updateRuntimeCapabilityUi();
        widget.updateTurnAction();
    }

    static QString runtimeDegradationRequestId(const AgentWorkbenchWidget &widget)
    {
        return widget.m_runtimeDegradationRequestId;
    }

    static bool runtimeDegradationPending(const AgentWorkbenchWidget &widget)
    {
        return widget.m_runtimeDegradationState
            == AgentWorkbenchWidget::RuntimeDegradationState::Pending;
    }

    static void tryStartPendingTurn(AgentWorkbenchWidget &widget)
    {
        widget.startPendingTurnIfReady();
    }

    static void setMutationAcknowledgementAvailable(AgentWorkbenchWidget &widget,
                                                    bool available)
    {
        widget.m_mutationAcknowledgementAvailable = available;
        widget.updateTurnAction();
    }

    static void submitPrompt(AgentWorkbenchWidget &widget)
    {
        widget.submitPrompt();
    }

    static bool turnRunning(const AgentWorkbenchWidget &widget)
    {
        return widget.m_turnRunning;
    }

    static bool turnCancelling(const AgentWorkbenchWidget &widget)
    {
        return widget.m_turnCancelling;
    }

    static quint64 lastTimelineSequence(const AgentWorkbenchWidget &widget)
    {
        const QString sessionId = widget.m_mode == QStringLiteral("work")
            ? widget.m_workSessionId : widget.m_chatSessionId;
        return widget.m_lastTimelineEventSequences.value(sessionId, 0);
    }

    static quint64 timelineSequenceForSession(const AgentWorkbenchWidget &widget,
                                              const QString &sessionId)
    {
        return widget.m_lastTimelineEventSequences.value(sessionId, 0);
    }

    static QString currentSessionId(const AgentWorkbenchWidget &widget)
    {
        return widget.m_mode == QStringLiteral("work")
            ? widget.m_workSessionId : widget.m_chatSessionId;
    }

    static bool mutationReconciliationBlocked(
        const AgentWorkbenchWidget &widget, const QString &sessionId)
    {
        return widget.m_mutationReconciliationSessionIds.contains(sessionId);
    }

    static bool terminalMutationAcknowledgementConsumed(
        const AgentWorkbenchWidget &widget, const QString &sessionId)
    {
        const QSet<QString> identities =
            widget.m_durableMutationSessionOperations.value(sessionId);
        return std::any_of(
            identities.cbegin(), identities.cend(),
            [&widget](const QString &identity) {
                const QJsonObject operation =
                    widget.m_durableMutationOperations.value(identity);
                return operation.value(QStringLiteral("state")).toString()
                        == QStringLiteral("terminal")
                    && operation.value(
                        QStringLiteral("accepted_consumed")).toBool()
                    && operation.value(
                        QStringLiteral("terminal_consumed")).toBool();
            });
    }

    static quint64 timelineTimestampForSession(const AgentWorkbenchWidget &widget,
                                               const QString &sessionId)
    {
        return widget.m_lastTimelineEventTimestamps.value(sessionId, 0);
    }

    static QString timelineTurnState(const AgentWorkbenchWidget &widget,
                                     const QString &sessionId,
                                     const QString &turnId)
    {
        const QString key = sessionId + QChar(0x1f) + turnId;
        return widget.m_turnStates.value(key);
    }

    static QString timelineItemState(const AgentWorkbenchWidget &widget,
                                     const QString &sessionId,
                                     const QString &turnId,
                                     const QString &itemId)
    {
        const QString key = sessionId + QChar(0x1f) + turnId
            + QChar(0x1f) + itemId;
        return widget.m_itemStates.value(key);
    }

    static quint64 unknownTimelineEventCount(const AgentWorkbenchWidget &widget)
    {
        quint64 total = widget.m_unknownTimelineEventOverflowCount;
        for (quint64 count : widget.m_unknownTimelineEventCounts) total += count;
        return total;
    }

    static void resetTimelineValidation(AgentWorkbenchWidget &widget)
    {
        widget.m_turnRunning = false;
        widget.m_turnCancelling = false;
        widget.m_activeTurnSessionId.clear();
        widget.m_activeTurnId.clear();
        widget.m_lastTimelineEventSequences.clear();
        widget.m_lastTimelineEventTimestamps.clear();
        widget.m_turnStates.clear();
        widget.m_itemKinds.clear();
        widget.m_itemRoles.clear();
        widget.m_itemStates.clear();
        widget.m_itemRevisions.clear();
        widget.m_timelineSessions.clear();
        widget.m_timelineTrackedEventCount = 0;
        widget.m_timelinePendingEventCount = 0;
        widget.m_timelinePendingBytes = 0;
        widget.m_timelineTrackingExhausted = false;
        widget.m_timelineSyncAvailable = false;
        widget.m_timelineSnapshotAvailable = false;
        widget.m_timelineSubscriptionAvailable = false;
        widget.m_timelineSnapshotRequester = {};
        widget.m_timelineSubscriptionActivationRequester = {};
        widget.m_timelineSubscriptionConnectionAbandoner = {};
        widget.m_runtimeReconnectRecoveryGeneration = 0;
        widget.m_runtimeReconnectTimelinePending.clear();
        widget.m_unknownTimelineEventCounts.clear();
        widget.m_unknownTimelineEventOverflowCount = 0;
        widget.updateTurnAction();
    }

    static void prepareTimelineSync(AgentWorkbenchWidget &widget,
                                    const QString &sessionId,
                                    const QString &requestId)
    {
        auto &state = widget.m_timelineSessions[sessionId];
        state.recovery = AgentWorkbenchWidget::TimelineRecoveryState::RecoveringSync;
        state.syncRequestId = requestId;
        state.requestedAfterSequence = state.projection.sequence;
        state.requestedAfterEventId = state.projection.eventId;
        state.watermark = {};
        state.syncProjection = state.projection;
        state.syncProjectionValid = true;
        state.stagedSyncEvents.clear();
        state.stagedSyncBytes = 0;
        state.retryOnReconnect = false;
    }

    static QString timelineRecoveryState(const AgentWorkbenchWidget &widget,
                                         const QString &sessionId)
    {
        const auto state = widget.m_timelineSessions.constFind(sessionId);
        if (state == widget.m_timelineSessions.cend()) return QStringLiteral("missing");
        switch (state->recovery) {
        case AgentWorkbenchWidget::TimelineRecoveryState::Untracked:
            return QStringLiteral("untracked");
        case AgentWorkbenchWidget::TimelineRecoveryState::Active:
            return QStringLiteral("live");
        case AgentWorkbenchWidget::TimelineRecoveryState::RecoveringSync:
        case AgentWorkbenchWidget::TimelineRecoveryState::RecoveringSnapshot:
            return QStringLiteral("syncing");
        case AgentWorkbenchWidget::TimelineRecoveryState::Subscribing:
            return QStringLiteral("subscribing");
        case AgentWorkbenchWidget::TimelineRecoveryState::AwaitingActivation:
            return QStringLiteral("awaiting-activation");
        case AgentWorkbenchWidget::TimelineRecoveryState::Failed:
            return QStringLiteral("failed");
        case AgentWorkbenchWidget::TimelineRecoveryState::Frozen:
            return QStringLiteral("frozen");
        case AgentWorkbenchWidget::TimelineRecoveryState::Unrecoverable:
            return QStringLiteral("unrecoverable");
        }
        return QStringLiteral("invalid");
    }

    static QString timelineEventId(const AgentWorkbenchWidget &widget,
                                   const QString &sessionId)
    {
        return widget.m_timelineSessions.value(sessionId).projection.eventId;
    }

    static qsizetype queuedTimelineEvents(const AgentWorkbenchWidget &widget,
                                          const QString &sessionId)
    {
        return widget.m_timelineSessions.value(sessionId).queuedLiveEvents.size();
    }

    static QString timelineSyncRequestId(const AgentWorkbenchWidget &widget,
                                         const QString &sessionId)
    {
        return widget.m_timelineSessions.value(sessionId).syncRequestId;
    }

    static bool timelineRetriesOnReconnect(const AgentWorkbenchWidget &widget,
                                           const QString &sessionId)
    {
        return widget.m_timelineSessions.value(sessionId).retryOnReconnect;
    }

    static void exhaustTimelinePendingBytes(AgentWorkbenchWidget &widget)
    {
        widget.m_timelinePendingBytes = 64 * 1024 * 1024;
    }

    static qsizetype timelinePendingEventCount(const AgentWorkbenchWidget &widget)
    {
        return widget.m_timelinePendingEventCount;
    }

    static void fillTimelineSessionCapacity(AgentWorkbenchWidget &widget)
    {
        for (qsizetype index = 0; index < 10000; ++index) {
            widget.m_timelineSessions.insert(
                QStringLiteral("capacity-session-%1").arg(index), {});
        }
    }

    static void beginTimelineSync(AgentWorkbenchWidget &widget,
                                  const QString &sessionId)
    {
        widget.beginTimelineSync(sessionId);
    }

    static bool timelineTrackingExhausted(const AgentWorkbenchWidget &widget)
    {
        return widget.m_timelineTrackingExhausted;
    }

    static void setTimelineSyncAvailable(AgentWorkbenchWidget &widget, bool available)
    {
        widget.m_timelineSyncAvailable = available;
    }

    static void setTimelineSnapshotAvailable(AgentWorkbenchWidget &widget, bool available)
    {
        widget.m_timelineSnapshotAvailable = available;
    }

    static void setTimelineSubscriptionAvailable(AgentWorkbenchWidget &widget,
                                                 bool available)
    {
        widget.m_timelineSubscriptionAvailable = available;
    }

    static void beginTimelineSnapshot(AgentWorkbenchWidget &widget,
                                      const QString &sessionId)
    {
        widget.beginTimelineSnapshot(sessionId);
    }

    static void prepareTimelineSnapshot(AgentWorkbenchWidget &widget,
                                        const QString &sessionId,
                                        const QString &requestId)
    {
        auto &state = widget.m_timelineSessions[sessionId];
        state.recovery = AgentWorkbenchWidget::TimelineRecoveryState::RecoveringSnapshot;
        state.snapshotRequestId = requestId;
        state.snapshotRecoveryRequired = true;
        state.snapshotIdentity.clear();
        state.snapshotFloor = {};
        state.snapshotWatermark = {};
        state.snapshotAfter = {};
        state.snapshotActiveTurn = {};
        state.snapshotTotalItems = 0;
        state.snapshotExpectedCanonicalBytes = 0;
        state.snapshotTotalCanonicalBytes = 0;
        state.stagedSnapshotItems.clear();
        state.stagedSnapshotBytes = 0;
    }

    static void setTimelineSnapshotRequester(AgentWorkbenchWidget &widget,
                                             const QString &requestId)
    {
        widget.m_timelineSnapshotRequester = [requestId](
            const QString &, const QString &, const QJsonObject &,
            const QJsonObject &, int) { return requestId; };
    }

    static void setTimelineSnapshotRequester(
        AgentWorkbenchWidget &widget,
        std::function<QString(const QString &, const QString &, const QJsonObject &,
                              const QJsonObject &, int)> requester)
    {
        widget.m_timelineSnapshotRequester = std::move(requester);
    }

    static QString timelineSnapshotRequestId(const AgentWorkbenchWidget &widget,
                                             const QString &sessionId)
    {
        return widget.m_timelineSessions.value(sessionId).snapshotRequestId;
    }

    static bool timelineSnapshotRecoveryRequired(const AgentWorkbenchWidget &widget,
                                                 const QString &sessionId)
    {
        return widget.m_timelineSessions.value(sessionId).snapshotRecoveryRequired;
    }

    static void prepareTimelineSubscriptionSync(
        AgentWorkbenchWidget &widget, const QString &sessionId, quint64 generation,
        const QString &subscriptionId, const QString &requestId,
        const QJsonObject &watermark)
    {
        auto &state = widget.m_timelineSessions[sessionId];
        state.recovery = AgentWorkbenchWidget::TimelineRecoveryState::RecoveringSync;
        state.subscriptionGeneration = generation;
        state.subscriptionId = subscriptionId;
        state.subscriptionCursor = {
            {QStringLiteral("sequence"), static_cast<double>(state.projection.sequence)},
            {QStringLiteral("event_id"), state.projection.sequence == 0
                 ? QJsonValue(QJsonValue::Null) : QJsonValue(state.projection.eventId)},
        };
        state.subscriptionWatermark = watermark;
        state.syncRequestId = requestId;
        state.requestedAfterSequence = state.projection.sequence;
        state.requestedAfterEventId = state.projection.eventId;
        state.watermark = watermark;
        state.syncProjection = state.projection;
        state.syncProjectionValid = true;
        state.activationSource = QStringLiteral("sync");
    }

    static void prepareTimelineSubscriptionSubscribe(
        AgentWorkbenchWidget &widget, const QString &sessionId, quint64 generation,
        const QString &subscriptionId, const QString &requestId)
    {
        auto &state = widget.m_timelineSessions[sessionId];
        state.recovery = AgentWorkbenchWidget::TimelineRecoveryState::Subscribing;
        state.subscriptionGeneration = generation;
        state.subscriptionId = subscriptionId;
        state.subscriptionCursor = {
            {QStringLiteral("sequence"), static_cast<double>(state.projection.sequence)},
            {QStringLiteral("event_id"), state.projection.sequence == 0
                 ? QJsonValue(QJsonValue::Null) : QJsonValue(state.projection.eventId)},
        };
        state.subscribeRequestId = requestId;
    }

    static void prepareTimelineSubscriptionSnapshot(
        AgentWorkbenchWidget &widget, const QString &sessionId, quint64 generation,
        const QString &subscriptionId, const QString &requestId)
    {
        auto &state = widget.m_timelineSessions[sessionId];
        state.recovery = AgentWorkbenchWidget::TimelineRecoveryState::RecoveringSnapshot;
        state.subscriptionGeneration = generation;
        state.subscriptionId = subscriptionId;
        state.subscriptionCursor = {
            {QStringLiteral("sequence"), static_cast<double>(state.projection.sequence)},
            {QStringLiteral("event_id"), state.projection.sequence == 0
                 ? QJsonValue(QJsonValue::Null) : QJsonValue(state.projection.eventId)},
        };
        state.snapshotRequestId = requestId;
        state.snapshotRecoveryRequired = true;
        state.activationSource = QStringLiteral("snapshot");
    }

    static void setTimelineSubscriptionActivationRequester(
        AgentWorkbenchWidget &widget, const QString &requestId)
    {
        widget.m_timelineSubscriptionActivationRequester =
            [requestId](quint64, const QString &, const QString &, const QString &,
                        const QJsonObject &, const QJsonObject &, const QString &) {
                return requestId;
            };
    }

    static void setTimelineSubscriptionConnectionAbandoner(
        AgentWorkbenchWidget &widget, std::function<void(const QString &)> abandoner)
    {
        widget.m_timelineSubscriptionConnectionAbandoner = std::move(abandoner);
    }

    static void setTimelineReconnectBarrier(AgentWorkbenchWidget &widget,
                                            quint64 generation,
                                            const QString &sessionId)
    {
        widget.m_runtimeReconnectRecoveryGeneration = generation;
        widget.m_runtimeReconnectTimelinePending.insert(sessionId);
    }

    static bool timelineReconnectBarrierContains(
        const AgentWorkbenchWidget &widget, quint64 generation,
        const QString &sessionId)
    {
        return widget.m_runtimeReconnectRecoveryGeneration == generation
            && widget.m_runtimeReconnectTimelinePending.contains(sessionId);
    }

    static bool timelineSubscriptionAuthorityCleared(
        const AgentWorkbenchWidget &widget, const QString &sessionId)
    {
        const auto state = widget.m_timelineSessions.constFind(sessionId);
        return state != widget.m_timelineSessions.cend()
            && state->subscriptionGeneration == 0
            && state->subscriptionId.isEmpty()
            && state->subscribeRequestId.isEmpty()
            && state->activationRequestId.isEmpty()
            && state->subscriptionCursor.isEmpty()
            && state->subscriptionWatermark.isEmpty();
    }

    static QJsonObject timelineSubscriptionCursor(
        const AgentWorkbenchWidget &widget, const QString &sessionId)
    {
        return widget.m_timelineSessions.value(sessionId).subscriptionCursor;
    }

    static quint64 timelineSubscriptionGeneration(
        const AgentWorkbenchWidget &widget, const QString &sessionId)
    {
        return widget.m_timelineSessions.value(sessionId).subscriptionGeneration;
    }

    static QString timelineSubscriptionId(
        const AgentWorkbenchWidget &widget, const QString &sessionId)
    {
        return widget.m_timelineSessions.value(sessionId).subscriptionId;
    }

    static bool timelineAllowsNewTurn(const AgentWorkbenchWidget &widget)
    {
        return !widget.currentTimelineSessionFrozen();
    }

    static void presentHistoryTimelineItem(AgentWorkbenchWidget &widget,
                                           const QJsonObject &item)
    {
        widget.addTimelineItem(item);
    }

    static void suspendTimelinesForDisconnect(AgentWorkbenchWidget &widget)
    {
        widget.suspendTimelinesForDisconnect();
    }

    static bool hasTimelineItem(const AgentWorkbenchWidget &widget, const QString &id)
    {
        return widget.m_itemLabels.contains(id);
    }

    static QString timelineItemText(const AgentWorkbenchWidget &widget, const QString &id)
    {
        const QLabel *label = widget.m_itemLabels.value(id, nullptr);
        return label ? label->text() : QString();
    }

    static bool validateTimelineItem(const AgentWorkbenchWidget &widget,
                                     const QJsonObject &item,
                                     const QString &sessionId,
                                     const QString &turnId)
    {
        QHash<QString, QString> kinds;
        QHash<QString, QString> roles;
        return widget.validateTimelineItem(item, sessionId, turnId, &kinds, &roles);
    }

    static int timelineItemPresentationCount(const AgentWorkbenchWidget &widget,
                                             const QString &id)
    {
        QSet<const QLabel *> labels;
        const QString suffix = QChar(0x1f) + id;
        for (auto entry = widget.m_itemLabels.cbegin();
             entry != widget.m_itemLabels.cend(); ++entry) {
            if (entry.key() == id || entry.key().endsWith(suffix)) {
                labels.insert(entry.value());
            }
        }
        return labels.size();
    }

    static void prepareSessionRead(AgentWorkbenchWidget &widget, const QString &requestId,
                                   const QString &sessionId, bool appending,
                                   const QString &cursor = QString(), int limit = 100,
                                   quint64 firstSequence = 0,
                                   quint64 latestSequence = 0)
    {
        widget.m_sessionReadRequestId = requestId;
        widget.m_sessionReadSessionId = sessionId;
        widget.m_sessionReadCursor = cursor;
        widget.m_sessionReadLimit = limit;
        widget.m_sessionReadExpectedFirstSequence = firstSequence;
        widget.m_sessionReadExpectedLatestSequence = latestSequence;
        widget.m_sessionHistoryAppending = appending;
        widget.m_sessionHistoryId = appending ? sessionId : QString();
        widget.m_sessionHistoryCursor = cursor;
        widget.m_sessionHistoryFirstSequence = firstSequence;
        widget.m_sessionHistoryLatestSequence = latestSequence;
    }

    static void setRequestedHistoryCursor(AgentWorkbenchWidget &widget,
                                          const QString &cursor)
    {
        widget.m_sessionReadCursor = cursor;
    }

    static QString historyCursor(const AgentWorkbenchWidget &widget)
    {
        return widget.m_sessionHistoryCursor;
    }

    static quint64 historyFirstSequence(const AgentWorkbenchWidget &widget)
    {
        return widget.m_sessionHistoryFirstSequence;
    }

    static quint64 historyLatestSequence(const AgentWorkbenchWidget &widget)
    {
        return widget.m_sessionHistoryLatestSequence;
    }

    static QString gitPinGateState(const AgentWorkbenchWidget &widget)
    {
        return QStringLiteral(
            "ready=%1 recovery=%2 sessionRecovery=%3 deletion=%4 operationBlocked=%5 "
            "project=%6 pinned=%7 git=%8 gitRequest=%9 mutation=%10 diffRequest=%11")
            .arg(widget.m_runtime && widget.m_runtime->isReady())
            .arg(widget.m_runtimeRecoveryMode)
            .arg(widget.currentSessionRecoveryRequired())
            .arg(widget.currentSessionDeletionPending())
            .arg(widget.currentOperationStatusBlocked())
            .arg(!widget.m_projectId.isEmpty())
            .arg(widget.m_pinnedContextAvailable)
            .arg(widget.m_gitContextAvailable)
            .arg(!widget.m_gitContextRequestId.isEmpty())
            .arg(!widget.m_pinnedContextMutationRequestId.isEmpty())
            .arg(!widget.m_gitDiffRequestId.isEmpty());
    }
};

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) {
        qCritical().noquote() << "AEGISY_TEST_FAILURE:" << message;
    }
    return condition;
}

bool verifyBoundedContextThresholdCache(AgentWorkbenchWidget &workbench,
                                        const QString &protectedSessionId)
{
    const QJsonObject threshold{
        {QStringLiteral("schema_version"),
         QStringLiteral("session-context-threshold/0.1")},
        {QStringLiteral("status"), QStringLiteral("no_action")},
        {QStringLiteral("source"), QStringLiteral("runtime-authoritative")},
        {QStringLiteral("history_state"), QStringLiteral("replayed")},
        {QStringLiteral("automatic_compaction_authority"), false},
    };
    for (int index = 0; index < 132; ++index) {
        AgentWorkbenchWidgetTestAccess::storeContextThreshold(
            workbench, QStringLiteral("threshold-cache-%1").arg(index), threshold);
    }
    return expect(AgentWorkbenchWidgetTestAccess::contextThresholdCacheSize(workbench) == 128,
                  "context threshold cache did not enforce its fixed capacity")
        && expect(!AgentWorkbenchWidgetTestAccess::containsContextThreshold(
                      workbench, QStringLiteral("threshold-cache-0")),
                  "context threshold cache did not evict the oldest unprotected entry")
        && expect(AgentWorkbenchWidgetTestAccess::containsContextThreshold(
                      workbench, QStringLiteral("threshold-cache-131")),
                  "context threshold cache evicted its newest entry")
        && expect(AgentWorkbenchWidgetTestAccess::containsContextThreshold(
                      workbench, protectedSessionId),
                  "context threshold cache evicted the protected Chat session");
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

QJsonObject ordinaryDegradation(const QString &feature, const QString &state,
                                const QString &scope)
{
    return QJsonObject{
        {QStringLiteral("feature"), feature},
        {QStringLiteral("state"), state},
        {QStringLiteral("reason"), QStringLiteral("bounded fixture reason")},
        {QStringLiteral("scope"), scope},
        {QStringLiteral("authority_granted"), false},
    };
}

QJsonObject autonomyDegradation(const QString &feature, const QJsonArray &missingGates)
{
    QJsonObject degradation = ordinaryDegradation(
        feature, QStringLiteral("disabled"), QStringLiteral("runtime"));
    degradation.insert(QStringLiteral("availability"), QStringLiteral("not-advertised"));
    degradation.insert(QStringLiteral("stable_enabled"), false);
    degradation.insert(QStringLiteral("override_available"), false);
    degradation.insert(QStringLiteral("missing_gates"), missingGates);
    return degradation;
}

QJsonObject runtimeOnlyDegradation(const QString &feature)
{
    QJsonObject degradation = ordinaryDegradation(
        feature, QStringLiteral("runtime-only"), QStringLiteral("provider"));
    degradation.insert(QStringLiteral("runtime_supported"), true);
    degradation.insert(QStringLiteral("desktop_surface_available"), false);
    return degradation;
}

QJsonObject validCodexRuntimeDegradationSnapshot()
{
    return QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("runtime-degradations/0.2")},
        {QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("codex")},
            {QStringLiteral("adapter"), QStringLiteral("codex-app-server")},
            {QStringLiteral("version"), QStringLiteral("codex-cli 0.144.5")},
            {QStringLiteral("status"), QStringLiteral("ready")},
        }},
        {QStringLiteral("capability_matrix"), QJsonObject{
            {QStringLiteral("schema_version"),
             QStringLiteral("codex-capability-matrix/0.1")},
            {QStringLiteral("identity"),
             QStringLiteral("codex-capability-matrix:sha256:"
                            "473ddd66cd30b903778c248f28aa55d3cfb2ff37123c4831a23a263703362d04")},
            {QStringLiteral("adapter"), QStringLiteral("codex-app-server")},
            {QStringLiteral("codex_version"), QStringLiteral("codex-cli 0.144.5")},
            {QStringLiteral("vendor_schema_version"), QStringLiteral("v2")},
            {QStringLiteral("vendor_schema_sha256"),
             QStringLiteral("e66ff6063c146734a92c9a018e43efefb079278ee597782f30674edcccedbdb2")},
            {QStringLiteral("client_request_count"), 87},
            {QStringLiteral("server_notification_count"), 68},
            {QStringLiteral("thread_item_count"), 18},
            {QStringLiteral("complete"), true},
        }},
        {QStringLiteral("complete"), true},
        {QStringLiteral("degradations"), QJsonArray{
            ordinaryDegradation(QStringLiteral("agent-mutation"),
                                QStringLiteral("disabled"), QStringLiteral("runtime")),
            ordinaryDegradation(QStringLiteral("provider-thread-item-content"),
                                QStringLiteral("metadata-only"), QStringLiteral("provider")),
            ordinaryDegradation(QStringLiteral("provider-thread-delete"),
                                QStringLiteral("blocked"), QStringLiteral("provider")),
            ordinaryDegradation(QStringLiteral("provider-thread-compact"),
                                QStringLiteral("blocked"), QStringLiteral("provider")),
            runtimeOnlyDegradation(QStringLiteral("turn.steer.same-turn")),
            runtimeOnlyDegradation(QStringLiteral("session.provider.lifecycle.list-read")),
            autonomyDegradation(QStringLiteral("background-jobs"),
                                QJsonArray{QStringLiteral("21.2"), QStringLiteral("21.8")}),
            autonomyDegradation(QStringLiteral("multi-agent"),
                                QJsonArray{QStringLiteral("21.3"), QStringLiteral("21.5")}),
            autonomyDegradation(QStringLiteral("unattended-writes"),
                                QJsonArray{QStringLiteral("15.3"), QStringLiteral("18.4")}),
        }},
    };
}

QJsonObject validRuntimeDegradationSnapshotForBackend(const QString &kind)
{
    QJsonObject snapshot = validCodexRuntimeDegradationSnapshot();
    if (kind == QStringLiteral("codex")) return snapshot;
    QJsonObject backend;
    QJsonObject backendEntry;
    if (kind == QStringLiteral("preview")) {
        backend = QJsonObject{{QStringLiteral("kind"), kind},
                              {QStringLiteral("adapter"), QStringLiteral("preview")},
                              {QStringLiteral("version"), QStringLiteral("0.1.0")},
                              {QStringLiteral("status"), QStringLiteral("ready")}};
        backendEntry = ordinaryDegradation(
            QStringLiteral("codex-provider"), QStringLiteral("unavailable"),
            QStringLiteral("runtime"));
    } else if (kind == QStringLiteral("recovery")) {
        backend = QJsonObject{
            {QStringLiteral("kind"), kind},
            {QStringLiteral("adapter"), QStringLiteral("aegisy-workbench-store")},
            {QStringLiteral("version"), QStringLiteral("workbench-recovery-diagnostic/0.1")},
            {QStringLiteral("status"), QStringLiteral("read-only-recovery")}};
        backendEntry = ordinaryDegradation(
            QStringLiteral("workbench-mutation"), QStringLiteral("disabled"),
            QStringLiteral("runtime"));
    } else {
        backend = QJsonObject{{QStringLiteral("kind"), QStringLiteral("unavailable")},
                              {QStringLiteral("adapter"), QStringLiteral("codex-app-server")},
                              {QStringLiteral("version"), QStringLiteral("codex-cli 0.144.5")},
                              {QStringLiteral("status"), QStringLiteral("unavailable")}};
        backendEntry = ordinaryDegradation(
            QStringLiteral("runtime-adapter"), QStringLiteral("unavailable"),
            QStringLiteral("runtime"));
    }
    const QJsonArray codexEntries = snapshot.value(QStringLiteral("degradations")).toArray();
    snapshot.insert(QStringLiteral("backend"), backend);
    snapshot.insert(QStringLiteral("degradations"), QJsonArray{
        backendEntry, codexEntries.at(6), codexEntries.at(7), codexEntries.at(8),
    });
    return snapshot;
}

QJsonObject mutateDegradation(QJsonObject snapshot, int index,
                              const QString &key, const QJsonValue &value,
                              bool remove = false)
{
    QJsonArray degradations = snapshot.value(QStringLiteral("degradations")).toArray();
    QJsonObject degradation = degradations.at(index).toObject();
    if (remove) degradation.remove(key);
    else degradation.insert(key, value);
    degradations.replace(index, degradation);
    snapshot.insert(QStringLiteral("degradations"), degradations);
    return snapshot;
}

QJsonObject timelineEnvelope(const QString &event, const QString &sessionId,
                             const QString &turnId,
                             const QJsonValue &item = QJsonValue(QJsonValue::Null),
                             quint64 explicitSequence = 0,
                             quint64 explicitTimestamp = 0,
                             quint64 explicitRevision = 0)
{
    static QHash<QString, quint64> nextSequences;
    static QHash<QString, quint64> nextRevisions;
    const quint64 sequence = explicitSequence == 0
        ? ++nextSequences[sessionId] : explicitSequence;
    const quint64 timestamp = explicitTimestamp == 0
        ? 1'000 + sequence : explicitTimestamp;
    const QString turnState = event == QStringLiteral("turn.completed")
        ? QStringLiteral("completed")
        : event == QStringLiteral("turn.failed")
            ? QStringLiteral("failed")
            : event == QStringLiteral("turn.interrupted")
                ? QStringLiteral("interrupted") : QStringLiteral("running");
    const bool hasItem = item.isObject();
    QJsonValue itemUpdate(QJsonValue::Null);
    quint64 revision = 0;
    if (hasItem) {
        const QString itemId = item.toObject().value(QStringLiteral("id")).toString();
        const QString revisionKey = sessionId + QChar(0x1f) + turnId
            + QChar(0x1f) + itemId;
        revision = explicitRevision == 0
            ? ++nextRevisions[revisionKey] : explicitRevision;
        itemUpdate = QJsonObject{
            {QStringLiteral("revision"), static_cast<double>(revision)},
            {QStringLiteral("content_mode"), QStringLiteral("snapshot-replacement")},
        };
    }
    QJsonObject envelope{
        {QStringLiteral("schema_version"), QStringLiteral("timeline-event/0.1")},
        {QStringLiteral("event_id"), QString()},
        {QStringLiteral("sequence"), static_cast<double>(sequence)},
        {QStringLiteral("timestamp_ms"), static_cast<double>(timestamp)},
        {QStringLiteral("correlation_id"), turnId},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("turn_id"), turnId},
        {QStringLiteral("turn_state"), turnState},
        {QStringLiteral("event"), event},
        {QStringLiteral("item"), item},
        {QStringLiteral("item_update"), itemUpdate},
    };
    envelope.insert(QStringLiteral("event_id"),
                    AgentRuntimeClient::timelineEventIdentity(envelope));
    return envelope;
}

QJsonObject timelineAnchorForEvent(const QJsonObject &event)
{
    return {
        {QStringLiteral("sequence"), event.value(QStringLiteral("sequence"))},
        {QStringLiteral("event_id"), event.value(QStringLiteral("event_id"))},
    };
}

QJsonObject timelineSubscriptionActiveResult(
    quint64 generation, const QString &sessionId, const QString &subscriptionId,
    const QJsonObject &cursor)
{
    return {
        {QStringLiteral("schema_version"),
         QStringLiteral("timeline-subscription-active/0.1")},
        {QStringLiteral("connection_generation"), static_cast<double>(generation)},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("subscription_id"), subscriptionId},
        {QStringLiteral("state"), QStringLiteral("active")},
        {QStringLiteral("cursor"), cursor},
        {QStringLiteral("watermark"), cursor},
    };
}

QJsonObject timelineSubscriptionEventWrapper(
    quint64 generation, const QString &sessionId, const QString &subscriptionId,
    const QJsonObject &cursor, const QJsonObject &watermark,
    const QJsonObject &event)
{
    return {
        {QStringLiteral("schema_version"),
         QStringLiteral("timeline-subscription-event/0.1")},
        {QStringLiteral("connection_generation"), static_cast<double>(generation)},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("subscription_id"), subscriptionId},
        {QStringLiteral("state"), QStringLiteral("active")},
        {QStringLiteral("cursor"), cursor},
        {QStringLiteral("watermark"), watermark},
        {QStringLiteral("event"), event},
    };
}

QJsonObject timelineSyncPage(const QString &sessionId, const QJsonObject &after,
                             const QJsonObject &watermark, const QJsonArray &events,
                             bool complete)
{
    return {
        {QStringLiteral("schema_version"), QStringLiteral("timeline-sync-page/0.1")},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("after"), after},
        {QStringLiteral("watermark"), watermark},
        {QStringLiteral("events"), events},
        {QStringLiteral("next_after"), complete || events.isEmpty()
             ? QJsonValue(QJsonValue::Null)
             : QJsonValue(timelineAnchorForEvent(events.last().toObject()))},
        {QStringLiteral("complete"), complete},
    };
}

QJsonObject timelineSnapshotItemPage(const QString &sessionId, quint64 ordinal,
                                     const QString &turnId, const QString &turnState,
                                     const QJsonObject &firstEvent,
                                     const QJsonObject &latestEvent,
                                     const QJsonObject &item)
{
    QJsonObject pageItem{
        {QStringLiteral("ordinal"), static_cast<double>(ordinal)},
        {QStringLiteral("item_identity"), QStringLiteral(
             "timeline-session-snapshot-item:sha256:") + QString(64, QLatin1Char('0'))},
        {QStringLiteral("turn_id"), turnId},
        {QStringLiteral("correlation_id"), turnId},
        {QStringLiteral("turn_state"), turnState},
        {QStringLiteral("first_event"), firstEvent},
        {QStringLiteral("latest_event"), latestEvent},
        {QStringLiteral("item"), item},
        {QStringLiteral("item_update"), QJsonObject{
            {QStringLiteral("revision"), 1},
            {QStringLiteral("content_mode"), QStringLiteral("snapshot-replacement")},
        }},
    };
    pageItem.insert(QStringLiteral("item_identity"),
                    AgentRuntimeClient::timelineSnapshotItemIdentity(sessionId, pageItem));
    return pageItem;
}

QJsonObject timelineSnapshotPage(const QString &sessionId,
                                 const QJsonObject &floor,
                                 const QJsonObject &watermark,
                                 const QJsonObject &activeTurn,
                                 const QList<QJsonObject> &allItems,
                                 const QList<QJsonObject> &pageItems,
                                 const QJsonObject &after,
                                 bool complete)
{
    QStringList identities;
    quint64 totalBytes = 0;
    for (const QJsonObject &item : allItems) {
        identities.append(item.value(QStringLiteral("item_identity")).toString());
        totalBytes += AgentRuntimeClient::timelineSnapshotItemCanonicalBytes(sessionId, item);
    }
    QJsonValue nextAfter(QJsonValue::Null);
    if (!complete && !pageItems.isEmpty()) {
        const QJsonObject last = pageItems.last();
        nextAfter = QJsonObject{
            {QStringLiteral("ordinal"), last.value(QStringLiteral("ordinal"))},
            {QStringLiteral("item_id"), last.value(QStringLiteral("item")).toObject()
                .value(QStringLiteral("id"))},
            {QStringLiteral("item_identity"), last.value(QStringLiteral("item_identity"))},
        };
    }
    const QString snapshotIdentity = AgentRuntimeClient::timelineSnapshotIdentity(
        sessionId, floor, watermark, activeTurn, static_cast<quint64>(allItems.size()),
        totalBytes, identities);
    QJsonArray pageItemsArray;
    for (const QJsonObject &item : pageItems) pageItemsArray.append(item);
    QJsonObject page{
        {QStringLiteral("schema_version"), QStringLiteral("timeline-session-snapshot-page/0.1")},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("snapshot_identity"), snapshotIdentity},
        {QStringLiteral("floor"), floor},
        {QStringLiteral("watermark"), watermark},
        {QStringLiteral("active_turn"), activeTurn.isEmpty()
             ? QJsonValue(QJsonValue::Null) : QJsonValue(activeTurn)},
        {QStringLiteral("total_items"), static_cast<double>(allItems.size())},
        {QStringLiteral("total_canonical_bytes"), static_cast<double>(totalBytes)},
        {QStringLiteral("after"), after.isEmpty()
             ? QJsonValue(QJsonValue::Null) : QJsonValue(after)},
        {QStringLiteral("items"), pageItemsArray},
        {QStringLiteral("next_after"), nextAfter},
        {QStringLiteral("complete"), complete},
        {QStringLiteral("page_identity"), QString()},
    };
    page.insert(QStringLiteral("page_identity"),
                AgentRuntimeClient::timelineSnapshotPageIdentity(page));
    return page;
}

QJsonObject timelineMessage(const QString &id, const QString &state,
                            const QString &content,
                            const QString &role = QStringLiteral("agent"))
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("kind"), QStringLiteral("message")},
        {QStringLiteral("role"), role},
        {QStringLiteral("state"), state},
        {QStringLiteral("content"), content},
    };
}

QJsonObject replaySnapshot(const QString &sessionId, const QJsonArray &items,
                           int firstSequence, int lastSequence, int latestSequence,
                           int limit = 100)
{
    const bool empty = items.isEmpty();
    const bool hasOlder = !empty && firstSequence > 1;
    return QJsonObject{
        {QStringLiteral("session"), QJsonObject{
            {QStringLiteral("id"), sessionId},
            {QStringLiteral("mode"), QStringLiteral("chat")},
            {QStringLiteral("project_id"), QJsonValue::Null},
            {QStringLiteral("title"), QStringLiteral("Timeline fixture")},
        }},
        {QStringLiteral("status"), QStringLiteral("active")},
        {QStringLiteral("items"), items},
        {QStringLiteral("history_page"), QJsonObject{
            {QStringLiteral("limit"), limit},
            {QStringLiteral("first_sequence"), empty
                ? QJsonValue(QJsonValue::Null) : QJsonValue(firstSequence)},
            {QStringLiteral("last_sequence"), empty
                ? QJsonValue(QJsonValue::Null) : QJsonValue(lastSequence)},
            {QStringLiteral("latest_sequence"), latestSequence},
            {QStringLiteral("has_older"), hasOlder},
            {QStringLiteral("older_cursor"), hasOlder
                ? QJsonValue(QStringLiteral("before:%1").arg(firstSequence))
                : QJsonValue(QJsonValue::Null)},
        }},
    };
}

QJsonObject proposalReadResult(const QString &sessionId, QLatin1Char fill);
QJsonObject proposalTimelineReference(const QJsonObject &proposalResult,
                                      const QString &itemId);
QJsonObject proposalTimelineItem(const QString &itemId,
                                 const QJsonObject &reference);

bool verifyRuntimeDegradationFailures(QApplication &application,
                                      AgentWorkbenchWidget &workbench,
                                      AgentRuntimeClient *runtimeClient,
                                      QLabel *runtimeCapability)
{
    if (!runtimeClient || !runtimeCapability) return false;
    const QJsonObject valid = validCodexRuntimeDegradationSnapshot();
    const QList<QPair<QString, QString>> validBackends{
        {QStringLiteral("codex"), QStringLiteral("Agent 只读")},
        {QStringLiteral("preview"), QStringLiteral("Provider 不可用")},
        {QStringLiteral("recovery"), QStringLiteral("工作台只读")},
        {QStringLiteral("unavailable"), QStringLiteral("Runtime 不可用")},
    };
    for (const auto &fixture : validBackends) {
        const QString requestId = QStringLiteral("valid-") + fixture.first;
        AgentWorkbenchWidgetTestAccess::prepareRuntimeDegradationRequest(
            workbench, requestId);
        runtimeClient->runtimeDegradationsRead(
            requestId,
            validRuntimeDegradationSnapshotForBackend(fixture.first));
        application.processEvents();
        if (!expect(runtimeCapability->text().contains(fixture.second),
                    qPrintable(QStringLiteral("valid backend matrix was rejected: %1")
                                   .arg(fixture.first)))) {
            return false;
        }
    }
    QList<QPair<QString, QJsonObject>> invalid;
    QJsonObject candidate = valid;
    candidate.remove(QStringLiteral("degradations"));
    invalid.append({QStringLiteral("missing-array"), candidate});
    candidate = valid;
    candidate.insert(QStringLiteral("degradations"), QJsonObject{});
    invalid.append({QStringLiteral("non-array"), candidate});
    candidate = valid;
    candidate.insert(QStringLiteral("degradations"), QJsonArray{});
    invalid.append({QStringLiteral("empty-array"), candidate});
    candidate = valid;
    QJsonArray degradations = candidate.value(QStringLiteral("degradations")).toArray();
    degradations.replace(0, QStringLiteral("not-an-object"));
    candidate.insert(QStringLiteral("degradations"), degradations);
    invalid.append({QStringLiteral("non-object-entry"), candidate});
    candidate = valid;
    degradations = candidate.value(QStringLiteral("degradations")).toArray();
    degradations.replace(8, degradations.at(0));
    candidate.insert(QStringLiteral("degradations"), degradations);
    invalid.append({QStringLiteral("duplicate-identical"), candidate});
    candidate = valid;
    degradations = candidate.value(QStringLiteral("degradations")).toArray();
    QJsonObject contradictory = degradations.at(0).toObject();
    contradictory.insert(QStringLiteral("state"), QStringLiteral("blocked"));
    degradations.replace(8, contradictory);
    candidate.insert(QStringLiteral("degradations"), degradations);
    invalid.append({QStringLiteral("duplicate-contradictory"), candidate});
    invalid.append({QStringLiteral("unknown-feature"), mutateDegradation(
                        valid, 0, QStringLiteral("feature"), QStringLiteral("unknown"))});
    invalid.append({QStringLiteral("unknown-state"), mutateDegradation(
                        valid, 0, QStringLiteral("state"), QStringLiteral("unknown"))});
    invalid.append({QStringLiteral("wrong-scope"), mutateDegradation(
                        valid, 0, QStringLiteral("scope"), QStringLiteral("provider"))});
    invalid.append({QStringLiteral("missing-autonomy-boolean"), mutateDegradation(
                        valid, 6, QStringLiteral("stable_enabled"), {}, true)});
    invalid.append({QStringLiteral("stable-enabled"), mutateDegradation(
                        valid, 6, QStringLiteral("stable_enabled"), true)});
    invalid.append({QStringLiteral("override-available"), mutateDegradation(
                        valid, 6, QStringLiteral("override_available"), true)});
    invalid.append({QStringLiteral("missing-gates-empty"), mutateDegradation(
                        valid, 6, QStringLiteral("missing_gates"), QJsonArray{})});
    candidate = valid;
    QJsonObject matrix = candidate.value(QStringLiteral("capability_matrix")).toObject();
    matrix.insert(QStringLiteral("vendor_schema_sha256"), QString(64, QLatin1Char('0')));
    candidate.insert(QStringLiteral("capability_matrix"), matrix);
    invalid.append({QStringLiteral("schema-hash-drift"), candidate});
    candidate = valid;
    matrix = candidate.value(QStringLiteral("capability_matrix")).toObject();
    matrix.insert(QStringLiteral("identity"),
                  QStringLiteral("codex-capability-matrix:sha256:")
                      + QString(64, QLatin1Char('0')));
    candidate.insert(QStringLiteral("capability_matrix"), matrix);
    invalid.append({QStringLiteral("matrix-identity-drift"), candidate});
    candidate = valid;
    matrix = candidate.value(QStringLiteral("capability_matrix")).toObject();
    matrix.insert(QStringLiteral("server_notification_count"), 69);
    candidate.insert(QStringLiteral("capability_matrix"), matrix);
    invalid.append({QStringLiteral("schema-count-drift"), candidate});
    candidate = valid;
    matrix = candidate.value(QStringLiteral("capability_matrix")).toObject();
    matrix.insert(QStringLiteral("complete"), false);
    candidate.insert(QStringLiteral("capability_matrix"), matrix);
    invalid.append({QStringLiteral("incomplete-matrix"), candidate});
    candidate = valid;
    candidate.insert(QStringLiteral("complete"), false);
    invalid.append({QStringLiteral("incomplete-snapshot"), candidate});
    candidate = valid;
    QJsonObject backend = candidate.value(QStringLiteral("backend")).toObject();
    backend.insert(QStringLiteral("version"), QStringLiteral("codex-cli 0.144.6"));
    candidate.insert(QStringLiteral("backend"), backend);
    invalid.append({QStringLiteral("backend-version-drift"), candidate});
    candidate = valid;
    backend = candidate.value(QStringLiteral("backend")).toObject();
    backend.insert(QStringLiteral("kind"), QStringLiteral("preview"));
    backend.insert(QStringLiteral("adapter"), QStringLiteral("preview"));
    backend.insert(QStringLiteral("version"), QStringLiteral("0.1.0"));
    candidate.insert(QStringLiteral("backend"), backend);
    invalid.append({QStringLiteral("backend-feature-set-mismatch"), candidate});

    for (const auto &fixture : invalid) {
        AgentWorkbenchWidgetTestAccess::prepareRuntimeDegradationRequest(
            workbench, fixture.first);
        runtimeClient->runtimeDegradationsRead(fixture.first, fixture.second);
        application.processEvents();
        if (!expect(runtimeCapability->text() == QStringLiteral("能力未知")
                        && !runtimeCapability->text().contains(QStringLiteral("能力已协商"))
                        && runtimeCapability->toolTip().contains(QStringLiteral("只读门控")),
                    qPrintable(QStringLiteral("runtime degradation fixture did not fail closed: %1")
                                   .arg(fixture.first)))) {
            return false;
        }
    }
    return true;
}

bool verifyRuntimeHealthDegradationRefresh(QApplication &application,
                                           AgentWorkbenchWidget &workbench,
                                           AgentRuntimeClient *runtimeClient,
                                           QLabel *runtimeCapability,
                                           QTextEdit *composer,
                                           QPushButton *sendButton)
{
    if (!runtimeClient || !runtimeCapability || !composer || !sendButton) return false;
    AgentWorkbenchWidgetTestAccess::setPendingPrompt(
        workbench, QStringLiteral("queued-health-turn"));
    runtimeClient->runtimeHealthRead(QJsonObject{
        {QStringLiteral("state"), QStringLiteral("exited")},
        {QStringLiteral("restart_required"), true},
    });
    if (!expect(runtimeCapability->text() == QStringLiteral("能力未知")
                    && sendButton->text() == QStringLiteral("能力未知")
                    && !sendButton->isEnabled()
                    && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                        == QStringLiteral("queued-health-turn"),
                "unhealthy Runtime did not invalidate capabilities and gate queued Turns")) {
        return false;
    }
    runtimeClient->runtimeDegradationsRead(
        QStringLiteral("stale-before-restart"), validCodexRuntimeDegradationSnapshot());
    AgentWorkbenchWidgetTestAccess::tryStartPendingTurn(workbench);
    if (!expect(runtimeCapability->text() == QStringLiteral("能力未知")
                    && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                        == QStringLiteral("queued-health-turn")
                    && AgentWorkbenchWidgetTestAccess::activeTurnSubmitIsInert(
                        workbench, composer, sendButton),
                "stale degradation snapshot reopened Turn authority or hid active Stop")) {
        return false;
    }

    runtimeClient->runtimeRestarted(QStringLiteral("restart-health-fixture"), QJsonObject{
        {QStringLiteral("health"), QJsonObject{
            {QStringLiteral("state"), QStringLiteral("running")},
            {QStringLiteral("restart_required"), false},
        }},
    });
    QString freshRequestId =
        AgentWorkbenchWidgetTestAccess::runtimeDegradationRequestId(workbench);
    if (freshRequestId.isEmpty()) {
        // Direct signal fixtures do not change AgentRuntimeClient::m_ready, so seed the
        // correlation identity that a real successful restart request supplies.
        freshRequestId = QStringLiteral("fresh-after-restart");
        AgentWorkbenchWidgetTestAccess::prepareRuntimeDegradationRequest(
            workbench, freshRequestId);
    }
    if (!expect(!freshRequestId.isEmpty()
                    && AgentWorkbenchWidgetTestAccess::runtimeDegradationPending(workbench)
                    && sendButton->text() == QStringLiteral("能力检查中")
                    && !sendButton->isEnabled()
                    && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                        == QStringLiteral("queued-health-turn"),
                "successful Runtime restart did not request a fresh correlated snapshot")) {
        return false;
    }
    runtimeClient->runtimeDegradationsRead(
        QStringLiteral("stale-after-restart"), validCodexRuntimeDegradationSnapshot());
    if (!expect(AgentWorkbenchWidgetTestAccess::runtimeDegradationPending(workbench)
                    && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                        == QStringLiteral("queued-health-turn"),
                "uncorrelated post-restart snapshot reopened Turn authority")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::setPendingPrompt(workbench, QString());
    runtimeClient->runtimeDegradationsRead(
        freshRequestId, validCodexRuntimeDegradationSnapshot());
    application.processEvents();
    return expect(runtimeCapability->text().contains(QStringLiteral("Agent 只读"))
                      && sendButton->text() != QStringLiteral("能力检查中"),
                  "fresh correlated degradation snapshot did not restore Turn gating");
}

bool verifyStrictTimelineValidation(QApplication &application,
                                    AgentWorkbenchWidget &workbench,
                                    AgentRuntimeClient *runtimeClient)
{
    if (!runtimeClient) return false;
    const QString sessionId = QStringLiteral("timeline-validation-session");
    const QString turnId = QStringLiteral("timeline-validation-turn");
    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionId);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 1, 1'001));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.started"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("started"),
                        QString()), 2, 1'002, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("partial")), 3, 1'003, 2));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.cancellation-acknowledged"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 4, 1'004));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && AgentWorkbenchWidgetTestAccess::turnCancelling(workbench)
                    && AgentWorkbenchWidgetTestAccess::lastTimelineSequence(workbench) == 4
                    && AgentWorkbenchWidgetTestAccess::timelineTimestampForSession(
                           workbench, sessionId) == 1'004
                    && AgentWorkbenchWidgetTestAccess::timelineItemText(
                           workbench, QStringLiteral("live-item")) == QStringLiteral("partial"),
                "valid bound live event was rejected")) {
        return false;
    }

    // Every invalid candidate targets the next cursor and must be atomic.
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 4, 1'005));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 6, 1'006));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 5, 1'005));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.interrupted"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 5, 1'005));
    QJsonObject timestampRollback = timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("rollback")), 5, 1'003, 3);
    runtimeClient->timelineEvent(timestampRollback);
    QJsonObject correlationReplacement = timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("correlation")), 5, 1'005, 3);
    correlationReplacement.insert(QStringLiteral("correlation_id"),
                                  QStringLiteral("other-turn"));
    runtimeClient->timelineEvent(correlationReplacement);
    QJsonObject wrongTurnState = timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("wrong-state")), 5, 1'005, 3);
    wrongTurnState.insert(QStringLiteral("turn_state"), QStringLiteral("completed"));
    runtimeClient->timelineEvent(wrongTurnState);
    QJsonObject missingUpdate = timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("missing-update")), 5, 1'005, 3);
    missingUpdate.insert(QStringLiteral("item_update"), QJsonValue(QJsonValue::Null));
    runtimeClient->timelineEvent(missingUpdate);
    QJsonObject wrongSchema = timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("wrong-schema")), 5, 1'005, 3);
    wrongSchema.insert(QStringLiteral("schema_version"),
                       QStringLiteral("timeline-event/0.2"));
    runtimeClient->timelineEvent(wrongSchema);
    QJsonObject wrongEventId = timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("wrong-event-id")), 5, 1'005, 3);
    wrongEventId.insert(QStringLiteral("event_id"), QStringLiteral("event:sha256:ABC"));
    runtimeClient->timelineEvent(wrongEventId);
    QJsonObject identityTamper = timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("before-tamper")), 5, 1'005, 3);
    QJsonObject tamperedItem = identityTamper.value(QStringLiteral("item")).toObject();
    tamperedItem.insert(QStringLiteral("content"), QStringLiteral("after-tamper"));
    identityTamper.insert(QStringLiteral("item"), tamperedItem);
    runtimeClient->timelineEvent(identityTamper);
    QJsonObject extraEnvelopeField = timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("extra-field")), 5, 1'005, 3);
    extraEnvelopeField.insert(QStringLiteral("extra"), true);
    runtimeClient->timelineEvent(extraEnvelopeField);
    QJsonObject wrongContentMode = timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("wrong-content-mode")), 5, 1'005, 3);
    QJsonObject wrongUpdate = wrongContentMode.value(
        QStringLiteral("item_update")).toObject();
    wrongUpdate.insert(QStringLiteral("content_mode"), QStringLiteral("append"));
    wrongContentMode.insert(QStringLiteral("item_update"), wrongUpdate);
    runtimeClient->timelineEvent(wrongContentMode);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.persistence-failed"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 5, 1'005));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("revision-jump")), 5, 1'005, 4));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("unknown.event"), sessionId, turnId,
        timelineMessage(QStringLiteral("unknown-event-item"),
                        QStringLiteral("completed"), QStringLiteral("must-not-render")),
        5, 1'005, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), QStringLiteral("other-session"), turnId,
        QJsonValue(QJsonValue::Null), 5, 1'005));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), sessionId, QStringLiteral("other-turn"),
        QJsonValue(QJsonValue::Null), 5, 1'005));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("unknown-event-item"))
                    && AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && AgentWorkbenchWidgetTestAccess::turnCancelling(workbench)
                    && AgentWorkbenchWidgetTestAccess::lastTimelineSequence(workbench) == 4
                    && AgentWorkbenchWidgetTestAccess::timelineTimestampForSession(
                           workbench, sessionId) == 1'004
                    && AgentWorkbenchWidgetTestAccess::unknownTimelineEventCount(workbench) == 0,
                "rejected envelope mutated the Timeline, cancellation, Turn, or sequence")) {
        return false;
    }

    const QList<QJsonObject> invalidItems{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("bad-kind")},
                    {QStringLiteral("kind"), QStringLiteral("Bad Kind")},
                    {QStringLiteral("role"), QStringLiteral("agent")},
                    {QStringLiteral("state"), QStringLiteral("delta")},
                    {QStringLiteral("content"), QStringLiteral("bad")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("bad-role")},
                    {QStringLiteral("kind"), QStringLiteral("message")},
                    {QStringLiteral("role"), QStringLiteral("unknown")},
                    {QStringLiteral("state"), QStringLiteral("delta")},
                    {QStringLiteral("content"), QStringLiteral("bad")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("bad-state")},
                    {QStringLiteral("kind"), QStringLiteral("message")},
                    {QStringLiteral("role"), QStringLiteral("agent")},
                    {QStringLiteral("state"), QStringLiteral("unknown")},
                    {QStringLiteral("content"), QStringLiteral("bad")}},
        timelineMessage(QString(129, QLatin1Char('i')), QStringLiteral("delta"),
                        QStringLiteral("bad")),
        timelineMessage(QStringLiteral("项目-item"), QStringLiteral("delta"),
                        QStringLiteral("bad")),
        timelineMessage(QStringLiteral("oversized-content"), QStringLiteral("delta"),
                        QString(65 * 1024, QLatin1Char('x'))),
    };
    for (const QJsonObject &item : invalidItems) {
        runtimeClient->timelineEvent(timelineEnvelope(
            QStringLiteral("item.delta"), sessionId, turnId, item, 5, 1'005, 3));
    }
    QJsonObject unsafeIntegerData = timelineMessage(
        QStringLiteral("unsafe-integer-data"), QStringLiteral("delta"),
        QStringLiteral("bad"));
    unsafeIntegerData.insert(QStringLiteral("data"), QJsonObject{
        {QStringLiteral("unsafe"), 9'007'199'254'740'992.0},
    });
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId, unsafeIntegerData,
        5, 1'005, 3));
    QJsonObject floatData = timelineMessage(
        QStringLiteral("float-data"), QStringLiteral("delta"), QStringLiteral("bad"));
    floatData.insert(QStringLiteral("data"), QJsonObject{
        {QStringLiteral("unsafe"), 1.5},
    });
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId, floatData, 5, 1'005, 3));
    QJsonObject invalidDataKey = timelineMessage(
        QStringLiteral("invalid-data-key"), QStringLiteral("delta"),
        QStringLiteral("bad"));
    invalidDataKey.insert(QStringLiteral("data"), QJsonObject{
        {QStringLiteral("项目"), true},
    });
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId, invalidDataKey,
        5, 1'005, 3));
    application.processEvents();
    for (const QString &id : {QStringLiteral("bad-kind"), QStringLiteral("bad-role"),
                              QStringLiteral("bad-state"),
                              QStringLiteral("oversized-content"),
                              QStringLiteral("unsafe-integer-data"),
                              QStringLiteral("float-data"),
                              QStringLiteral("invalid-data-key")}) {
        if (!expect(!AgentWorkbenchWidgetTestAccess::hasTimelineItem(workbench, id),
                    "malformed or oversized live item was rendered")) {
            return false;
        }
    }

    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("unknown.future-event"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 5, 1'004));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && AgentWorkbenchWidgetTestAccess::lastTimelineSequence(workbench) == 5
                    && AgentWorkbenchWidgetTestAccess::timelineTimestampForSession(
                           workbench, sessionId) == 1'004
                    && AgentWorkbenchWidgetTestAccess::unknownTimelineEventCount(workbench) == 1
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("unknown-event-item")),
                "well-formed unknown event did not advance only its bounded diagnostics")) {
        return false;
    }

    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("after unknown")), 6, 1'005, 3));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("completed"),
                        QStringLiteral("complete")), 7, 1'006, 4));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineItemText(
                    workbench, QStringLiteral("live-item")) == QStringLiteral("complete")
                    && AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && AgentWorkbenchWidgetTestAccess::lastTimelineSequence(workbench) == 7,
                "legal event after unknown or Item completion was rejected")) {
        return false;
    }
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, turnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("delta"),
                        QStringLiteral("after-item-terminal")), 8, 1'007, 5));
    QJsonObject terminalStateMismatch = timelineEnvelope(
        QStringLiteral("turn.completed"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 8, 1'007);
    terminalStateMismatch.insert(QStringLiteral("turn_state"),
                                 QStringLiteral("failed"));
    runtimeClient->timelineEvent(terminalStateMismatch);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 8, 1'007));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 9, 1'008));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), sessionId, turnId,
        timelineMessage(QStringLiteral("post-terminal"), QStringLiteral("completed"),
                        QStringLiteral("must-not-render")), 9, 1'008, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 9, 1'008));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && AgentWorkbenchWidgetTestAccess::lastTimelineSequence(workbench) == 8
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("post-terminal")),
                "terminal Turn accepted a second terminal, later Item, or reopen")) {
        return false;
    }

    runtimeClient->runtimeRestarted(QStringLiteral("adapter-restart-fixture"), QJsonObject{
        {QStringLiteral("health"), QJsonObject{
            {QStringLiteral("state"), QStringLiteral("running")},
            {QStringLiteral("restart_required"), false},
        }},
    });
    const QString postRestartTurnId = QStringLiteral("timeline-post-restart-turn");
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), sessionId, postRestartTurnId,
        QJsonValue(QJsonValue::Null), 9, 1'009));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), sessionId, postRestartTurnId,
        timelineMessage(QStringLiteral("post-restart-item"),
                        QStringLiteral("completed"), QStringLiteral("continued")),
        10, 1'010, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), sessionId, postRestartTurnId,
        QJsonValue(QJsonValue::Null), 11, 1'011));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && AgentWorkbenchWidgetTestAccess::lastTimelineSequence(workbench) == 11
                    && AgentWorkbenchWidgetTestAccess::timelineItemText(
                           workbench, QStringLiteral("post-restart-item"))
                        == QStringLiteral("continued"),
                "Codex backend restart reset the live sidecar Timeline cursor")) {
        return false;
    }

    const QString failedSessionId = QStringLiteral("timeline-partial-failure-session");
    const QString failedTurnId = QStringLiteral("timeline-partial-failure-turn");
    const QString partialItemId = QStringLiteral("partial-before-failure");
    const QString failureItemId = QStringLiteral("structured-failure");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, failedSessionId);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), failedSessionId, failedTurnId,
        QJsonValue(QJsonValue::Null), 1, 3'001));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.started"), failedSessionId, failedTurnId,
        timelineMessage(partialItemId, QStringLiteral("started"), QString()),
        2, 3'002, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.delta"), failedSessionId, failedTurnId,
        timelineMessage(partialItemId, QStringLiteral("delta"),
                        QStringLiteral("retained partial output")),
        3, 3'003, 2));
    const QJsonObject failureItem{
        {QStringLiteral("id"), failureItemId},
        {QStringLiteral("kind"), QStringLiteral("error")},
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("state"), QStringLiteral("completed")},
        {QStringLiteral("content"), QStringLiteral("structured failure")},
    };
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.failed"), failedSessionId, failedTurnId,
        failureItem, 4, 3'004, 1));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, failedSessionId) == 4
                    && AgentWorkbenchWidgetTestAccess::timelineTurnState(
                           workbench, failedSessionId, failedTurnId)
                        == QStringLiteral("failed")
                    && AgentWorkbenchWidgetTestAccess::timelineItemState(
                           workbench, failedSessionId, failedTurnId, partialItemId)
                        == QStringLiteral("delta")
                    && AgentWorkbenchWidgetTestAccess::timelineItemText(
                           workbench, partialItemId)
                        == QStringLiteral("retained partial output")
                    && AgentWorkbenchWidgetTestAccess::timelineItemState(
                           workbench, failedSessionId, failedTurnId, failureItemId)
                        == QStringLiteral("completed")
                    && AgentWorkbenchWidgetTestAccess::timelineItemText(
                           workbench, failureItemId)
                        == QStringLiteral("structured failure"),
                "structured failure discarded partial output or did not close the Turn")) {
        return false;
    }

    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), failedSessionId, failedTurnId,
        timelineMessage(QStringLiteral("late-after-failure"),
                        QStringLiteral("completed"),
                        QStringLiteral("must-not-render")),
        5, 3'005, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), failedSessionId, failedTurnId,
        QJsonValue(QJsonValue::Null), 5, 3'005));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), failedSessionId, failedTurnId,
        QJsonValue(QJsonValue::Null), 5, 3'005));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                       workbench, failedSessionId) == 4
                    && AgentWorkbenchWidgetTestAccess::timelineTurnState(
                           workbench, failedSessionId, failedTurnId)
                        == QStringLiteral("failed")
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("late-after-failure")),
                "failed Turn accepted a later Item, terminal, or reopen event")) {
        return false;
    }

    const QString updatingSessionId = QStringLiteral("timeline-updating-terminal-session");
    const QString updatingCompletedTurnId = QStringLiteral("timeline-updating-completed-turn");
    const QString updatingInterruptedTurnId = QStringLiteral("timeline-updating-interrupted-turn");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, updatingSessionId);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), updatingSessionId, updatingCompletedTurnId,
        QJsonValue(QJsonValue::Null), 1, 4'001));
    QJsonObject usageUpdate{
        {QStringLiteral("id"), QStringLiteral("usage-open-snapshot")},
        {QStringLiteral("kind"), QStringLiteral("usage")},
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("state"), QStringLiteral("updated")},
        {QStringLiteral("content"), QStringLiteral("latest usage snapshot")},
    };
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("usage.updated"), updatingSessionId, updatingCompletedTurnId,
        usageUpdate, 2, 4'002, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), updatingSessionId, updatingCompletedTurnId,
        QJsonValue(QJsonValue::Null), 3, 4'003));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), updatingSessionId, updatingInterruptedTurnId,
        QJsonValue(QJsonValue::Null), 4, 4'004));
    QJsonObject planUpdate{
        {QStringLiteral("id"), QStringLiteral("plan-open-snapshot")},
        {QStringLiteral("kind"), QStringLiteral("plan")},
        {QStringLiteral("role"), QStringLiteral("agent")},
        {QStringLiteral("state"), QStringLiteral("updated")},
        {QStringLiteral("content"), QStringLiteral("latest plan snapshot")},
    };
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.plan.updated"), updatingSessionId,
        updatingInterruptedTurnId, planUpdate, 5, 4'005, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.interrupted"), updatingSessionId,
        updatingInterruptedTurnId, QJsonValue(QJsonValue::Null), 6, 4'006));
    const QString atomicTruncatedTurnId = QStringLiteral("timeline-atomic-truncated-turn");
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), updatingSessionId, atomicTruncatedTurnId,
        QJsonValue(QJsonValue::Null), 7, 4'007));
    const QJsonObject atomicTruncatedItem{
        {QStringLiteral("id"), QStringLiteral("atomic-truncated-snapshot")},
        {QStringLiteral("kind"), QStringLiteral("usage")},
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("state"), QStringLiteral("truncated")},
        {QStringLiteral("content"), QStringLiteral("usage updates truncated")},
    };
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("usage.truncated"), updatingSessionId,
        atomicTruncatedTurnId, atomicTruncatedItem, 8, 4'008, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), updatingSessionId, atomicTruncatedTurnId,
        QJsonValue(QJsonValue::Null), 9, 4'009));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, updatingSessionId) == 9
                    && AgentWorkbenchWidgetTestAccess::timelineTurnState(
                           workbench, updatingSessionId, updatingCompletedTurnId)
                        == QStringLiteral("completed")
                    && AgentWorkbenchWidgetTestAccess::timelineTurnState(
                           workbench, updatingSessionId, updatingInterruptedTurnId)
                        == QStringLiteral("interrupted")
                    && AgentWorkbenchWidgetTestAccess::timelineItemState(
                           workbench, updatingSessionId, updatingCompletedTurnId,
                           QStringLiteral("usage-open-snapshot"))
                        == QStringLiteral("updated")
                    && AgentWorkbenchWidgetTestAccess::timelineItemState(
                           workbench, updatingSessionId, updatingInterruptedTurnId,
                           QStringLiteral("plan-open-snapshot"))
                        == QStringLiteral("updated")
                    && AgentWorkbenchWidgetTestAccess::timelineTurnState(
                           workbench, updatingSessionId, atomicTruncatedTurnId)
                        == QStringLiteral("completed")
                    && AgentWorkbenchWidgetTestAccess::timelineItemState(
                           workbench, updatingSessionId, atomicTruncatedTurnId,
                           QStringLiteral("atomic-truncated-snapshot"))
                        == QStringLiteral("truncated"),
                "updated terminal or atomic truncated Item lifecycle diverged from Runtime")) {
        return false;
    }

    const QString secondSessionId = QStringLiteral("timeline-validation-session-two");
    const QString secondTurnId = QStringLiteral("timeline-validation-turn-two");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, secondSessionId);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), secondSessionId, secondTurnId,
        QJsonValue(QJsonValue::Null), 1, 2'001));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), secondSessionId, secondTurnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("completed"),
                        QStringLiteral("same ID in second Session")), 2, 2'002, 1));
    QJsonObject usageItem{
        {QStringLiteral("id"), QStringLiteral("usage-revisions")},
        {QStringLiteral("kind"), QStringLiteral("usage")},
        {QStringLiteral("role"), QStringLiteral("system")},
        {QStringLiteral("state"), QStringLiteral("updated")},
        {QStringLiteral("content"), QStringLiteral("usage one")},
    };
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("usage.updated"), secondSessionId, secondTurnId,
        usageItem, 3, 2'003, 1));
    usageItem.insert(QStringLiteral("content"), QStringLiteral("usage two"));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("usage.updated"), secondSessionId, secondTurnId,
        usageItem, 4, 2'004, 2));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("usage.updated"), secondSessionId, secondTurnId,
        usageItem, 5, 2'005, 2));
    usageItem.insert(QStringLiteral("state"), QStringLiteral("truncated"));
    usageItem.insert(QStringLiteral("content"), QStringLiteral("usage truncated"));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("usage.truncated"), secondSessionId, secondTurnId,
        usageItem, 5, 2'005, 3));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), secondSessionId, secondTurnId,
        QJsonValue(QJsonValue::Null), 6, 2'006));

    const QString thirdTurnId = QStringLiteral("timeline-validation-turn-three");
    const QString boundaryItemId(128, QLatin1Char('i'));
    const QString oversizedItemId(129, QLatin1Char('i'));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), secondSessionId, thirdTurnId,
        QJsonValue(QJsonValue::Null), 7, 2'007));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), secondSessionId, thirdTurnId,
        timelineMessage(QStringLiteral("live-item"), QStringLiteral("completed"),
                        QStringLiteral("same ID in another Turn")), 8, 2'008, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), secondSessionId, thirdTurnId,
        timelineMessage(boundaryItemId, QStringLiteral("completed"),
                        QStringLiteral("128-byte Item ID")), 9, 2'009, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), secondSessionId, thirdTurnId,
        timelineMessage(oversizedItemId, QStringLiteral("completed"),
                        QStringLiteral("must-not-render")), 10, 2'010, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), secondSessionId, thirdTurnId,
        timelineMessage(QStringLiteral("项目-item"), QStringLiteral("completed"),
                        QStringLiteral("must-not-render")), 10, 2'010, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), secondSessionId, thirdTurnId,
        QJsonValue(QJsonValue::Null), 10, 2'010));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, secondSessionId) == 10
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, sessionId) == 11
                    && AgentWorkbenchWidgetTestAccess::timelineItemText(
                           workbench, QStringLiteral("live-item"))
                        == QStringLiteral("same ID in another Turn")
                    && AgentWorkbenchWidgetTestAccess::timelineItemPresentationCount(
                           workbench, QStringLiteral("live-item")) == 3
                    && AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                           workbench, boundaryItemId)
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                           workbench, oversizedItemId)
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                           workbench, QStringLiteral("项目-item")),
                "Session/Turn Item isolation, revision stream, or ID bounds failed")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionId);

    QJsonObject replayGood = timelineMessage(
        QStringLiteral("replay-good"), QStringLiteral("completed"),
        QStringLiteral("replayed"));
    replayGood.insert(QStringLiteral("sequence"), 3);
    QJsonObject replayBad = timelineMessage(
        QStringLiteral("replay-bad"), QStringLiteral("completed"),
        QStringLiteral("bad"));
    replayBad.insert(QStringLiteral("sequence"), 4);
    replayBad.insert(QStringLiteral("kind"), QStringLiteral("Bad Kind"));
    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("malformed-replay"), sessionId, false);
    runtimeClient->sessionRead(
        QStringLiteral("malformed-replay"),
        replaySnapshot(sessionId, QJsonArray{replayGood, replayBad}, 3, 4, 4));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("live-item"))
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("replay-good")),
                "malformed replay cleared the existing Timeline or partially rendered")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("forged-tail"), sessionId, false);
    runtimeClient->sessionRead(
        QStringLiteral("forged-tail"),
        replaySnapshot(sessionId, QJsonArray{replayGood}, 3, 3, 4));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("live-item"))
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("replay-good")),
                "initial replay accepted a last/latest boundary mismatch")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("wrong-initial-limit"), sessionId, false);
    runtimeClient->sessionRead(
        QStringLiteral("wrong-initial-limit"),
        replaySnapshot(sessionId, QJsonArray{replayGood}, 3, 3, 3, 99));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("live-item")),
                "initial replay accepted a different limit than requested")) {
        return false;
    }

    QJsonObject replayDuplicate = replayGood;
    replayDuplicate.insert(QStringLiteral("sequence"), 4);
    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("duplicate-initial"), sessionId, false);
    runtimeClient->sessionRead(
        QStringLiteral("duplicate-initial"),
        replaySnapshot(sessionId, QJsonArray{replayGood, replayDuplicate}, 3, 4, 4));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("live-item"))
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("replay-good")),
                "duplicate Item IDs partially replaced the initial Timeline")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("valid-replay"), sessionId, false);
    runtimeClient->sessionRead(
        QStringLiteral("valid-replay"),
        replaySnapshot(sessionId, QJsonArray{replayGood}, 3, 3, 3));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("live-item"))
                    && AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("replay-good"))
                    && AgentWorkbenchWidgetTestAccess::historyCursor(workbench)
                        == QStringLiteral("before:3")
                    && AgentWorkbenchWidgetTestAccess::historyFirstSequence(workbench) == 3
                    && AgentWorkbenchWidgetTestAccess::historyLatestSequence(workbench) == 3,
                "valid replay did not atomically replace the Timeline")) {
        return false;
    }

    QJsonObject olderGood = timelineMessage(
        QStringLiteral("older-good"), QStringLiteral("completed"),
        QStringLiteral("older"), QStringLiteral("assistant"));
    olderGood.insert(QStringLiteral("sequence"), 1);
    QJsonObject olderBad = timelineMessage(
        QStringLiteral("older-bad"), QStringLiteral("completed"),
        QStringLiteral("bad"));
    olderBad.insert(QStringLiteral("sequence"), 2);
    olderBad.insert(QStringLiteral("state"), QStringLiteral("unknown"));
    QJsonObject olderSecond = timelineMessage(
        QStringLiteral("older-second"), QStringLiteral("completed"),
        QStringLiteral("older second"));
    olderSecond.insert(QStringLiteral("sequence"), 2);
    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("malformed-older-page"), sessionId, true,
        QStringLiteral("before:3"), 100, 3, 3);
    runtimeClient->sessionRead(
        QStringLiteral("malformed-older-page"),
        replaySnapshot(sessionId, QJsonArray{olderGood, olderBad}, 1, 2, 3));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("older-good"))
                    && AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("replay-good"))
                    && AgentWorkbenchWidgetTestAccess::historyCursor(workbench)
                        == QStringLiteral("before:3"),
                "malformed older replay page partially prepended or changed the cursor")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("wrong-older-boundary"), sessionId, true,
        QStringLiteral("before:3"), 100, 3, 3);
    runtimeClient->sessionRead(
        QStringLiteral("wrong-older-boundary"),
        replaySnapshot(sessionId, QJsonArray{olderGood}, 1, 1, 3));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("older-good"))
                    && AgentWorkbenchWidgetTestAccess::historyCursor(workbench)
                        == QStringLiteral("before:3"),
                "older replay accepted the wrong previous-page boundary")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("wrong-older-cursor"), sessionId, true,
        QStringLiteral("before:3"), 100, 3, 3);
    AgentWorkbenchWidgetTestAccess::setRequestedHistoryCursor(
        workbench, QStringLiteral("before:4"));
    runtimeClient->sessionRead(
        QStringLiteral("wrong-older-cursor"),
        replaySnapshot(sessionId, QJsonArray{olderGood, olderSecond}, 1, 2, 3));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("older-good"))
                    && AgentWorkbenchWidgetTestAccess::historyCursor(workbench)
                        == QStringLiteral("before:3"),
                "older replay accepted a request cursor that was not bound to the page")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("wrong-older-limit"), sessionId, true,
        QStringLiteral("before:3"), 100, 3, 3);
    runtimeClient->sessionRead(
        QStringLiteral("wrong-older-limit"),
        replaySnapshot(sessionId, QJsonArray{olderGood, olderSecond}, 1, 2, 3, 99));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("older-good"))
                    && AgentWorkbenchWidgetTestAccess::historyCursor(workbench)
                        == QStringLiteral("before:3"),
                "older replay accepted a different limit than requested")) {
        return false;
    }

    QJsonObject olderDuplicate = olderGood;
    olderDuplicate.insert(QStringLiteral("sequence"), 2);
    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("duplicate-older"), sessionId, true,
        QStringLiteral("before:3"), 100, 3, 3);
    runtimeClient->sessionRead(
        QStringLiteral("duplicate-older"),
        replaySnapshot(sessionId, QJsonArray{olderGood, olderDuplicate}, 1, 2, 3));
    application.processEvents();
    if (!expect(!AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("older-good"))
                    && AgentWorkbenchWidgetTestAccess::historyCursor(workbench)
                        == QStringLiteral("before:3"),
                "duplicate older Item IDs partially changed the Timeline")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("valid-older"), sessionId, true,
        QStringLiteral("before:3"), 100, 3, 3);
    runtimeClient->sessionRead(
        QStringLiteral("valid-older"),
        replaySnapshot(sessionId, QJsonArray{olderGood, olderSecond}, 1, 2, 3));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("older-good"))
                    && AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("older-second"))
                    && AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("replay-good"))
                    && AgentWorkbenchWidgetTestAccess::historyCursor(workbench).isEmpty()
                    && AgentWorkbenchWidgetTestAccess::historyFirstSequence(workbench) == 1
                    && AgentWorkbenchWidgetTestAccess::historyLatestSequence(workbench) == 3,
                "valid older page did not prepend atomically or close pagination")) {
        return false;
    }

    const QString proposalItemId = QStringLiteral("replay-proposal-item");
    const QJsonObject initialProposal = proposalReadResult(sessionId, QLatin1Char('6'));
    const QJsonObject initialReference = proposalTimelineReference(
        initialProposal, proposalItemId);
    const QString initialTurnId = initialReference.value(
        QStringLiteral("turn_id")).toString();
    QJsonObject initialProposalItem = proposalTimelineItem(
        proposalItemId, initialReference);
    initialProposalItem.insert(QStringLiteral("sequence"), 3);
    initialProposalItem.insert(QStringLiteral("turn_id"), initialTurnId);

    QJsonObject forgedInitialItem = initialProposalItem;
    forgedInitialItem.insert(QStringLiteral("turn_id"), QStringLiteral("forged-replay-turn"));
    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("forged-initial-proposal-turn"), sessionId, false);
    runtimeClient->sessionRead(
        QStringLiteral("forged-initial-proposal-turn"),
        replaySnapshot(sessionId, QJsonArray{forgedInitialItem}, 3, 3, 3));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("replay-good"))
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, proposalItemId),
                "initial Proposal replay accepted a forged outer Turn binding")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("valid-initial-proposal"), sessionId, false);
    runtimeClient->sessionRead(
        QStringLiteral("valid-initial-proposal"),
        replaySnapshot(sessionId, QJsonArray{initialProposalItem}, 3, 3, 3));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, proposalItemId)
                    && AgentWorkbenchWidgetTestAccess::timelineItemPresentationCount(
                        workbench, proposalItemId) == 1
                    && AgentWorkbenchWidgetTestAccess::historyCursor(workbench)
                        == QStringLiteral("before:3"),
                "initial Proposal replay did not retain its explicit Turn binding")) {
        return false;
    }

    const QJsonObject olderProposal = proposalReadResult(sessionId, QLatin1Char('7'));
    const QJsonObject olderReference = proposalTimelineReference(
        olderProposal, proposalItemId);
    const QString olderTurnId = olderReference.value(QStringLiteral("turn_id")).toString();
    QJsonObject olderProposalItem = proposalTimelineItem(proposalItemId, olderReference);
    olderProposalItem.insert(QStringLiteral("sequence"), 1);
    olderProposalItem.insert(QStringLiteral("turn_id"), olderTurnId);
    QJsonObject compatibleLegacyItem = timelineMessage(
        QStringLiteral("older-legacy-item"), QStringLiteral("completed"),
        QStringLiteral("older item without an outer Turn"));
    compatibleLegacyItem.insert(QStringLiteral("sequence"), 2);

    QJsonObject forgedOlderItem = olderProposalItem;
    forgedOlderItem.insert(QStringLiteral("turn_id"), QStringLiteral("forged-older-turn"));
    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("forged-older-proposal-turn"), sessionId, true,
        QStringLiteral("before:3"), 100, 3, 3);
    runtimeClient->sessionRead(
        QStringLiteral("forged-older-proposal-turn"),
        replaySnapshot(sessionId, QJsonArray{forgedOlderItem, compatibleLegacyItem},
                       1, 2, 3));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineItemPresentationCount(
                    workbench, proposalItemId) == 1
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("older-legacy-item"))
                    && AgentWorkbenchWidgetTestAccess::historyCursor(workbench)
                        == QStringLiteral("before:3"),
                "older Proposal replay accepted forged Turn data or partially prepended")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, QStringLiteral("valid-older-proposal"), sessionId, true,
        QStringLiteral("before:3"), 100, 3, 3);
    runtimeClient->sessionRead(
        QStringLiteral("valid-older-proposal"),
        replaySnapshot(sessionId, QJsonArray{olderProposalItem, compatibleLegacyItem},
                       1, 2, 3));
    application.processEvents();
    return expect(AgentWorkbenchWidgetTestAccess::timelineItemPresentationCount(
                      workbench, proposalItemId) == 2
                      && AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                          workbench, QStringLiteral("older-legacy-item"))
                      && AgentWorkbenchWidgetTestAccess::historyCursor(workbench).isEmpty()
                      && AgentWorkbenchWidgetTestAccess::historyFirstSequence(workbench) == 1
                      && AgentWorkbenchWidgetTestAccess::historyLatestSequence(workbench) == 3,
                  "older Proposal replay did not preserve scoped Item identity or legacy Items");
}

bool verifySessionScopedTimelineSequences(QApplication &application,
                                          AgentWorkbenchWidget &workbench,
                                          AgentRuntimeClient *runtimeClient)
{
    if (!runtimeClient) return false;
    const QString firstSession = QStringLiteral("timeline-session-first");
    const QString secondSession = QStringLiteral("timeline-session-second");
    const QString firstTurn = QStringLiteral("turn-first");
    const QString secondTurn = QStringLiteral("turn-second");
    const QString sharedItem = QStringLiteral("shared-item");
    const QString genericKind = QStringLiteral("a") + QString(63, QLatin1Char('g'));
    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);

    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, firstSession);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), firstSession, firstTurn,
        QJsonValue(QJsonValue::Null), 1, 1'001));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), secondSession, secondTurn,
        QJsonValue(QJsonValue::Null), 1, 2'001));

    QJsonObject backgroundItem = timelineMessage(
        sharedItem, QStringLiteral("started"), QStringLiteral("background-start"),
        QStringLiteral("tool"));
    backgroundItem.insert(QStringLiteral("kind"), genericKind);
    backgroundItem.insert(QStringLiteral("data"), QJsonObject{
        {QStringLiteral("payload"), QString(3 * 1024 * 1024, QLatin1Char('d'))},
        {QStringLiteral("integers"), QJsonArray{
            1.0, 1e3, -0.0,
            9'007'199'254'740'991.0,
            -9'007'199'254'740'991.0,
        }},
    });
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.started"), secondSession, secondTurn,
        backgroundItem, 2, 2'002, 1));

    QJsonObject foregroundItem = timelineMessage(
        sharedItem, QStringLiteral("completed"), QStringLiteral("foreground-only"));
    foregroundItem.insert(QStringLiteral("kind"), QStringLiteral("future.message"));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), firstSession, firstTurn,
        foregroundItem, 2, 1'002, 1));

    backgroundItem.insert(QStringLiteral("state"), QStringLiteral("delta"));
    backgroundItem.insert(QStringLiteral("content"), QStringLiteral("background-delta"));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.delta"), secondSession, secondTurn,
        backgroundItem, 3, 2'003, 2));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), firstSession, firstTurn,
        QJsonValue(QJsonValue::Null), 3, 1'003));

    backgroundItem.insert(QStringLiteral("state"), QStringLiteral("completed"));
    backgroundItem.insert(QStringLiteral("content"), QStringLiteral("background-complete"));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), secondSession, secondTurn,
        backgroundItem, 4, 2'004, 3));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), secondSession, secondTurn,
        QJsonValue(QJsonValue::Null), 5, 2'005));
    application.processEvents();
    const bool scoped = AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                            workbench, firstSession) == 3
        && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
               workbench, secondSession) == 5
        && AgentWorkbenchWidgetTestAccess::timelineItemState(
               workbench, secondSession, secondTurn, sharedItem)
               == QStringLiteral("completed")
        && AgentWorkbenchWidgetTestAccess::timelineItemText(workbench, sharedItem)
               == QStringLiteral("foreground-only")
        && AgentWorkbenchWidgetTestAccess::timelineItemPresentationCount(
               workbench, sharedItem) == 1
        && !AgentWorkbenchWidgetTestAccess::turnRunning(workbench);

    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, secondSession);
    const QString resumedTurn = QStringLiteral("turn-second-resumed");
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), secondSession, resumedTurn,
        QJsonValue(QJsonValue::Null), 6, 2'006));
    QJsonObject overlongKindItem = timelineMessage(
        QStringLiteral("overlong-kind"), QStringLiteral("completed"),
        QStringLiteral("must-not-render"));
    overlongKindItem.insert(QStringLiteral("kind"),
                            QStringLiteral("a") + QString(64, QLatin1Char('x')));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), secondSession, resumedTurn,
        overlongKindItem, 7, 2'007, 1));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), secondSession, resumedTurn,
        QJsonValue(QJsonValue::Null), 7, 2'007));
    application.processEvents();
    return expect(scoped
                      && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                             workbench, firstSession) == 3
                      && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                             workbench, secondSession) == 7
                      && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                          workbench, QStringLiteral("overlong-kind"))
                      && !AgentWorkbenchWidgetTestAccess::turnRunning(workbench),
                  "interleaved Session Timeline state, generic kind, or UI isolation failed");
}

bool verifyTimelineGapRecovery(QApplication &application,
                               AgentWorkbenchWidget &workbench,
                               AgentRuntimeClient *runtimeClient)
{
    if (!runtimeClient) return false;
    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString sessionA = QStringLiteral("timeline-gap-session-a");
    const QString sessionB = QStringLiteral("timeline-gap-session-b");
    const QString turnA = QStringLiteral("timeline-gap-turn-a");
    const QString turnB = QStringLiteral("timeline-gap-turn-b");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionA);

    const QJsonObject eventA1 = timelineEnvelope(
        QStringLiteral("turn.started"), sessionA, turnA,
        QJsonValue(QJsonValue::Null), 1, 10'001);
    const QJsonObject eventA2 = timelineEnvelope(
        QStringLiteral("item.completed"), sessionA, turnA,
        timelineMessage(QStringLiteral("timeline-gap-item"),
                        QStringLiteral("completed"), QStringLiteral("recovered")),
        2, 10'002, 1);
    const QJsonObject eventA3 = timelineEnvelope(
        QStringLiteral("turn.completed"), sessionA, turnA,
        QJsonValue(QJsonValue::Null), 3, 10'003);
    AgentWorkbenchWidgetTestAccess::presentHistoryTimelineItem(
        workbench, eventA2.value(QStringLiteral("item")).toObject());
    runtimeClient->timelineEvent(eventA1);
    AgentWorkbenchWidgetTestAccess::setTimelineSyncAvailable(workbench, true);
    runtimeClient->timelineEvent(eventA3);
    const QString firstPageRequest =
        AgentWorkbenchWidgetTestAccess::timelineSyncRequestId(workbench, sessionA);

    const QJsonObject eventB1 = timelineEnvelope(
        QStringLiteral("turn.started"), sessionB, turnB,
        QJsonValue(QJsonValue::Null), 1, 20'001);
    const QJsonObject eventB2 = timelineEnvelope(
        QStringLiteral("item.completed"), sessionB, turnB,
        timelineMessage(QStringLiteral("timeline-background-item"),
                        QStringLiteral("completed"), QStringLiteral("background")),
        2, 20'002, 1);
    const QJsonObject eventB3 = timelineEnvelope(
        QStringLiteral("turn.completed"), sessionB, turnB,
        QJsonValue(QJsonValue::Null), 3, 20'003);
    runtimeClient->timelineEvent(eventB1);
    runtimeClient->timelineEvent(eventB3);
    const QString backgroundPageRequest =
        AgentWorkbenchWidgetTestAccess::timelineSyncRequestId(workbench, sessionB);
    runtimeClient->timelineSynced(
        backgroundPageRequest,
        timelineSyncPage(sessionB, timelineAnchorForEvent(eventB1),
                         timelineAnchorForEvent(eventB3),
                         QJsonArray{eventB2, eventB3}, true));
    if (!expect(!firstPageRequest.isEmpty()
                    && !backgroundPageRequest.isEmpty()
                    && AgentWorkbenchWidgetTestAccess::queuedTimelineEvents(
                           workbench, sessionA) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionA) == QStringLiteral("syncing")
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, sessionA) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, sessionB) == 3
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionB) == QStringLiteral("live")
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                           workbench, QStringLiteral("timeline-background-item")),
                "a Session gap blocked an independent Session or failed to queue live data")) {
        return false;
    }

    runtimeClient->timelineSynced(
        firstPageRequest,
        timelineSyncPage(sessionA, timelineAnchorForEvent(eventA1),
                         timelineAnchorForEvent(eventA3), QJsonArray{eventA2}, false));
    const QString secondPageRequest =
        AgentWorkbenchWidgetTestAccess::timelineSyncRequestId(workbench, sessionA);
    if (!expect(!secondPageRequest.isEmpty()
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, sessionA) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionA) == QStringLiteral("syncing")
                    && AgentWorkbenchWidgetTestAccess::queuedTimelineEvents(
                           workbench, sessionA) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineItemPresentationCount(
                           workbench, QStringLiteral("timeline-gap-item")) == 1,
                "an incomplete replay page advanced its confirmed projection or rendered early")) {
        return false;
    }
    runtimeClient->timelineSynced(
        secondPageRequest,
        timelineSyncPage(sessionA, timelineAnchorForEvent(eventA2),
                         timelineAnchorForEvent(eventA3), QJsonArray{eventA3}, true));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionA) == 3
                    && AgentWorkbenchWidgetTestAccess::timelineEventId(
                           workbench, sessionA)
                        == eventA3.value(QStringLiteral("event_id")).toString()
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionA) == QStringLiteral("live")
                    && AgentWorkbenchWidgetTestAccess::queuedTimelineEvents(
                           workbench, sessionA) == 0
                    && AgentWorkbenchWidgetTestAccess::timelineItemPresentationCount(
                           workbench, QStringLiteral("timeline-gap-item")) == 1,
                "fixed-watermark replay did not atomically commit and drain live events")) {
        return false;
    }

    runtimeClient->timelineEvent(eventA3);
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionA) == 3
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionA) == QStringLiteral("live"),
                "an exact duplicate Timeline event was not idempotent")) {
        return false;
    }
    QJsonObject driftedA3 = eventA3;
    driftedA3.insert(QStringLiteral("timestamp_ms"), 10'004);
    driftedA3.insert(QStringLiteral("event_id"), QString());
    driftedA3.insert(QStringLiteral("event_id"),
                     AgentRuntimeClient::timelineEventIdentity(driftedA3));
    runtimeClient->timelineEvent(driftedA3);
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, sessionA) == QStringLiteral("frozen")
                    && !AgentWorkbenchWidgetTestAccess::timelineAllowsNewTurn(workbench),
                "same-sequence anchor drift did not freeze only the affected Session")) {
        return false;
    }

    const QString sessionC = QStringLiteral("timeline-gap-session-c");
    const QString turnC = QStringLiteral("timeline-gap-turn-c");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionC);
    const QJsonObject eventC1 = timelineEnvelope(
        QStringLiteral("turn.started"), sessionC, turnC,
        QJsonValue(QJsonValue::Null), 1, 30'001);
    const QJsonObject eventC2 = timelineEnvelope(
        QStringLiteral("item.completed"), sessionC, turnC,
        timelineMessage(QStringLiteral("timeline-page-must-not-render"),
                        QStringLiteral("completed"), QStringLiteral("candidate")),
        2, 30'002, 1);
    const QJsonObject eventC3 = timelineEnvelope(
        QStringLiteral("turn.completed"), sessionC, turnC,
        QJsonValue(QJsonValue::Null), 3, 30'003);
    runtimeClient->timelineEvent(eventC1);
    AgentWorkbenchWidgetTestAccess::prepareTimelineSync(
        workbench, sessionC, QStringLiteral("timeline-sync-c"));
    QJsonObject malformedC3 = eventC3;
    malformedC3.insert(QStringLiteral("event_id"),
                       QStringLiteral("event:sha256:") + QString(64, QLatin1Char('f')));
    runtimeClient->timelineSynced(
        QStringLiteral("timeline-sync-c"),
        timelineSyncPage(sessionC, timelineAnchorForEvent(eventC1),
                         timelineAnchorForEvent(eventC3), QJsonArray{eventC2}, false));
    const QString malformedPageRequest =
        AgentWorkbenchWidgetTestAccess::timelineSyncRequestId(workbench, sessionC);
    if (!expect(!malformedPageRequest.isEmpty()
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, sessionC) == 1
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                           workbench, QStringLiteral("timeline-page-must-not-render")),
                "the first replay page escaped its atomic staging boundary")) {
        return false;
    }
    runtimeClient->timelineSynced(
        malformedPageRequest,
        timelineSyncPage(sessionC, timelineAnchorForEvent(eventC2),
                         timelineAnchorForEvent(eventC3), QJsonArray{malformedC3}, true));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionC) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionC) == QStringLiteral("frozen")
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                           workbench, QStringLiteral("timeline-page-must-not-render")),
                "a malformed replay page partially changed projection or presentation")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString unavailableSession = QStringLiteral("timeline-snapshot-unavailable-session");
    const QString unavailableTurn = QStringLiteral("timeline-snapshot-unavailable-turn");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, unavailableSession);
    AgentWorkbenchWidgetTestAccess::setTimelineSyncAvailable(workbench, true);
    AgentWorkbenchWidgetTestAccess::setTimelineSnapshotAvailable(workbench, false);
    const QJsonObject unavailableConfirmed = timelineEnvelope(
        QStringLiteral("turn.started"), unavailableSession, unavailableTurn,
        QJsonValue(QJsonValue::Null), 1, 51'051);
    const QJsonObject unavailableQueued = timelineEnvelope(
        QStringLiteral("turn.completed"), unavailableSession, unavailableTurn,
        QJsonValue(QJsonValue::Null), 2, 51'052);
    runtimeClient->timelineEvent(unavailableConfirmed);
    AgentWorkbenchWidgetTestAccess::prepareTimelineSync(
        workbench, unavailableSession, QStringLiteral("timeline-snapshot-unavailable-sync"));
    runtimeClient->timelineEvent(unavailableQueued);
    int unavailableSnapshotRequests = 0;
    AgentWorkbenchWidgetTestAccess::setTimelineSnapshotRequester(
        workbench,
        [&unavailableSnapshotRequests](const QString &, const QString &,
                                       const QJsonObject &, const QJsonObject &, int) {
            ++unavailableSnapshotRequests;
            return QStringLiteral("unexpected-snapshot-request");
        });
    const QJsonObject unavailableFloor = timelineAnchorForEvent(unavailableQueued);
    runtimeClient->timelineRetentionGap(
        QStringLiteral("timeline-snapshot-unavailable-sync"),
        QJsonObject{
            {QStringLiteral("schema_version"),
             QStringLiteral("timeline-retention-gap/0.1")},
            {QStringLiteral("reason"),
             QStringLiteral("requested-anchor-not-retained")},
            {QStringLiteral("session_id"), unavailableSession},
            {QStringLiteral("requested_after"), timelineAnchorForEvent(unavailableConfirmed)},
            {QStringLiteral("requested_watermark"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("retained_floor"), unavailableFloor},
            {QStringLiteral("head"), unavailableFloor},
            {QStringLiteral("snapshot_required"), true},
            {QStringLiteral("snapshot_available"), false},
            {QStringLiteral("snapshot_capability"),
             QStringLiteral("timeline.snapshot.current")},
            {QStringLiteral("snapshot_method"), QStringLiteral("timeline/snapshot")},
            {QStringLiteral("event_history_complete"), false},
            {QStringLiteral("replay_from_floor_allowed"), false},
        });
    const bool unavailablePreserved = unavailableSnapshotRequests == 0
        && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
               workbench, unavailableSession) == 1
        && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
               workbench, unavailableSession) == QStringLiteral("frozen")
        && AgentWorkbenchWidgetTestAccess::timelineSnapshotRecoveryRequired(
               workbench, unavailableSession)
        && AgentWorkbenchWidgetTestAccess::timelinePendingEventCount(workbench) == 1;
    AgentWorkbenchWidgetTestAccess::suspendTimelinesForDisconnect(workbench);
    if (!expect(unavailablePreserved
                    && AgentWorkbenchWidgetTestAccess::timelinePendingEventCount(workbench) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRetriesOnReconnect(
                        workbench, unavailableSession),
                "snapshot-unavailable recovery discarded confirmed or queued live state")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString sessionF = QStringLiteral("timeline-gap-session-f");
    const QString turnF = QStringLiteral("timeline-gap-turn-f");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionF);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), sessionF, turnF,
        QJsonValue(QJsonValue::Null), 1, 32'001));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSync(
        workbench, sessionF, QStringLiteral("timeline-sync-f"));
    for (quint64 index = 0; index < 257; ++index) {
        runtimeClient->timelineEvent(timelineEnvelope(
            QStringLiteral("future.notice"), sessionF, turnF,
            QJsonValue(QJsonValue::Null), index + 2, 32'002 + index));
    }
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, sessionF) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::queuedTimelineEvents(
                           workbench, sessionF) == 0
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, sessionF) == 1,
                "live queue overflow retained untrusted events or advanced the cursor")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString sessionG = QStringLiteral("timeline-gap-session-g");
    const QString turnG = QStringLiteral("timeline-gap-turn-g");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionG);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), sessionG, turnG,
        QJsonValue(QJsonValue::Null), 1, 34'001));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSync(
        workbench, sessionG, QStringLiteral("timeline-sync-g"));
    AgentWorkbenchWidgetTestAccess::setPendingPrompt(
        workbench, QStringLiteral("preserve-prompt-during-timeline-sync"));
    runtimeClient->requestFailedExact(QStringLiteral("timeline-sync-g"),
                                      QStringLiteral("timeline/sync"),
                                      QStringLiteral("redacted"),
                                      QStringLiteral("-32160"));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, sessionG) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, sessionG) == 1
                    && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                        == QStringLiteral("preserve-prompt-during-timeline-sync"),
                "Timeline sync error did not freeze the cursor or preserve pending input")) {
        return false;
    }
    AgentWorkbenchWidgetTestAccess::setPendingPrompt(workbench, QString());

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString sessionH = QStringLiteral("timeline-gap-session-h");
    const QString turnH = QStringLiteral("timeline-gap-turn-h");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionH);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), sessionH, turnH,
        QJsonValue(QJsonValue::Null), 1, 33'001));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSync(
        workbench, sessionH, QStringLiteral("timeline-sync-h"));
    AgentWorkbenchWidgetTestAccess::exhaustTimelinePendingBytes(workbench);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("future.notice"), sessionH, turnH,
        QJsonValue(QJsonValue::Null), 2, 33'002));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, sessionH) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::queuedTimelineEvents(
                           workbench, sessionH) == 0
                    && AgentWorkbenchWidgetTestAccess::timelinePendingEventCount(workbench) == 0
                    && !AgentWorkbenchWidgetTestAccess::timelineAllowsNewTurn(workbench),
                "aggregate Timeline pending-byte exhaustion did not fail closed")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString capacitySession = QStringLiteral("timeline-capacity-overflow");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, capacitySession);
    AgentWorkbenchWidgetTestAccess::fillTimelineSessionCapacity(workbench);
    AgentWorkbenchWidgetTestAccess::beginTimelineSync(workbench, capacitySession);
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, capacitySession) == QStringLiteral("missing")
                    && AgentWorkbenchWidgetTestAccess::timelineTrackingExhausted(workbench)
                    && !AgentWorkbenchWidgetTestAccess::timelineAllowsNewTurn(workbench),
                "the 10001st Timeline Session bypassed the global capacity gate")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString sessionE = QStringLiteral("timeline-gap-session-e");
    const QString turnE = QStringLiteral("timeline-gap-turn-e");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionE);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), sessionE, turnE,
        QJsonValue(QJsonValue::Null), 1, 35'001));
    AgentWorkbenchWidgetTestAccess::setTimelineSyncAvailable(workbench, false);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), sessionE, turnE,
        QJsonValue(QJsonValue::Null), 3, 35'003));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, sessionE) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, sessionE) == 1
                    && !AgentWorkbenchWidgetTestAccess::timelineAllowsNewTurn(workbench),
                "missing replay capability did not preserve and freeze the confirmed cursor")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString sessionD = QStringLiteral("timeline-gap-session-d");
    const QString turnD = QStringLiteral("timeline-gap-turn-d");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionD);
    const QJsonObject eventD1 = timelineEnvelope(
        QStringLiteral("turn.started"), sessionD, turnD,
        QJsonValue(QJsonValue::Null), 1, 40'001);
    const QJsonObject eventD3 = timelineEnvelope(
        QStringLiteral("turn.completed"), sessionD, turnD,
        QJsonValue(QJsonValue::Null), 3, 40'003);
    runtimeClient->timelineEvent(eventD1);
    AgentWorkbenchWidgetTestAccess::prepareTimelineSync(
        workbench, sessionD, QStringLiteral("timeline-sync-d"));
    runtimeClient->timelineEvent(eventD3);
    const QString confirmedEventId = AgentWorkbenchWidgetTestAccess::timelineEventId(
        workbench, sessionD);
    runtimeClient->connectionStateChanged(false, QStringLiteral("runtime disconnected"));
    runtimeClient->requestFailedExact(QStringLiteral("timeline-sync-d"),
                                      QStringLiteral("timeline/sync"),
                                      QStringLiteral("runtime disconnected"),
                                      QStringLiteral("-1"));
    application.processEvents();
    QPushButton *sendButton = workbench.findChild<QPushButton *>(
        QStringLiteral("agentSendButton"));
    const bool disconnectedStatePreserved =
        AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(workbench, sessionD) == 1
        && AgentWorkbenchWidgetTestAccess::timelineEventId(workbench, sessionD)
            == confirmedEventId
        && AgentWorkbenchWidgetTestAccess::timelineTurnState(workbench, sessionD, turnD)
            == QStringLiteral("running")
        && AgentWorkbenchWidgetTestAccess::queuedTimelineEvents(workbench, sessionD) == 0
        && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(workbench, sessionD)
            == QStringLiteral("frozen")
        && AgentWorkbenchWidgetTestAccess::timelineRetriesOnReconnect(
               workbench, sessionD)
        && !AgentWorkbenchWidgetTestAccess::timelineAllowsNewTurn(workbench)
        && sendButton && sendButton->text() == QStringLiteral("发送")
        && !sendButton->isEnabled();
    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    return expect(disconnectedStatePreserved,
                  "disconnect lost its confirmed projection, pending bounds, or reconnect retry");
}

bool verifyTimelineSubscriptionRecovery(QApplication &application,
                                        AgentWorkbenchWidget &workbench,
                                        AgentRuntimeClient *runtimeClient)
{
    if (!runtimeClient) return false;
    const auto suppressRealConnectionAbandon = [&workbench]() {
        AgentWorkbenchWidgetTestAccess::setTimelineSubscriptionConnectionAbandoner(
            workbench, [](const QString &) {});
    };
    const quint64 generation = runtimeClient->processGeneration();
    if (!expect(generation > 0, "subscription test has no Runtime generation")) {
        return false;
    }
    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    suppressRealConnectionAbandon();
    const QString sessionId = QStringLiteral("timeline-subscription-session");
    const QString turnId = QStringLiteral("timeline-subscription-turn");
    const QString subscriptionId = QStringLiteral("qt-subscription-test");
    const QString syncRequestId = QStringLiteral("timeline-subscription-sync-test");
    const QString activateRequestId = QStringLiteral("timeline-subscription-activate-test");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionId);
    const QJsonObject started = timelineEnvelope(
        QStringLiteral("turn.started"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 1, 31'001);
    const QJsonObject recovered = timelineEnvelope(
        QStringLiteral("item.completed"), sessionId, turnId,
        timelineMessage(QStringLiteral("subscription-recovered-item"),
                        QStringLiteral("completed"), QStringLiteral("recovered")),
        2, 31'002, 1);
    const QJsonObject completed = timelineEnvelope(
        QStringLiteral("turn.completed"), sessionId, turnId,
        QJsonValue(QJsonValue::Null), 3, 31'003);
    runtimeClient->timelineEvent(started);
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSync(
        workbench, sessionId, generation, subscriptionId, syncRequestId,
        timelineAnchorForEvent(recovered));
    AgentWorkbenchWidgetTestAccess::setTimelineSubscriptionActivationRequester(
        workbench, activateRequestId);
    runtimeClient->timelineSubscriptionSynced(
        syncRequestId,
        timelineSyncPage(sessionId, timelineAnchorForEvent(started),
                         timelineAnchorForEvent(recovered), QJsonArray{recovered}, true));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionId) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionId)
                        == QStringLiteral("awaiting-activation")
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("subscription-recovered-item")),
                "subscription recovery published before activation")) {
        return false;
    }
    runtimeClient->timelineSubscriptionEvent(timelineSubscriptionEventWrapper(
        generation, sessionId, subscriptionId, timelineAnchorForEvent(recovered),
        timelineAnchorForEvent(recovered), completed));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionId) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionId) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                           workbench, sessionId)
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("subscription-recovered-item"))
                    && AgentWorkbenchWidgetTestAccess::timelinePendingEventCount(workbench) == 0,
                "pre-activation subscription event did not fail closed")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    suppressRealConnectionAbandon();
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionId);
    runtimeClient->timelineEvent(started);
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSync(
        workbench, sessionId, generation, subscriptionId, syncRequestId,
        timelineAnchorForEvent(recovered));
    AgentWorkbenchWidgetTestAccess::setTimelineSubscriptionActivationRequester(
        workbench, activateRequestId);
    runtimeClient->timelineSubscriptionSynced(
        syncRequestId,
        timelineSyncPage(sessionId, timelineAnchorForEvent(started),
                         timelineAnchorForEvent(recovered), QJsonArray{recovered}, true));
    runtimeClient->timelineSubscriptionActivated(
        activateRequestId,
        timelineSubscriptionActiveResult(
            generation + 1, sessionId, subscriptionId,
            timelineAnchorForEvent(recovered)));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionId) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionId)
                        == QStringLiteral("awaiting-activation"),
                "stale generation activated a Timeline subscription")) {
        return false;
    }

    runtimeClient->timelineSubscriptionActivated(
        activateRequestId,
        timelineSubscriptionActiveResult(
            generation, sessionId, subscriptionId,
            timelineAnchorForEvent(recovered)));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionId) == 2
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionId) == QStringLiteral("live")
                    && AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("subscription-recovered-item"))
                    && AgentWorkbenchWidgetTestAccess::timelinePendingEventCount(workbench) == 0
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionCursor(
                           workbench, sessionId) == timelineAnchorForEvent(recovered),
                "activation did not atomically publish subscription recovery")) {
        return false;
    }
    runtimeClient->timelineSubscriptionEvent(timelineSubscriptionEventWrapper(
        generation, sessionId, subscriptionId, timelineAnchorForEvent(recovered),
        timelineAnchorForEvent(recovered), completed));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionId) == 3
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionId) == QStringLiteral("live")
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionCursor(
                           workbench, sessionId) == timelineAnchorForEvent(completed),
                "activation-before-drain ordering did not publish the live event")) {
        return false;
    }

    const quint64 activeGeneration =
        AgentWorkbenchWidgetTestAccess::timelineSubscriptionGeneration(
            workbench, sessionId);
    const QString activeSubscriptionId =
        AgentWorkbenchWidgetTestAccess::timelineSubscriptionId(workbench, sessionId);
    const QJsonObject activeSubscriptionCursor =
        AgentWorkbenchWidgetTestAccess::timelineSubscriptionCursor(
            workbench, sessionId);
    QJsonObject replayedSubscriptionItem = timelineMessage(
        QStringLiteral("subscription-recovered-item"), QStringLiteral("completed"),
        QStringLiteral("recovered"));
    replayedSubscriptionItem.insert(QStringLiteral("sequence"), 1);
    replayedSubscriptionItem.insert(QStringLiteral("turn_id"), turnId);
    const QString activeSessionReadId =
        QStringLiteral("active-subscription-session-read");
    AgentWorkbenchWidgetTestAccess::prepareSessionRead(
        workbench, activeSessionReadId, sessionId, false);
    runtimeClient->sessionRead(
        activeSessionReadId,
        replaySnapshot(sessionId, QJsonArray{replayedSubscriptionItem}, 1, 1, 1));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, sessionId) == QStringLiteral("live")
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionGeneration(
                           workbench, sessionId) == activeGeneration
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionId(
                           workbench, sessionId) == activeSubscriptionId
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionCursor(
                           workbench, sessionId) == activeSubscriptionCursor
                    && AgentWorkbenchWidgetTestAccess::timelineSyncRequestId(
                           workbench, sessionId).isEmpty()
                    && AgentWorkbenchWidgetTestAccess::queuedTimelineEvents(
                           workbench, sessionId) == 0,
                "ordinary session/read replaced an active Timeline subscription attempt")) {
        return false;
    }

    const QJsonObject staleEvent = timelineEnvelope(
        QStringLiteral("turn.started"), sessionId,
        QStringLiteral("timeline-subscription-stale-turn"),
        QJsonValue(QJsonValue::Null), 4, 31'004);
    runtimeClient->timelineSubscriptionEvent(timelineSubscriptionEventWrapper(
        generation + 1, sessionId, subscriptionId, timelineAnchorForEvent(completed),
        timelineAnchorForEvent(recovered), staleEvent));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionId) == 3
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionId) == QStringLiteral("live"),
                "stale generation subscription event changed confirmed state")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::suspendTimelinesForDisconnect(workbench);
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionId) == 3
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionId) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelineRetriesOnReconnect(
                           workbench, sessionId)
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                           workbench, sessionId)
                    && AgentWorkbenchWidgetTestAccess::timelinePendingEventCount(workbench) == 0,
                "disconnect discarded confirmed projection or retained subscription authority")) {
        return false;
    }

    const QString ambiguousSession =
        QStringLiteral("timeline-subscription-ambiguous-session");
    const QString ambiguousRequest =
        QStringLiteral("timeline-subscription-ambiguous-sync");
    AgentWorkbenchWidgetTestAccess::setPendingPrompt(
        workbench, QStringLiteral("prompt-waiting-for-subscription-recovery"));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSync(
        workbench, ambiguousSession, generation,
        QStringLiteral("ambiguous-subscription"), ambiguousRequest,
        timelineAnchorForEvent(completed));
    runtimeClient->requestFailedExact(
        ambiguousRequest, QStringLiteral("timeline/subscription-sync"),
        QStringLiteral("connection ownership unknown"), QStringLiteral("-1"));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, ambiguousSession) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelineRetriesOnReconnect(
                           workbench, ambiguousSession)
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                           workbench, ambiguousSession)
                    && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                        == QStringLiteral("prompt-waiting-for-subscription-recovery"),
                "ambiguous subscription ownership discarded the queued prompt or retained authority")) {
        return false;
    }
    AgentWorkbenchWidgetTestAccess::setPendingPrompt(workbench, QString());

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString failedSession = QStringLiteral("timeline-subscription-failed-session");
    const QString unaffectedSession = QStringLiteral("timeline-subscription-unaffected-session");
    const QJsonObject failedStarted = timelineEnvelope(
        QStringLiteral("turn.started"), failedSession,
        QStringLiteral("timeline-subscription-failed-turn"),
        QJsonValue(QJsonValue::Null), 1, 31'101);
    const QJsonObject unaffectedStarted = timelineEnvelope(
        QStringLiteral("turn.started"), unaffectedSession,
        QStringLiteral("timeline-subscription-unaffected-turn"),
        QJsonValue(QJsonValue::Null), 1, 31'201);
    runtimeClient->timelineEvent(failedStarted);
    runtimeClient->timelineEvent(unaffectedStarted);
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSync(
        workbench, failedSession, generation, QStringLiteral("failed-subscription"),
        QStringLiteral("failed-subscription-request"),
        timelineAnchorForEvent(failedStarted));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSync(
        workbench, unaffectedSession, generation, QStringLiteral("unaffected-subscription"),
        QStringLiteral("unaffected-subscription-request"),
        timelineAnchorForEvent(unaffectedStarted));
    const QJsonObject boundFailure{
            {QStringLiteral("schema_version"),
             QStringLiteral("timeline-subscription-failure/0.1")},
            {QStringLiteral("connection_generation"), static_cast<double>(generation)},
            {QStringLiteral("session_id"), failedSession},
            {QStringLiteral("subscription_id"), QStringLiteral("failed-subscription")},
            {QStringLiteral("state"), QStringLiteral("failed")},
            {QStringLiteral("stage"), QStringLiteral("sync")},
            {QStringLiteral("cursor"), timelineAnchorForEvent(failedStarted)},
            {QStringLiteral("watermark"), timelineAnchorForEvent(failedStarted)},
            {QStringLiteral("request_identity"), QStringLiteral(
                "timeline-subscription-request:sha256:") + QString(64, QLatin1Char('a'))},
            {QStringLiteral("reason"), QStringLiteral("test.failure")},
            {QStringLiteral("retryable"), true},
            {QStringLiteral("cleanup_required"), true},
        };
    runtimeClient->timelineSubscriptionFailed(
        QStringLiteral("wrong-subscription-request"), boundFailure);
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, failedSession) == QStringLiteral("syncing")
                    && !AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                        workbench, failedSession),
                "mismatched request ID terminated a Timeline subscription")) {
        return false;
    }
    runtimeClient->timelineSubscriptionFailed(
        QStringLiteral("failed-subscription-request"), boundFailure);
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, failedSession) == QStringLiteral("failed")
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                           workbench, failedSession)
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, failedSession) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, unaffectedSession) == QStringLiteral("syncing")
                    && !AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                        workbench, unaffectedSession)
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, unaffectedSession) == 1,
                "typed subscription failure was not isolated to its bound Session")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString snapshotSession = QStringLiteral("timeline-subscription-snapshot-session");
    const QString snapshotTurn = QStringLiteral("timeline-subscription-snapshot-turn");
    const QString snapshotSubscription = QStringLiteral("qt-subscription-snapshot-test");
    const QString snapshotRequest = QStringLiteral("timeline-subscription-snapshot-test");
    const QString snapshotActivate = QStringLiteral("timeline-subscription-snapshot-activate");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, snapshotSession);
    const QJsonObject snapshotStarted = timelineEnvelope(
        QStringLiteral("turn.started"), snapshotSession, snapshotTurn,
        QJsonValue(QJsonValue::Null), 1, 32'001);
    const QJsonObject snapshotItemEvent = timelineEnvelope(
        QStringLiteral("item.completed"), snapshotSession, snapshotTurn,
        timelineMessage(QStringLiteral("subscription-snapshot-item"),
                        QStringLiteral("completed"), QStringLiteral("snapshot")),
        2, 32'002, 1);
    const QJsonObject snapshotCompleted = timelineEnvelope(
        QStringLiteral("turn.completed"), snapshotSession, snapshotTurn,
        QJsonValue(QJsonValue::Null), 3, 32'003);
    runtimeClient->timelineEvent(snapshotStarted);
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSnapshot(
        workbench, snapshotSession, generation, snapshotSubscription, snapshotRequest);
    AgentWorkbenchWidgetTestAccess::setTimelineSubscriptionActivationRequester(
        workbench, snapshotActivate);
    const QJsonObject snapshotItem = timelineSnapshotItemPage(
        snapshotSession, 1, snapshotTurn, QStringLiteral("completed"),
        timelineAnchorForEvent(snapshotItemEvent), timelineAnchorForEvent(snapshotItemEvent),
        snapshotItemEvent.value(QStringLiteral("item")).toObject());
    runtimeClient->timelineSubscriptionSnapshotReceived(
        snapshotRequest,
        timelineSnapshotPage(snapshotSession, timelineAnchorForEvent(snapshotStarted),
                             timelineAnchorForEvent(snapshotCompleted), {}, {snapshotItem},
                             {snapshotItem}, {}, true));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, snapshotSession) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, snapshotSession)
                        == QStringLiteral("awaiting-activation")
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("subscription-snapshot-item")),
                "complete subscription snapshot published before activation")) {
        return false;
    }
    runtimeClient->timelineSubscriptionActivated(
        snapshotActivate,
        timelineSubscriptionActiveResult(
            generation, snapshotSession, snapshotSubscription,
            timelineAnchorForEvent(snapshotCompleted)));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, snapshotSession) == 3
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, snapshotSession) == QStringLiteral("live")
                    && AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("subscription-snapshot-item")),
                "subscription snapshot did not publish after activation")) {
        return false;
    }

    int abandonedConnections = 0;
    QStringList abandonDetails;
    const auto installAbandonRecorder = [&]() {
        AgentWorkbenchWidgetTestAccess::setTimelineSubscriptionConnectionAbandoner(
            workbench, [&abandonedConnections, &abandonDetails](const QString &detail) {
                ++abandonedConnections;
                abandonDetails.append(detail);
            });
    };

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    installAbandonRecorder();
    const QString invalidSyncSession = QStringLiteral("invalid-subscription-sync-session");
    const QString invalidSyncRequest = QStringLiteral("invalid-subscription-sync-request");
    AgentWorkbenchWidgetTestAccess::setPendingPrompt(
        workbench, QStringLiteral("queued-prompt-survives-invalid-sync"));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSync(
        workbench, invalidSyncSession, generation,
        QStringLiteral("invalid-subscription-sync"), invalidSyncRequest,
        timelineAnchorForEvent(completed));
    AgentWorkbenchWidgetTestAccess::setTimelineReconnectBarrier(
        workbench, generation, invalidSyncSession);
    runtimeClient->timelineSubscriptionSynced(
        invalidSyncRequest,
        timelineSyncPage(QStringLiteral("different-session"), {}, {}, {}, true));
    if (!expect(abandonedConnections == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, invalidSyncSession) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelineRetriesOnReconnect(
                           workbench, invalidSyncSession)
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                           workbench, invalidSyncSession)
                    && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                        == QStringLiteral("queued-prompt-survives-invalid-sync")
                    && AgentWorkbenchWidgetTestAccess::timelineReconnectBarrierContains(
                           workbench, generation, invalidSyncSession),
                "invalid subscription sync page did not preserve prompt and reconnect barrier while abandoning the connection")) {
        return false;
    }
    runtimeClient->timelineSubscriptionSynced(
        invalidSyncRequest,
        timelineSyncPage(invalidSyncSession, {}, {}, {}, true));
    if (!expect(abandonedConnections == 1,
                "retired subscription sync response triggered a second connection abandonment")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    installAbandonRecorder();
    const QString invalidSnapshotSession =
        QStringLiteral("invalid-subscription-snapshot-session");
    const QString invalidSnapshotRequest =
        QStringLiteral("invalid-subscription-snapshot-request");
    AgentWorkbenchWidgetTestAccess::setPendingPrompt(
        workbench, QStringLiteral("queued-prompt-survives-invalid-snapshot"));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSnapshot(
        workbench, invalidSnapshotSession, generation,
        QStringLiteral("invalid-subscription-snapshot"), invalidSnapshotRequest);
    AgentWorkbenchWidgetTestAccess::setTimelineReconnectBarrier(
        workbench, generation, invalidSnapshotSession);
    runtimeClient->timelineSubscriptionSnapshotReceived(invalidSnapshotRequest, {});
    if (!expect(abandonedConnections == 2
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, invalidSnapshotSession) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                           workbench, invalidSnapshotSession)
                    && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                        == QStringLiteral("queued-prompt-survives-invalid-snapshot")
                    && AgentWorkbenchWidgetTestAccess::timelineReconnectBarrierContains(
                           workbench, generation, invalidSnapshotSession),
                "invalid subscription snapshot page completed the barrier or lost the queued prompt")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    installAbandonRecorder();
    const QString invalidActivationSession =
        QStringLiteral("invalid-subscription-activation-session");
    const QString invalidActivationRequest =
        QStringLiteral("invalid-subscription-activation-request");
    const QString invalidActivationSubscription =
        QStringLiteral("invalid-subscription-activation");
    const QJsonObject activationStarted = timelineEnvelope(
        QStringLiteral("turn.started"), invalidActivationSession,
        QStringLiteral("invalid-activation-turn"), QJsonValue(QJsonValue::Null),
        1, 33'001);
    runtimeClient->timelineEvent(activationStarted);
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSync(
        workbench, invalidActivationSession, generation,
        invalidActivationSubscription, invalidSyncRequest,
        timelineAnchorForEvent(activationStarted));
    AgentWorkbenchWidgetTestAccess::setTimelineSubscriptionActivationRequester(
        workbench, invalidActivationRequest);
    runtimeClient->timelineSubscriptionSynced(
        invalidSyncRequest,
        timelineSyncPage(invalidActivationSession,
                         timelineAnchorForEvent(activationStarted),
                         timelineAnchorForEvent(activationStarted), {}, true));
    QJsonObject invalidActivation = timelineSubscriptionActiveResult(
        generation, invalidActivationSession, invalidActivationSubscription,
        timelineAnchorForEvent(activationStarted));
    invalidActivation.insert(QStringLiteral("cursor"), QJsonObject{
        {QStringLiteral("sequence"), 0},
        {QStringLiteral("event_id"), QJsonValue(QJsonValue::Null)},
    });
    runtimeClient->timelineSubscriptionActivated(
        invalidActivationRequest, invalidActivation);
    if (!expect(abandonedConnections == 3
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, invalidActivationSession) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                           workbench, invalidActivationSession),
                "invalid subscription activation did not abandon its current generation")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    installAbandonRecorder();
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, snapshotSession);
    runtimeClient->timelineEvent(snapshotStarted);
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSnapshot(
        workbench, snapshotSession, generation, snapshotSubscription, snapshotRequest);
    AgentWorkbenchWidgetTestAccess::setTimelineSubscriptionActivationRequester(
        workbench, snapshotActivate);
    runtimeClient->timelineSubscriptionSnapshotReceived(
        snapshotRequest,
        timelineSnapshotPage(snapshotSession, timelineAnchorForEvent(snapshotStarted),
                             timelineAnchorForEvent(snapshotCompleted), {}, {snapshotItem},
                             {snapshotItem}, {}, true));
    runtimeClient->timelineSubscriptionActivated(
        snapshotActivate,
        timelineSubscriptionActiveResult(
            generation, snapshotSession, snapshotSubscription,
            timelineAnchorForEvent(snapshotCompleted)));
    const QJsonObject driftedLiveEvent = timelineEnvelope(
        QStringLiteral("turn.started"), snapshotSession,
        QStringLiteral("drifted-live-turn"), QJsonValue(QJsonValue::Null), 4, 33'101);
    const QJsonObject driftedWrapper = timelineSubscriptionEventWrapper(
        generation, snapshotSession, snapshotSubscription,
        timelineAnchorForEvent(snapshotItemEvent),
        timelineAnchorForEvent(snapshotCompleted), driftedLiveEvent);
    runtimeClient->timelineSubscriptionEvent(driftedWrapper);
    if (!expect(abandonedConnections == 4
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, snapshotSession) == 3
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, snapshotSession) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                           workbench, snapshotSession),
                "active subscription cursor drift did not preserve the confirmed projection and start a new generation")) {
        return false;
    }
    runtimeClient->timelineSubscriptionEvent(driftedWrapper);
    runtimeClient->timelineSubscriptionActivated(
        snapshotActivate,
        timelineSubscriptionActiveResult(
            generation, snapshotSession, snapshotSubscription,
            timelineAnchorForEvent(snapshotCompleted)));
    if (!expect(abandonedConnections == 4
                    && abandonDetails.size() == 4
                    && AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                           workbench, snapshotSession) == 3,
                "old-generation subscription messages were not inert after authority retirement")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    installAbandonRecorder();
    const QString occupiedSession = QStringLiteral("occupied-subscription-session");
    const QString occupiedSubscription = QStringLiteral("occupied-subscription");
    const QString occupiedRequest = QStringLiteral("occupied-subscription-request");
    AgentWorkbenchWidgetTestAccess::setPendingPrompt(
        workbench, QStringLiteral("queued-prompt-survives-owned-attempt"));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSubscriptionSubscribe(
        workbench, occupiedSession, generation, occupiedSubscription, occupiedRequest);
    runtimeClient->timelineSubscriptionFailed(occupiedRequest, QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("timeline-subscription-failure/0.1")},
        {QStringLiteral("connection_generation"), static_cast<double>(generation)},
        {QStringLiteral("session_id"), occupiedSession},
        {QStringLiteral("subscription_id"), occupiedSubscription},
        {QStringLiteral("state"), QStringLiteral("failed")},
        {QStringLiteral("stage"), QStringLiteral("subscribe")},
        {QStringLiteral("cursor"), QJsonObject{
            {QStringLiteral("sequence"), 0},
            {QStringLiteral("event_id"), QJsonValue(QJsonValue::Null)},
        }},
        {QStringLiteral("watermark"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("request_identity"), QStringLiteral(
            "timeline-subscription-request:sha256:") + QString(64, QLatin1Char('b'))},
        {QStringLiteral("reason"), QStringLiteral("session-attempt-exists")},
        {QStringLiteral("retryable"), true},
        {QStringLiteral("cleanup_required"), true},
    });
    return expect(abandonedConnections == 5
                      && abandonDetails.size() == 5
                      && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                             workbench, occupiedSession) == QStringLiteral("frozen")
                      && AgentWorkbenchWidgetTestAccess::timelineRetriesOnReconnect(
                             workbench, occupiedSession)
                      && AgentWorkbenchWidgetTestAccess::timelineSubscriptionAuthorityCleared(
                             workbench, occupiedSession)
                      && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                          == QStringLiteral("queued-prompt-survives-owned-attempt"),
                  "occupied subscription attempt retried on the same connection");
}

bool verifyTimelineSnapshotRecovery(QApplication &application,
                                    AgentWorkbenchWidget &workbench,
                                    AgentRuntimeClient *runtimeClient)
{
    if (!runtimeClient) return false;
    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString sessionId = QStringLiteral("timeline-snapshot-session");
    const QString oldTurn = QStringLiteral("timeline-snapshot-old-turn");
    const QString activeTurn = QStringLiteral("timeline-snapshot-active-turn");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, sessionId);
    AgentWorkbenchWidgetTestAccess::setTimelineSyncAvailable(workbench, true);
    AgentWorkbenchWidgetTestAccess::setTimelineSnapshotAvailable(workbench, true);
    const QJsonObject oldEvent = timelineEnvelope(
        QStringLiteral("turn.started"), sessionId, oldTurn,
        QJsonValue(QJsonValue::Null), 1, 51'001);
    runtimeClient->timelineEvent(oldEvent);
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionId) == 1,
                "snapshot fixture did not establish the old confirmed projection")) {
        return false;
    }
    AgentWorkbenchWidgetTestAccess::prepareTimelineSync(
        workbench, sessionId, QStringLiteral("timeline-snapshot-sync"));
    const QJsonObject retainedFloor = timelineAnchorForEvent(timelineEnvelope(
        QStringLiteral("turn.started"), sessionId, activeTurn,
        QJsonValue(QJsonValue::Null), 2, 51'002));
    const QJsonObject retainedHead = timelineAnchorForEvent(timelineEnvelope(
        QStringLiteral("item.started"), sessionId, activeTurn,
        QJsonValue(QJsonValue::Null), 4, 51'004));
    int initialSnapshotRequests = 0;
    QString requestedSnapshotSession;
    QString requestedSnapshotIdentity;
    QJsonObject requestedSnapshotWatermark;
    QJsonObject requestedSnapshotAfter;
    int requestedSnapshotLimit = 0;
    AgentWorkbenchWidgetTestAccess::setTimelineSnapshotRequester(
        workbench,
        [&initialSnapshotRequests, &requestedSnapshotSession,
         &requestedSnapshotIdentity, &requestedSnapshotWatermark,
         &requestedSnapshotAfter, &requestedSnapshotLimit](
            const QString &requestedSession, const QString &snapshotIdentity,
            const QJsonObject &watermark, const QJsonObject &after, int limit) {
            ++initialSnapshotRequests;
            requestedSnapshotSession = requestedSession;
            requestedSnapshotIdentity = snapshotIdentity;
            requestedSnapshotWatermark = watermark;
            requestedSnapshotAfter = after;
            requestedSnapshotLimit = limit;
            return QStringLiteral("timeline-snapshot-page-1");
        });
    runtimeClient->timelineRetentionGap(
        QStringLiteral("timeline-snapshot-sync"),
        QJsonObject{
            {QStringLiteral("schema_version"),
             QStringLiteral("timeline-retention-gap/0.1")},
            {QStringLiteral("reason"),
             QStringLiteral("requested-anchor-not-retained")},
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("requested_after"), timelineAnchorForEvent(oldEvent)},
            {QStringLiteral("requested_watermark"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("retained_floor"), retainedFloor},
            {QStringLiteral("head"), retainedHead},
            {QStringLiteral("snapshot_required"), true},
            {QStringLiteral("snapshot_available"), true},
            {QStringLiteral("snapshot_capability"),
             QStringLiteral("timeline.snapshot.current")},
            {QStringLiteral("snapshot_method"), QStringLiteral("timeline/snapshot")},
            {QStringLiteral("event_history_complete"), false},
            {QStringLiteral("replay_from_floor_allowed"), false},
        });
    const QString firstRequest = AgentWorkbenchWidgetTestAccess::timelineSnapshotRequestId(
        workbench, sessionId);
    if (!expect(firstRequest == QStringLiteral("timeline-snapshot-page-1")
                    && initialSnapshotRequests == 1
                    && requestedSnapshotSession == sessionId
                    && requestedSnapshotIdentity.isEmpty()
                    && requestedSnapshotWatermark.isEmpty()
                    && requestedSnapshotAfter.isEmpty()
                    && requestedSnapshotLimit == 200
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionId) == QStringLiteral("syncing"),
                "retention gap did not issue one null-first-page snapshot request")) {
        return false;
    }

    const QJsonObject event2 = timelineEnvelope(
        QStringLiteral("turn.started"), sessionId, activeTurn,
        QJsonValue(QJsonValue::Null), 2, 51'002);
    const QJsonObject event3 = timelineEnvelope(
        QStringLiteral("item.completed"), sessionId, oldTurn,
        QJsonValue(QJsonValue::Null), 3, 51'003);
    const QJsonObject event4 = timelineEnvelope(
        QStringLiteral("item.started"), sessionId, activeTurn,
        QJsonValue(QJsonValue::Null), 4, 51'004);
    const QJsonObject floor = timelineAnchorForEvent(event2);
    const QJsonObject watermark = timelineAnchorForEvent(event4);
    const QJsonObject oldItem = timelineMessage(
        QStringLiteral("snapshot-old-item"), QStringLiteral("completed"),
        QStringLiteral("snapshot history"));
    const QJsonObject openItem = timelineMessage(
        QStringLiteral("snapshot-open-item"), QStringLiteral("started"),
        QStringLiteral("snapshot working"));
    const QJsonObject item1 = timelineSnapshotItemPage(
        sessionId, 1, oldTurn, QStringLiteral("completed"), floor,
        timelineAnchorForEvent(event3), oldItem);
    const QJsonObject item2 = timelineSnapshotItemPage(
        sessionId, 2, activeTurn, QStringLiteral("running"), floor, watermark, openItem);
    const QList<QJsonObject> allItems{item1, item2};
    const QJsonObject firstPage = timelineSnapshotPage(
        sessionId, floor, watermark,
        QJsonObject{
            {QStringLiteral("turn_id"), activeTurn},
            {QStringLiteral("correlation_id"), activeTurn},
            {QStringLiteral("state"), QStringLiteral("running")},
            {QStringLiteral("started_event"), floor},
            {QStringLiteral("latest_event"), watermark},
            {QStringLiteral("open_item_ids"), QJsonArray{QStringLiteral("snapshot-open-item")}},
        },
        allItems, QList<QJsonObject>{item1}, {}, false);
    AgentWorkbenchWidgetTestAccess::setTimelineSnapshotRequester(
        workbench, QStringLiteral("timeline-snapshot-page-2"));
    runtimeClient->timelineSnapshotReceived(firstRequest, firstPage);
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionId) == 1
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                           workbench, QStringLiteral("snapshot-old-item")),
                "incomplete snapshot page changed visible projection")) {
        return false;
    }
    const QString secondRequest = AgentWorkbenchWidgetTestAccess::timelineSnapshotRequestId(
        workbench, sessionId);
    if (!expect(!secondRequest.isEmpty(),
                "incomplete snapshot page did not request its continuation")) {
        return false;
    }
    QJsonObject liveDelta = timelineEnvelope(
        QStringLiteral("item.delta"), sessionId, activeTurn,
        timelineMessage(QStringLiteral("snapshot-open-item"), QStringLiteral("delta"),
                        QStringLiteral("snapshot live delta")),
        5, 51'005, 2);
    runtimeClient->timelineEvent(liveDelta);
    const QJsonObject after = firstPage.value(QStringLiteral("next_after")).toObject();
    const QJsonObject secondPage = timelineSnapshotPage(
        sessionId, floor, watermark,
        firstPage.value(QStringLiteral("active_turn")).toObject(),
        allItems, QList<QJsonObject>{item2}, after, true);
    runtimeClient->timelineSnapshotReceived(secondRequest, secondPage);
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, sessionId) == 5
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, sessionId) == QStringLiteral("live")
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                           workbench, QStringLiteral("old-turn"))
                    && AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                           workbench, QStringLiteral("snapshot-old-item"))
                    && AgentWorkbenchWidgetTestAccess::timelineItemState(
                           workbench, sessionId, activeTurn, QStringLiteral("snapshot-open-item"))
                        == QStringLiteral("delta"),
                "complete snapshot did not atomically replace and drain queued live events")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString closedSession = QStringLiteral("timeline-snapshot-closed-session");
    const QString closedTurn = QStringLiteral("timeline-snapshot-closed-turn");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, closedSession);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), closedSession, closedTurn,
        QJsonValue(QJsonValue::Null), 1, 51'101));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSnapshot(
        workbench, closedSession, QStringLiteral("timeline-snapshot-closed"));
    const QJsonObject closedWatermarkEvent = timelineEnvelope(
        QStringLiteral("turn.completed"), closedSession, closedTurn,
        QJsonValue(QJsonValue::Null), 2, 51'102);
    const QJsonObject closedWatermark = timelineAnchorForEvent(closedWatermarkEvent);
    runtimeClient->timelineSnapshotReceived(
        QStringLiteral("timeline-snapshot-closed"),
        timelineSnapshotPage(closedSession, closedWatermark, closedWatermark, {}, {}, {}, {}, true));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                    workbench, closedSession) == 2
                    && !AgentWorkbenchWidgetTestAccess::turnRunning(workbench),
                "terminal snapshot retained a stale active Turn")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString visibleSession = QStringLiteral("timeline-snapshot-visible-session");
    const QString visibleTurn = QStringLiteral("timeline-snapshot-visible-turn");
    const QString backgroundSession = QStringLiteral("timeline-snapshot-background-session");
    const QString backgroundTurn = QStringLiteral("timeline-snapshot-background-turn");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, visibleSession);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), visibleSession, visibleTurn,
        QJsonValue(QJsonValue::Null), 1, 51'201));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), visibleSession, visibleTurn,
        timelineMessage(QStringLiteral("snapshot-visible-item"),
                        QStringLiteral("completed"), QStringLiteral("visible")),
        2, 51'202));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSnapshot(
        workbench, backgroundSession, QStringLiteral("timeline-snapshot-background"));
    const QJsonObject backgroundEvent = timelineEnvelope(
        QStringLiteral("turn.completed"), backgroundSession, backgroundTurn,
        QJsonValue(QJsonValue::Null), 1, 51'203);
    const QJsonObject backgroundAnchor = timelineAnchorForEvent(backgroundEvent);
    const QJsonObject backgroundItem = timelineSnapshotItemPage(
        backgroundSession, 1, backgroundTurn, QStringLiteral("completed"),
        backgroundAnchor, backgroundAnchor,
        timelineMessage(QStringLiteral("snapshot-background-item"),
                        QStringLiteral("completed"), QStringLiteral("background")));
    runtimeClient->timelineSnapshotReceived(
        QStringLiteral("timeline-snapshot-background"),
        timelineSnapshotPage(backgroundSession, backgroundAnchor, backgroundAnchor, {},
                             {backgroundItem}, {backgroundItem}, {}, true));
    if (!expect(AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                    workbench, QStringLiteral("snapshot-visible-item"))
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("snapshot-background-item"))
                    && AgentWorkbenchWidgetTestAccess::timelineItemState(
                        workbench, backgroundSession, backgroundTurn,
                        QStringLiteral("snapshot-background-item"))
                        == QStringLiteral("completed"),
                "background snapshot contaminated the visible Session or lost its projection")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString disconnectedSession = QStringLiteral("timeline-snapshot-disconnected-session");
    const QString disconnectedTurn = QStringLiteral("timeline-snapshot-disconnected-turn");
    AgentWorkbenchWidgetTestAccess::prepareTimelineSnapshot(
        workbench, disconnectedSession, QStringLiteral("timeline-snapshot-disconnected-page-1"));
    const QJsonObject disconnectedEvent = timelineEnvelope(
        QStringLiteral("turn.completed"), disconnectedSession, disconnectedTurn,
        QJsonValue(QJsonValue::Null), 1, 51'301);
    const QJsonObject disconnectedAnchor = timelineAnchorForEvent(disconnectedEvent);
    const QJsonObject disconnectedItem = timelineSnapshotItemPage(
        disconnectedSession, 1, disconnectedTurn, QStringLiteral("completed"),
        disconnectedAnchor, disconnectedAnchor,
        timelineMessage(QStringLiteral("snapshot-disconnected-item"),
                        QStringLiteral("completed"), QStringLiteral("disconnected")));
    const QJsonObject disconnectedSecondItem = timelineSnapshotItemPage(
        disconnectedSession, 2, disconnectedTurn, QStringLiteral("completed"),
        disconnectedAnchor, disconnectedAnchor,
        timelineMessage(QStringLiteral("snapshot-disconnected-second-item"),
                        QStringLiteral("completed"), QStringLiteral("disconnected second")));
    AgentWorkbenchWidgetTestAccess::setTimelineSnapshotRequester(
        workbench, QStringLiteral("timeline-snapshot-disconnected-page-2"));
    runtimeClient->timelineSnapshotReceived(
        QStringLiteral("timeline-snapshot-disconnected-page-1"),
        timelineSnapshotPage(disconnectedSession, disconnectedAnchor, disconnectedAnchor, {},
                             {disconnectedItem, disconnectedSecondItem},
                             {disconnectedItem}, {}, false));
    AgentWorkbenchWidgetTestAccess::suspendTimelinesForDisconnect(workbench);
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, disconnectedSession) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelineRetriesOnReconnect(
                        workbench, disconnectedSession)
                    && AgentWorkbenchWidgetTestAccess::timelineSnapshotRecoveryRequired(
                        workbench, disconnectedSession)
                    && AgentWorkbenchWidgetTestAccess::timelinePendingEventCount(workbench) == 0,
                "disconnect did not preserve snapshot recovery intent or release private staging")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString driftSession = QStringLiteral("timeline-snapshot-header-drift-session");
    const QString driftTurn = QStringLiteral("timeline-snapshot-header-drift-turn");
    AgentWorkbenchWidgetTestAccess::prepareTimelineSnapshot(
        workbench, driftSession, QStringLiteral("timeline-snapshot-header-drift-page-1"));
    const QJsonObject driftEvent = timelineEnvelope(
        QStringLiteral("turn.completed"), driftSession, driftTurn,
        QJsonValue(QJsonValue::Null), 1, 51'401);
    const QJsonObject driftAnchor = timelineAnchorForEvent(driftEvent);
    const QJsonObject driftItem1 = timelineSnapshotItemPage(
        driftSession, 1, driftTurn, QStringLiteral("completed"), driftAnchor, driftAnchor,
        timelineMessage(QStringLiteral("snapshot-header-drift-item-1"),
                        QStringLiteral("completed"), QStringLiteral("first")));
    const QJsonObject driftItem2 = timelineSnapshotItemPage(
        driftSession, 2, driftTurn, QStringLiteral("completed"), driftAnchor, driftAnchor,
        timelineMessage(QStringLiteral("snapshot-header-drift-item-2"),
                        QStringLiteral("completed"), QStringLiteral("second")));
    const QList<QJsonObject> driftItems{driftItem1, driftItem2};
    const QJsonObject driftPage1 = timelineSnapshotPage(
        driftSession, driftAnchor, driftAnchor, {}, driftItems, {driftItem1}, {}, false);
    AgentWorkbenchWidgetTestAccess::setTimelineSnapshotRequester(
        workbench, QStringLiteral("timeline-snapshot-header-drift-page-2"));
    runtimeClient->timelineSnapshotReceived(
        QStringLiteral("timeline-snapshot-header-drift-page-1"), driftPage1);
    QJsonObject driftPage2 = timelineSnapshotPage(
        driftSession, driftAnchor, driftAnchor, {}, driftItems, {driftItem2},
        driftPage1.value(QStringLiteral("next_after")).toObject(), false);
    driftPage2.insert(QStringLiteral("snapshot_identity"),
                      driftPage1.value(QStringLiteral("snapshot_identity")));
    driftPage2.insert(QStringLiteral("total_items"), 3);
    driftPage2.insert(QStringLiteral("page_identity"),
                      AgentRuntimeClient::timelineSnapshotPageIdentity(driftPage2));
    runtimeClient->timelineSnapshotReceived(
        QStringLiteral("timeline-snapshot-header-drift-page-2"), driftPage2);
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                    workbench, driftSession) == QStringLiteral("frozen")
                    && AgentWorkbenchWidgetTestAccess::timelinePendingEventCount(workbench) == 0
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("snapshot-header-drift-item-1")),
                "snapshot accepted a changed fixed pagination header")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString malformedSession = QStringLiteral("timeline-snapshot-malformed-session");
    const QString malformedTurn = QStringLiteral("timeline-snapshot-malformed-turn");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, malformedSession);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), malformedSession, malformedTurn,
        QJsonValue(QJsonValue::Null), 1, 52'001));
    AgentWorkbenchWidgetTestAccess::prepareTimelineSnapshot(
        workbench, malformedSession, QStringLiteral("timeline-snapshot-malformed"));
    const QJsonObject malformedWatermarkEvent = timelineEnvelope(
        QStringLiteral("turn.completed"), malformedSession, malformedTurn,
        QJsonValue(QJsonValue::Null), 2, 52'002);
    const QJsonObject malformedWatermark = timelineAnchorForEvent(malformedWatermarkEvent);
    const QJsonObject malformedItem = timelineSnapshotItemPage(
        malformedSession, 1, malformedTurn, QStringLiteral("completed"),
        malformedWatermark, malformedWatermark,
        timelineMessage(QStringLiteral("snapshot-must-not-render"),
                        QStringLiteral("completed"), QStringLiteral("forged")));
    QJsonObject malformedPage = timelineSnapshotPage(
        malformedSession, malformedWatermark, malformedWatermark, {},
        QList<QJsonObject>{malformedItem}, QList<QJsonObject>{malformedItem}, {}, true);
    malformedPage.insert(QStringLiteral("page_identity"),
        QStringLiteral("timeline-session-snapshot-page:sha256:")
            + QString(64, QLatin1Char('f')));
    runtimeClient->timelineSnapshotReceived(
        QStringLiteral("timeline-snapshot-malformed"), malformedPage);
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineSequenceForSession(
                      workbench, malformedSession) == 1
                    && AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, malformedSession) == QStringLiteral("frozen")
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                           workbench, QStringLiteral("snapshot-must-not-render")),
                "malformed snapshot page partially changed the visible Session")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString invalidStateSession = QStringLiteral("timeline-snapshot-invalid-state-session");
    const QString invalidStateTurn = QStringLiteral("timeline-snapshot-invalid-state-turn");
    AgentWorkbenchWidgetTestAccess::prepareTimelineSnapshot(
        workbench, invalidStateSession, QStringLiteral("timeline-snapshot-invalid-state"));
    const QJsonObject invalidStateEvent = timelineEnvelope(
        QStringLiteral("turn.completed"), invalidStateSession, invalidStateTurn,
        QJsonValue(QJsonValue::Null), 1, 52'101);
    const QJsonObject invalidStateAnchor = timelineAnchorForEvent(invalidStateEvent);
    const QJsonObject invalidStateItem = timelineSnapshotItemPage(
        invalidStateSession, 1, invalidStateTurn, QStringLiteral("paused"),
        invalidStateAnchor, invalidStateAnchor,
        timelineMessage(QStringLiteral("snapshot-invalid-state-item"),
                        QStringLiteral("completed"), QStringLiteral("invalid")));
    runtimeClient->timelineSnapshotReceived(
        QStringLiteral("timeline-snapshot-invalid-state"),
        timelineSnapshotPage(invalidStateSession, invalidStateAnchor, invalidStateAnchor, {},
                             {invalidStateItem}, {invalidStateItem}, {}, true));
    if (!expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                      workbench, invalidStateSession) == QStringLiteral("frozen")
                    && !AgentWorkbenchWidgetTestAccess::hasTimelineItem(
                        workbench, QStringLiteral("snapshot-invalid-state-item")),
                "snapshot accepted an out-of-contract Turn state")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    const QString invalidOpenSession = QStringLiteral("timeline-snapshot-invalid-open-session");
    const QString invalidOpenTurn = QStringLiteral("timeline-snapshot-invalid-open-turn");
    AgentWorkbenchWidgetTestAccess::prepareTimelineSnapshot(
        workbench, invalidOpenSession, QStringLiteral("timeline-snapshot-invalid-open"));
    const QJsonObject invalidOpenEvent = timelineEnvelope(
        QStringLiteral("turn.started"), invalidOpenSession, invalidOpenTurn,
        QJsonValue(QJsonValue::Null), 1, 52'201);
    const QJsonObject invalidOpenAnchor = timelineAnchorForEvent(invalidOpenEvent);
    const QJsonObject invalidActiveTurn{
        {QStringLiteral("turn_id"), invalidOpenTurn},
        {QStringLiteral("correlation_id"), invalidOpenTurn},
        {QStringLiteral("state"), QStringLiteral("running")},
        {QStringLiteral("started_event"), invalidOpenAnchor},
        {QStringLiteral("latest_event"), invalidOpenAnchor},
        {QStringLiteral("open_item_ids"), QStringLiteral("not-an-array")},
    };
    runtimeClient->timelineSnapshotReceived(
        QStringLiteral("timeline-snapshot-invalid-open"),
        timelineSnapshotPage(invalidOpenSession, invalidOpenAnchor, invalidOpenAnchor,
                             invalidActiveTurn, {}, {}, {}, true));
    return expect(AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                      workbench, invalidOpenSession) == QStringLiteral("frozen"),
                  "snapshot accepted non-array open Item identities");
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

QJsonObject proposalLatestResult(const QString &sessionId, QLatin1Char fill,
                                 const QByteArray &diffBytes = QByteArrayLiteral("diff\n"))
{
    const QString proposalId = QStringLiteral("workspace-edit-proposal:sha256:")
        + QString(64, fill);
    const QString diffSha = QString::fromLatin1(
        QCryptographicHash::hash(diffBytes, QCryptographicHash::Sha256).toHex());
    const QString baseSha(64, QLatin1Char('a'));
    const QString contentSha(64, QLatin1Char('b'));
    const QJsonObject format{
        {QStringLiteral("encoding"), QStringLiteral("utf-8")},
        {QStringLiteral("newline"), QStringLiteral("lf")},
        {QStringLiteral("mode"), QStringLiteral("preserve")},
    };
    const QJsonObject diff{
        {QStringLiteral("reference"),
         QStringLiteral("workspace-edit-diff:sha256:") + diffSha},
        {QStringLiteral("sha256"), diffSha},
        {QStringLiteral("bytes"), diffBytes.size()},
        {QStringLiteral("media_type"), QStringLiteral("text/x-diff; charset=utf-8")},
        {QStringLiteral("inline_truncated"), diffBytes.size() > 32 * 1024},
        {QStringLiteral("source_truncated"), false},
    };
    const QJsonObject file{
        {QStringLiteral("ordinal"), 0},
        {QStringLiteral("summary_state"), QStringLiteral("complete")},
        {QStringLiteral("kind"), QStringLiteral("update")},
        {QStringLiteral("path"), QStringLiteral("src/main.cpp")},
        {QStringLiteral("additions"), 1},
        {QStringLiteral("deletions"), 1},
        {QStringLiteral("base_matches"), true},
        {QStringLiteral("base"), QJsonObject{
            {QStringLiteral("sha256"), baseSha}, {QStringLiteral("bytes"), 4},
        }},
        {QStringLiteral("proposed"), QJsonObject{
            {QStringLiteral("reference"),
             QStringLiteral("workspace-edit-content:sha256:") + contentSha},
            {QStringLiteral("hash"), QJsonObject{
                {QStringLiteral("sha256"), contentSha}, {QStringLiteral("bytes"), 4},
            }},
            {QStringLiteral("encoding"), QStringLiteral("utf-8")},
            {QStringLiteral("newline"), QStringLiteral("lf")},
            {QStringLiteral("mode"), QStringLiteral("preserve")},
        }},
        {QStringLiteral("proposed_format"), format},
        {QStringLiteral("warnings"), QJsonArray{}},
        {QStringLiteral("diff"), diff},
    };
    QJsonObject proposal{
        {QStringLiteral("schema_version"), QStringLiteral("workspace-edit-proposal-view/0.1")},
        {QStringLiteral("internal_schema_version"), QStringLiteral("workspace-edit-proposal/0.2")},
        {QStringLiteral("proposal_id"), proposalId},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("turn_id"), QStringLiteral("turn-proposal")},
        {QStringLiteral("project_id"), QStringLiteral("project-proposal")},
        {QStringLiteral("root_id"), QStringLiteral("root-1")},
        {QStringLiteral("edit_id"), QStringLiteral("edit-proposal")},
        {QStringLiteral("canonical_edit_identity"),
         QStringLiteral("workspace-edit-canonical:sha256:") + QString(64, QLatin1Char('d'))},
        {QStringLiteral("preview_identity"),
         QStringLiteral("workspace-edit-preview:sha256:") + QString(64, QLatin1Char('e'))},
        {QStringLiteral("proposal_sha256"), QString(64, QLatin1Char('f'))},
        {QStringLiteral("proposal_bytes"), 2048},
        {QStringLiteral("event_sequence"), 7},
        {QStringLiteral("created_at_ms"), 1000},
        {QStringLiteral("approval_started_at_ms"), 999},
        {QStringLiteral("provider_identity"),
         QStringLiteral("codex-provider:sha256:") + QString(64, QLatin1Char('a'))},
        {QStringLiteral("provider_thread_identity"),
         QStringLiteral("codex-provider-thread:sha256:") + QString(64, QLatin1Char('b'))},
        {QStringLiteral("provider_item_identity"),
         QStringLiteral("codex-file-change-item:sha256:") + QString(64, QLatin1Char('c'))},
        {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("adapter"), QStringLiteral("codex-app-server")},
            {QStringLiteral("adapter_version"), QStringLiteral("codex-cli 0.144.5")},
            {QStringLiteral("permission"), QStringLiteral("read-only")},
        }},
        {QStringLiteral("summary"), QJsonObject{
            {QStringLiteral("files_complete"), true},
            {QStringLiteral("file_count"), 1},
            {QStringLiteral("additions"), 1},
            {QStringLiteral("deletions"), 1},
            {QStringLiteral("warning_count"), 0},
            {QStringLiteral("applicable"), true},
            {QStringLiteral("aggregate_diff"), diff},
            {QStringLiteral("files"), QJsonArray{file}},
        }},
        {QStringLiteral("file_mutation_authority"), false},
        {QStringLiteral("approval_recorded"), false},
        {QStringLiteral("apply_available"), false},
    };
    proposal.insert(QStringLiteral("preview_identity"),
                    AgentRuntimeClient::workspaceEditProposalPreviewIdentity(proposal));
    return {
        {QStringLiteral("schema_version"), QStringLiteral("workspace-edit-proposal-latest/0.1")},
        {QStringLiteral("session_id"), sessionId},
        {QStringLiteral("proposal"), proposal},
        {QStringLiteral("file_mutation_authority"), false},
        {QStringLiteral("approval_recorded"), false},
        {QStringLiteral("apply_available"), false},
    };
}

QJsonObject proposalReadResult(const QString &sessionId, QLatin1Char fill)
{
    QJsonObject result = proposalLatestResult(sessionId, fill);
    result.insert(QStringLiteral("schema_version"),
                  QStringLiteral("workspace-edit-proposal-read/0.1"));
    return result;
}

QJsonObject proposalTimelineReference(const QJsonObject &proposalResult,
                                      const QString &itemId)
{
    const QJsonObject proposal = proposalResult.value(QStringLiteral("proposal")).toObject();
    const QJsonObject summary = proposal.value(QStringLiteral("summary")).toObject();
    QJsonObject reference{
        {QStringLiteral("schema_version"),
         QStringLiteral("workspace-edit-proposal-reference/0.1")},
        {QStringLiteral("session_id"), proposal.value(QStringLiteral("session_id"))},
        {QStringLiteral("turn_id"), proposal.value(QStringLiteral("turn_id"))},
        {QStringLiteral("proposal_id"), proposal.value(QStringLiteral("proposal_id"))},
        {QStringLiteral("project_id"), proposal.value(QStringLiteral("project_id"))},
        {QStringLiteral("root_id"), proposal.value(QStringLiteral("root_id"))},
        {QStringLiteral("edit_id"), proposal.value(QStringLiteral("edit_id"))},
        {QStringLiteral("preview_identity"),
         proposal.value(QStringLiteral("preview_identity"))},
        {QStringLiteral("file_count"), summary.value(QStringLiteral("file_count"))},
        {QStringLiteral("additions"), summary.value(QStringLiteral("additions"))},
        {QStringLiteral("deletions"), summary.value(QStringLiteral("deletions"))},
        {QStringLiteral("warning_count"),
         summary.value(QStringLiteral("warning_count"))},
        {QStringLiteral("applicable"), summary.value(QStringLiteral("applicable"))},
        {QStringLiteral("file_mutation_authority"), false},
        {QStringLiteral("approval_recorded"), false},
        {QStringLiteral("apply_available"), false},
    };
    QJsonObject identityMaterial = reference;
    identityMaterial.insert(QStringLiteral("item_id"), itemId);
    reference.insert(QStringLiteral("reference_id"),
        QStringLiteral("workspace-edit-proposal-reference:sha256:")
            + QString::fromLatin1(QCryptographicHash::hash(
                QJsonDocument(identityMaterial).toJson(QJsonDocument::Compact),
                QCryptographicHash::Sha256).toHex()));
    return reference;
}

QJsonObject proposalTimelineItem(const QString &itemId,
                                 const QJsonObject &reference)
{
    return {
        {QStringLiteral("id"), itemId},
        {QStringLiteral("kind"), QStringLiteral("file-change")},
        {QStringLiteral("role"), QStringLiteral("tool")},
        {QStringLiteral("state"), QStringLiteral("completed")},
        {QStringLiteral("content"), QStringLiteral("持久化只读变更提案")},
        {QStringLiteral("data"), reference},
    };
}

QJsonObject completeProposalAllKindsResult(const QString &sessionId,
                                           QLatin1Char fill)
{
    QJsonObject result = proposalLatestResult(sessionId, fill);
    QJsonObject proposal = result.value(QStringLiteral("proposal")).toObject();
    QJsonObject summary = proposal.value(QStringLiteral("summary")).toObject();
    const QJsonObject updateTemplate = summary.value(QStringLiteral("files"))
        .toArray().first().toObject();
    const QJsonObject base = updateTemplate.value(QStringLiteral("base")).toObject();
    const QJsonObject proposed = updateTemplate.value(QStringLiteral("proposed")).toObject();
    const QJsonObject format = updateTemplate.value(QStringLiteral("proposed_format")).toObject();
    const QJsonObject diff = updateTemplate.value(QStringLiteral("diff")).toObject();
    auto file = [&](int ordinal, const QString &kind, const QString &path,
                    int additions, int deletions) {
        QJsonObject proposedValue = proposed;
        QJsonObject formatValue = format;
        if (kind == QStringLiteral("create")) {
            proposedValue.insert(QStringLiteral("mode"), QStringLiteral("regular"));
            formatValue.insert(QStringLiteral("mode"), QStringLiteral("regular"));
        }
        QJsonObject value{
            {QStringLiteral("ordinal"), ordinal},
            {QStringLiteral("summary_state"), QStringLiteral("complete")},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("path"), path},
            {QStringLiteral("additions"), additions},
            {QStringLiteral("deletions"), deletions},
            {QStringLiteral("base_matches"),
             kind == QStringLiteral("create") ? QJsonValue(QJsonValue::Null)
                                               : QJsonValue(true)},
            {QStringLiteral("base"),
             kind == QStringLiteral("create") ? QJsonValue(QJsonValue::Null)
                                               : QJsonValue(base)},
            {QStringLiteral("proposed"),
             (kind == QStringLiteral("create") || kind == QStringLiteral("update"))
                 ? QJsonValue(proposedValue) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("warnings"), QJsonArray{}},
            {QStringLiteral("diff"), diff},
        };
        if (kind == QStringLiteral("rename")) {
            value.insert(QStringLiteral("from_path"), QStringLiteral("src/old-name.cpp"));
        }
        if (kind == QStringLiteral("create") || kind == QStringLiteral("update")) {
            value.insert(QStringLiteral("proposed_format"), formatValue);
        }
        return value;
    };
    const QJsonArray files{
        file(0, QStringLiteral("create"), QStringLiteral("src/new.cpp"), 1, 0),
        file(1, QStringLiteral("update"), QStringLiteral("src/main.cpp"), 2, 1),
        file(2, QStringLiteral("delete"), QStringLiteral("src/obsolete.cpp"), 0, 2),
        file(3, QStringLiteral("rename"), QStringLiteral("src/new-name.cpp"), 0, 0),
    };
    summary.insert(QStringLiteral("file_count"), files.size());
    summary.insert(QStringLiteral("additions"), 3);
    summary.insert(QStringLiteral("deletions"), 3);
    summary.insert(QStringLiteral("files"), files);
    proposal.insert(QStringLiteral("summary"), summary);
    proposal.insert(QStringLiteral("preview_identity"),
                    AgentRuntimeClient::workspaceEditProposalPreviewIdentity(proposal));
    result.insert(QStringLiteral("proposal"), proposal);
    return result;
}

QJsonObject legacyProposalLatestResult(const QString &sessionId, QLatin1Char fill)
{
    QJsonObject result = proposalLatestResult(sessionId, fill);
    QJsonObject proposal = result.value(QStringLiteral("proposal")).toObject();
    proposal.insert(QStringLiteral("internal_schema_version"),
                    QStringLiteral("workspace-edit-proposal/0.1"));
    QJsonObject summary = proposal.value(QStringLiteral("summary")).toObject();
    summary.insert(QStringLiteral("files_complete"), false);
    QJsonObject aggregate = summary.value(QStringLiteral("aggregate_diff")).toObject();
    aggregate.insert(QStringLiteral("inline_truncated"), QJsonValue(QJsonValue::Null));
    aggregate.insert(QStringLiteral("source_truncated"), QJsonValue(QJsonValue::Null));
    summary.insert(QStringLiteral("aggregate_diff"), aggregate);
    QJsonObject file = summary.value(QStringLiteral("files")).toArray().first().toObject();
    file.insert(QStringLiteral("summary_state"), QStringLiteral("legacy-incomplete"));
    file.insert(QStringLiteral("from_path"), QJsonValue(QJsonValue::Null));
    file.insert(QStringLiteral("additions"), QJsonValue(QJsonValue::Null));
    file.insert(QStringLiteral("deletions"), QJsonValue(QJsonValue::Null));
    file.insert(QStringLiteral("base_matches"), QJsonValue(QJsonValue::Null));
    file.insert(QStringLiteral("warnings"), QJsonValue(QJsonValue::Null));
    file.remove(QStringLiteral("proposed_format"));
    QJsonObject fileDiff = file.value(QStringLiteral("diff")).toObject();
    fileDiff.insert(QStringLiteral("inline_truncated"), QJsonValue(QJsonValue::Null));
    fileDiff.insert(QStringLiteral("source_truncated"), QJsonValue(QJsonValue::Null));
    file.insert(QStringLiteral("diff"), fileDiff);
    summary.insert(QStringLiteral("files"), QJsonArray{file});
    proposal.insert(QStringLiteral("summary"), summary);
    proposal.insert(QStringLiteral("preview_identity"),
                    AgentRuntimeClient::workspaceEditProposalPreviewIdentity(proposal));
    result.insert(QStringLiteral("proposal"), proposal);
    return result;
}

QJsonObject proposalArtifactPage(const QJsonObject &latestResult,
                                 const QByteArray &content,
                                 int offset,
                                 int limit = 64 * 1024)
{
    const QJsonObject proposal = latestResult.value(QStringLiteral("proposal")).toObject();
    const QJsonObject diff = proposal.value(QStringLiteral("summary")).toObject()
        .value(QStringLiteral("aggregate_diff")).toObject();
    const QByteArray chunk = content.mid(offset, limit);
    const int end = offset + chunk.size();
    QJsonObject page{
        {QStringLiteral("schema_version"),
         QStringLiteral("workspace-edit-proposal-artifact-page/0.1")},
        {QStringLiteral("session_id"), proposal.value(QStringLiteral("session_id"))},
        {QStringLiteral("proposal_id"), proposal.value(QStringLiteral("proposal_id"))},
        {QStringLiteral("project_id"), proposal.value(QStringLiteral("project_id"))},
        {QStringLiteral("edit_id"), proposal.value(QStringLiteral("edit_id"))},
        {QStringLiteral("artifact"), QJsonObject{
            {QStringLiteral("kind"), QStringLiteral("diff")},
            {QStringLiteral("reference"), diff.value(QStringLiteral("reference"))},
            {QStringLiteral("sha256"), diff.value(QStringLiteral("sha256"))},
            {QStringLiteral("bytes"), content.size()},
            {QStringLiteral("media_type"),
             QStringLiteral("text/x-diff; charset=utf-8")},
        }},
        {QStringLiteral("offset"), offset},
        {QStringLiteral("next_offset"),
         end < content.size() ? QJsonValue(end) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("total_bytes"), content.size()},
        {QStringLiteral("data_base64"), QString::fromLatin1(chunk.toBase64())},
        {QStringLiteral("chunk_sha256"), QString::fromLatin1(
            QCryptographicHash::hash(chunk, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("page_identity"), QString()},
        {QStringLiteral("file_mutation_authority"), false},
        {QStringLiteral("approval_recorded"), false},
        {QStringLiteral("apply_available"), false},
    };
    page.insert(QStringLiteral("page_identity"),
                AgentRuntimeClient::workspaceEditProposalArtifactPageIdentity(page));
    return page;
}

bool verifyDurableProposalUtf8Paging(AgentWorkbenchWidget &workbench,
                                     AgentRuntimeClient *runtime,
                                     QLabel *summary,
                                     QPlainTextEdit *diffView,
                                     QPushButton *moreButton)
{
    if (!runtime || !summary || !diffView || !moreButton) return false;
    QByteArray artifact(65535, 'a');
    artifact += QByteArray::fromHex("e7958c");
    artifact += '\n';
    const QString sessionId = QStringLiteral("proposal-utf8-session");
    const QJsonObject result = proposalLatestResult(
        sessionId, QLatin1Char('3'), artifact);
    AgentWorkbenchWidgetTestAccess::setProposalContext(
        workbench, sessionId, QStringLiteral("project-proposal"));
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-utf8"), sessionId, 1);
    runtime->workspaceEditProposalLatestRead(QStringLiteral("proposal-utf8"), result);

    AgentWorkbenchWidgetTestAccess::prepareCurrentProposalArtifactRequest(
        workbench, QStringLiteral("proposal-utf8-page-1"));
    runtime->workspaceEditProposalArtifactRead(
        QStringLiteral("proposal-utf8-page-1"),
        proposalArtifactPage(result, artifact, 0));
    if (!expect(AgentWorkbenchWidgetTestAccess::proposalArtifactOffset(workbench)
                        == 64 * 1024
                    && AgentWorkbenchWidgetTestAccess::proposalArtifactBytes(workbench)
                        == artifact.left(64 * 1024)
                    && moreButton->isEnabled()
                    && diffView->toPlainText().toUtf8() == artifact.left(65535)
                    && !diffView->toPlainText().contains(QChar::ReplacementCharacter)
                    && !summary->text().contains(QStringLiteral("无效"))
                    && !summary->text().contains(QStringLiteral("完整性校验失败")),
                "first Proposal artifact page corrupted a split UTF-8 sequence")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareCurrentProposalArtifactRequest(
        workbench, QStringLiteral("proposal-utf8-page-2"));
    runtime->workspaceEditProposalArtifactRead(
        QStringLiteral("proposal-utf8-page-2"),
        proposalArtifactPage(result, artifact, 64 * 1024));
    if (!expect(AgentWorkbenchWidgetTestAccess::proposalArtifactBytes(workbench)
                        == artifact
                    && AgentWorkbenchWidgetTestAccess::proposalArtifactOffset(workbench)
                        == artifact.size()
                    && diffView->toPlainText().toUtf8() == artifact
                    && !diffView->toPlainText().contains(QChar::ReplacementCharacter)
                    && !moreButton->isEnabled()
                    && AgentWorkbenchWidgetTestAccess::proposalArtifactPendingCount(workbench)
                        == 0,
                "complete Proposal artifact did not decode from whole UTF-8 bytes")) {
        return false;
    }

    const QString emptySession = QStringLiteral("proposal-empty-session");
    const QByteArray emptyArtifact;
    const QJsonObject emptyResult = proposalLatestResult(
        emptySession, QLatin1Char('6'), emptyArtifact);
    AgentWorkbenchWidgetTestAccess::setProposalContext(
        workbench, emptySession, QStringLiteral("project-proposal"));
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-empty"), emptySession, 1);
    runtime->workspaceEditProposalLatestRead(QStringLiteral("proposal-empty"), emptyResult);
    AgentWorkbenchWidgetTestAccess::prepareCurrentProposalArtifactRequest(
        workbench, QStringLiteral("proposal-empty-page"));
    runtime->workspaceEditProposalArtifactRead(
        QStringLiteral("proposal-empty-page"),
        proposalArtifactPage(emptyResult, emptyArtifact, 0));
    if (!expect(AgentWorkbenchWidgetTestAccess::proposalArtifactOffset(workbench) == 0
                    && AgentWorkbenchWidgetTestAccess::proposalArtifactBytes(workbench).isEmpty()
                    && diffView->toPlainText().isEmpty()
                    && !moreButton->isEnabled()
                    && AgentWorkbenchWidgetTestAccess::proposalArtifactPendingCount(workbench)
                        == 0
                    && !summary->text().contains(QStringLiteral("无效"))
                    && !summary->text().contains(QStringLiteral("完整性校验失败")),
                "zero-byte Proposal artifact page was rejected")) {
        return false;
    }

    const QList<QPair<QByteArray, char>> invalidTails{
        {QByteArray(1, static_cast<char>(0x80)), '9'},
        {QByteArray(1, static_cast<char>(0xc0)), 'a'},
        {QByteArray(1, static_cast<char>(0xf5)), 'b'},
    };
    for (qsizetype index = 0; index < invalidTails.size(); ++index) {
        QByteArray invalidArtifact(65535, 'a');
        invalidArtifact += invalidTails.at(index).first;
        invalidArtifact += 'x';
        const QString invalidSession = QStringLiteral("proposal-invalid-utf8-%1")
            .arg(index);
        const QJsonObject invalidResult = proposalLatestResult(
            invalidSession, QLatin1Char(invalidTails.at(index).second), invalidArtifact);
        AgentWorkbenchWidgetTestAccess::setProposalContext(
            workbench, invalidSession, QStringLiteral("project-proposal"));
        const QString proposalRequest = QStringLiteral("proposal-invalid-utf8-request-%1")
            .arg(index);
        AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
            workbench, proposalRequest, invalidSession, 1);
        runtime->workspaceEditProposalLatestRead(proposalRequest, invalidResult);
        const QString pageRequest = QStringLiteral("proposal-invalid-utf8-page-%1")
            .arg(index);
        AgentWorkbenchWidgetTestAccess::prepareCurrentProposalArtifactRequest(
            workbench, pageRequest);
        runtime->workspaceEditProposalArtifactRead(
            pageRequest, proposalArtifactPage(invalidResult, invalidArtifact, 0));
        if (!expect(AgentWorkbenchWidgetTestAccess::proposalArtifactOffset(workbench) == 0
                        && AgentWorkbenchWidgetTestAccess::proposalArtifactBytes(workbench).isEmpty()
                        && !moreButton->isEnabled()
                        && summary->text().contains(QStringLiteral("完整性校验失败")),
                    "invalid UTF-8 tail was accepted as a repairable page prefix")) {
            return false;
        }
    }
    return true;
}

bool verifyStaleProposalArtifactResponseDiscarded(AgentWorkbenchWidget &workbench,
                                                  AgentRuntimeClient *runtime,
                                                  QLabel *summary,
                                                  QPlainTextEdit *diffView,
                                                  QPushButton *moreButton)
{
    if (!runtime || !summary || !diffView || !moreButton) return false;
    const QString projectId = QStringLiteral("project-proposal");
    const QString sessionA = QStringLiteral("proposal-race-session-a");
    const QByteArray artifactA("proposal A diff\n");
    const QJsonObject resultA = proposalLatestResult(
        sessionA, QLatin1Char('4'), artifactA);
    AgentWorkbenchWidgetTestAccess::setProposalContext(workbench, sessionA, projectId);
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-race-a"), sessionA, 1);
    runtime->workspaceEditProposalLatestRead(QStringLiteral("proposal-race-a"), resultA);
    AgentWorkbenchWidgetTestAccess::prepareCurrentProposalArtifactRequest(
        workbench, QStringLiteral("proposal-race-old-page"));

    const QString sessionB = QStringLiteral("proposal-race-session-b");
    const QByteArray artifactB("proposal B different diff\n");
    const QJsonObject resultB = proposalLatestResult(
        sessionB, QLatin1Char('5'), artifactB);
    AgentWorkbenchWidgetTestAccess::setProposalContext(workbench, sessionB, projectId);
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-race-b"), sessionB, 1);
    runtime->workspaceEditProposalLatestRead(QStringLiteral("proposal-race-b"), resultB);

    const QString proposalBefore =
        AgentWorkbenchWidgetTestAccess::currentProposalId(workbench);
    const QString referenceBefore =
        AgentWorkbenchWidgetTestAccess::proposalArtifactReference(workbench);
    const quint64 generationBefore =
        AgentWorkbenchWidgetTestAccess::proposalArtifactGeneration(workbench);
    const qint64 offsetBefore =
        AgentWorkbenchWidgetTestAccess::proposalArtifactOffset(workbench);
    const QByteArray bytesBefore =
        AgentWorkbenchWidgetTestAccess::proposalArtifactBytes(workbench);
    const QString textBefore = diffView->toPlainText();
    const QString summaryBefore = summary->text();
    const bool moreBefore = moreButton->isEnabled();

    runtime->workspaceEditProposalArtifactRead(
        QStringLiteral("proposal-race-old-page"),
        proposalArtifactPage(resultA, artifactA, 0));
    return expect(AgentWorkbenchWidgetTestAccess::currentProposalId(workbench)
                          == proposalBefore
                      && AgentWorkbenchWidgetTestAccess::proposalArtifactReference(workbench)
                          == referenceBefore
                      && AgentWorkbenchWidgetTestAccess::proposalArtifactGeneration(workbench)
                          == generationBefore
                      && AgentWorkbenchWidgetTestAccess::proposalArtifactOffset(workbench)
                          == offsetBefore
                      && AgentWorkbenchWidgetTestAccess::proposalArtifactBytes(workbench)
                          == bytesBefore
                      && diffView->toPlainText() == textBefore
                      && summary->text() == summaryBefore
                      && moreButton->isEnabled() == moreBefore
                      && AgentWorkbenchWidgetTestAccess::proposalArtifactPendingCount(workbench)
                          == 0,
                  "stale Proposal artifact response contaminated the active view");
}

bool verifyProposalSchemaVariants(AgentWorkbenchWidget &workbench,
                                  AgentRuntimeClient *runtime,
                                  QTreeWidget *files)
{
    if (!runtime || !files) return false;
    const QString projectId = QStringLiteral("project-proposal");
    const QString completeSession = QStringLiteral("proposal-all-kinds-session");
    const QJsonObject complete = completeProposalAllKindsResult(
        completeSession, QLatin1Char('7'));
    const QString completeId = complete.value(QStringLiteral("proposal")).toObject()
        .value(QStringLiteral("proposal_id")).toString();
    AgentWorkbenchWidgetTestAccess::setProposalContext(
        workbench, completeSession, projectId);
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-all-kinds"), completeSession, 1);
    runtime->workspaceEditProposalLatestRead(
        QStringLiteral("proposal-all-kinds"), complete);
    if (!expect(files->topLevelItemCount() == 5
                    && AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                        workbench, completeSession, completeId)
                    && files->topLevelItem(1)->text(1) == QStringLiteral("create")
                    && files->topLevelItem(2)->text(1) == QStringLiteral("update")
                    && files->topLevelItem(3)->text(1) == QStringLiteral("delete")
                    && files->topLevelItem(4)->text(1) == QStringLiteral("rename"),
                "complete Proposal did not accept all four operation kinds")) {
        return false;
    }

    const QString legacySession = QStringLiteral("proposal-legacy-session");
    const QJsonObject legacy = legacyProposalLatestResult(
        legacySession, QLatin1Char('8'));
    const QString legacyId = legacy.value(QStringLiteral("proposal")).toObject()
        .value(QStringLiteral("proposal_id")).toString();
    AgentWorkbenchWidgetTestAccess::setProposalContext(
        workbench, legacySession, projectId);
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-legacy"), legacySession, 1);
    runtime->workspaceEditProposalLatestRead(QStringLiteral("proposal-legacy"), legacy);
    if (!expect(files->topLevelItemCount() == 2
                    && AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                        workbench, legacySession, legacyId),
                "exact legacy Proposal public view was rejected")) {
        return false;
    }

    QJsonObject invalid = complete;
    QJsonObject invalidProposal = invalid.value(QStringLiteral("proposal")).toObject();
    QJsonObject invalidSummary = invalidProposal.value(QStringLiteral("summary")).toObject();
    QJsonArray invalidFiles = invalidSummary.value(QStringLiteral("files")).toArray();
    QJsonObject invalidUpdate = invalidFiles.at(1).toObject();
    invalidUpdate.insert(QStringLiteral("from_path"), QJsonValue(QJsonValue::Null));
    invalidFiles.replace(1, invalidUpdate);
    invalidSummary.insert(QStringLiteral("files"), invalidFiles);
    invalidProposal.insert(QStringLiteral("summary"), invalidSummary);
    invalidProposal.insert(QStringLiteral("preview_identity"),
        AgentRuntimeClient::workspaceEditProposalPreviewIdentity(invalidProposal));
    invalid.insert(QStringLiteral("proposal"), invalidProposal);
    AgentWorkbenchWidgetTestAccess::setProposalContext(
        workbench, completeSession, projectId);
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-invalid-optional-key"), completeSession, 2);
    runtime->workspaceEditProposalLatestRead(
        QStringLiteral("proposal-invalid-optional-key"), invalid);
    if (!expect(AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                    workbench, completeSession, completeId)
                    && AgentWorkbenchWidgetTestAccess::proposalUnverified(
                        workbench, completeSession)
                    && files->topLevelItemCount() == 0,
                "complete Proposal accepted a forbidden optional field")) {
        return false;
    }
#ifdef Q_OS_WIN
    const QString driveSession = QStringLiteral("proposal-drive-prefix-session");
    QJsonObject driveResult = complete;
    driveResult.insert(QStringLiteral("session_id"), driveSession);
    QJsonObject driveProposal = driveResult.value(QStringLiteral("proposal")).toObject();
    driveProposal.insert(QStringLiteral("session_id"), driveSession);
    QJsonObject driveSummary = driveProposal.value(QStringLiteral("summary")).toObject();
    QJsonArray driveFiles = driveSummary.value(QStringLiteral("files")).toArray();
    QJsonObject driveCreate = driveFiles.at(0).toObject();
    driveCreate.insert(QStringLiteral("path"), QStringLiteral("C:relative.txt"));
    driveFiles.replace(0, driveCreate);
    driveSummary.insert(QStringLiteral("files"), driveFiles);
    driveProposal.insert(QStringLiteral("summary"), driveSummary);
    driveProposal.insert(QStringLiteral("preview_identity"),
        AgentRuntimeClient::workspaceEditProposalPreviewIdentity(driveProposal));
    driveResult.insert(QStringLiteral("proposal"), driveProposal);
    AgentWorkbenchWidgetTestAccess::setProposalContext(
        workbench, driveSession, projectId);
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-drive-prefix"), driveSession, 1);
    runtime->workspaceEditProposalLatestRead(
        QStringLiteral("proposal-drive-prefix"), driveResult);
    if (!expect(!AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                        workbench, driveSession,
                        driveProposal.value(QStringLiteral("proposal_id")).toString())
                    && files->topLevelItemCount() == 0,
                "Windows Proposal accepted a drive-prefixed workspace path")) {
        return false;
    }
#endif
    return true;
}

bool verifyDurableProposalProjection(AgentWorkbenchWidget &workbench,
                                     AgentRuntimeClient *runtime,
                                     QTabWidget *tabs,
                                     QLabel *summary,
                                     QTreeWidget *files)
{
    if (!runtime || !tabs || !summary || !files) return false;
    const QString foreground = QStringLiteral("proposal-foreground-session");
    const QJsonObject valid = proposalLatestResult(foreground, QLatin1Char('1'));
    const QString proposalId = valid.value(QStringLiteral("proposal")).toObject()
        .value(QStringLiteral("proposal_id")).toString();
    AgentWorkbenchWidgetTestAccess::setProposalContext(
        workbench, foreground, QStringLiteral("project-proposal"));
    tabs->setCurrentIndex(0);
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-valid"), foreground, 1);
    runtime->workspaceEditProposalLatestRead(QStringLiteral("proposal-valid"), valid);
    const int changesTab = AgentWorkbenchWidgetTestAccess::workspaceEditTab(workbench);
    if (!expect(tabs->currentIndex() == changesTab
                    && summary->text().contains(QStringLiteral("持久化只读提案"))
                    && files->topLevelItemCount() == 2
                    && AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                        workbench, foreground, proposalId),
                "foreground durable Proposal did not auto-open validated Changes")) {
        return false;
    }
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-refresh-failure"), foreground, 2);
    runtime->requestFailedExact(QStringLiteral("proposal-refresh-failure"),
                                QStringLiteral("workspace/edit/proposal/latest"),
                                QStringLiteral("redacted"),
                                QStringLiteral("-32115"));
    if (!expect(AgentWorkbenchWidgetTestAccess::proposalUnverified(
                        workbench, foreground)
                    && AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                        workbench, foreground, proposalId)
                    && summary->text().contains(QStringLiteral("重新验证失败")),
                "failed latest Proposal refresh left the confirmed cache active")) {
        return false;
    }
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-refresh-recovered"), foreground, 3);
    runtime->workspaceEditProposalLatestRead(
        QStringLiteral("proposal-refresh-recovered"), valid);
    if (!expect(!AgentWorkbenchWidgetTestAccess::proposalUnverified(
                        workbench, foreground)
                    && summary->text().contains(QStringLiteral("持久化只读提案")),
                "valid latest Proposal refresh did not reactivate the cache")) {
        return false;
    }
    QWidget *changesPage = summary->parentWidget();
    for (QPushButton *button : changesPage->findChildren<QPushButton *>()) {
        const QString text = button->text().toLower();
        if (!expect(!text.contains(QStringLiteral("apply"))
                        && !text.contains(QStringLiteral("approve"))
                        && !text.contains(QStringLiteral("reject"))
                        && !text.contains(QStringLiteral("应用"))
                        && !text.contains(QStringLiteral("批准"))
                        && !text.contains(QStringLiteral("拒绝")),
                    "Changes exposed a forbidden mutation or approval control")) return false;
    }
    tabs->setCurrentIndex(1);
    const QString background = QStringLiteral("proposal-background-session");
    const QJsonObject backgroundResult = proposalLatestResult(background, QLatin1Char('2'));
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-background"), background, 1);
    runtime->workspaceEditProposalLatestRead(
        QStringLiteral("proposal-background"), backgroundResult);
    if (!expect(tabs->currentIndex() == 1
                    && AgentWorkbenchWidgetTestAccess::proposalUnread(workbench, background)
                    && tabs->tabText(changesTab).contains(QChar(0x2022)),
                "background Proposal stole focus or omitted unread state")) return false;

    QJsonObject invalidAuthority = valid;
    invalidAuthority.insert(QStringLiteral("file_mutation_authority"), true);
    AgentWorkbenchWidgetTestAccess::prepareCurrentProposalArtifactRequest(
        workbench, QStringLiteral("proposal-invalidated-artifact"));
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-authority"), foreground, 4);
    runtime->workspaceEditProposalLatestRead(
        QStringLiteral("proposal-authority"), invalidAuthority);
    const QString invalidatedSummary = summary->text();
    runtime->workspaceEditProposalArtifactRead(
        QStringLiteral("proposal-invalidated-artifact"),
        proposalArtifactPage(valid, QByteArrayLiteral("diff\n"), 0));
    if (!expect(summary->text() == invalidatedSummary
                    && files->topLevelItemCount() == 0
                    && AgentWorkbenchWidgetTestAccess::proposalArtifactPendingCount(workbench)
                        == 0,
                "invalid Proposal refresh left an artifact response able to repopulate Changes")) {
        return false;
    }
    QJsonObject invalidSchema = valid;
    QJsonObject invalidProposal = invalidSchema.value(QStringLiteral("proposal")).toObject();
    invalidProposal.insert(QStringLiteral("internal_schema_version"),
                           QStringLiteral("workspace-edit-proposal/0.1"));
    invalidSchema.insert(QStringLiteral("proposal"), invalidProposal);
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-schema"), foreground, 5);
    runtime->workspaceEditProposalLatestRead(QStringLiteral("proposal-schema"), invalidSchema);
    if (!expect(AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                    workbench, foreground, proposalId),
                "invalid Proposal authority/schema replaced confirmed cache")) return false;

    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("proposal-pending"), foreground, 6);
    AgentWorkbenchWidgetTestAccess::clearProposalPending(workbench);
    return expect(AgentWorkbenchWidgetTestAccess::proposalPendingCount(workbench) == 0
                    && AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                        workbench, foreground, proposalId),
                  "disconnect cleanup discarded confirmed Proposal or retained pending generation");
}

bool verifyTimelineProposalReference(AgentWorkbenchWidget &workbench,
                                     AgentRuntimeClient *runtime,
                                     QTabWidget *tabs,
                                     QLabel *changesSummary)
{
    if (!runtime || !tabs || !changesSummary) return false;
    const QString sessionId = QStringLiteral("proposal-timeline-session");
    const QString itemId = QStringLiteral("proposal-timeline-reference");
    const QJsonObject exactResult = proposalReadResult(sessionId, QLatin1Char('3'));
    const QJsonObject exactProposal = exactResult.value(QStringLiteral("proposal")).toObject();
    const QString exactProposalId = exactProposal.value(
        QStringLiteral("proposal_id")).toString();
    const QString turnId = exactProposal.value(QStringLiteral("turn_id")).toString();
    const QJsonObject reference = proposalTimelineReference(exactResult, itemId);
    const QJsonObject timelineItem = proposalTimelineItem(itemId, reference);
    AgentWorkbenchWidgetTestAccess::setProposalContext(
        workbench, sessionId, QStringLiteral("project-proposal"));

    if (!expect(AgentWorkbenchWidgetTestAccess::validateTimelineItem(
                    workbench, timelineItem, sessionId, turnId),
                "valid file-change Timeline reference was rejected")) {
        return false;
    }
    QJsonObject forgedIdentityItem = timelineItem;
    QJsonObject forgedIdentityData = reference;
    forgedIdentityData.insert(QStringLiteral("reference_id"),
        QStringLiteral("workspace-edit-proposal-reference:sha256:")
            + QString(64, QLatin1Char('f')));
    forgedIdentityItem.insert(QStringLiteral("data"), forgedIdentityData);
    if (!expect(!AgentWorkbenchWidgetTestAccess::validateTimelineItem(
                    workbench, forgedIdentityItem, sessionId, turnId),
                "file-change Timeline reference accepted a forged identity")) {
        return false;
    }
    QJsonObject unknownKeyItem = timelineItem;
    QJsonObject unknownKeyData = reference;
    unknownKeyData.insert(QStringLiteral("path"), QStringLiteral("secret.txt"));
    unknownKeyItem.insert(QStringLiteral("data"), unknownKeyData);
    if (!expect(!AgentWorkbenchWidgetTestAccess::validateTimelineItem(
                    workbench, unknownKeyItem, sessionId, turnId),
                "file-change Timeline reference accepted an unknown/path field")) {
        return false;
    }
    QJsonObject authorityItem = timelineItem;
    QJsonObject authorityData = reference;
    authorityData.insert(QStringLiteral("apply_available"), true);
    authorityItem.insert(QStringLiteral("data"), authorityData);
    if (!expect(!AgentWorkbenchWidgetTestAccess::validateTimelineItem(
                    workbench, authorityItem, sessionId, turnId),
                "file-change Timeline reference accepted Apply authority")) {
        return false;
    }

    runtime->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), sessionId, turnId));
    runtime->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), sessionId, turnId, timelineItem));
    QPushButton *viewButton = workbench.findChild<QPushButton *>(
        QStringLiteral("timelineFileChangeButton"));
    QLabel *referenceSummary = workbench.findChild<QLabel *>(
        QStringLiteral("timelineFileChangeSummary"));
    QLabel *referenceStatus = workbench.findChild<QLabel *>(
        QStringLiteral("timelineFileChangeStatus"));
    if (!expect(viewButton && referenceSummary && referenceStatus
                    && viewButton->text() == QStringLiteral("查看变更")
                    && referenceSummary->text().contains(QStringLiteral("1 个文件"))
                    && referenceSummary->text().contains(QStringLiteral("+1 / -1")),
                "file-change Timeline reference did not render its read-only action")) {
        return false;
    }

    const QJsonObject latest = proposalLatestResult(sessionId, QLatin1Char('1'));
    const QString latestProposalId = latest.value(QStringLiteral("proposal")).toObject()
        .value(QStringLiteral("proposal_id")).toString();
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("timeline-reference-latest"), sessionId, 1);
    runtime->workspaceEditProposalLatestRead(
        QStringLiteral("timeline-reference-latest"), latest);
    AgentWorkbenchWidgetTestAccess::prepareProposalReferenceRequest(
        workbench, QStringLiteral("timeline-reference-exact"), itemId, reference, 1);
    tabs->setCurrentIndex(0);
    runtime->workspaceEditProposalRead(
        QStringLiteral("timeline-reference-exact"), exactResult);
    if (!expect(tabs->currentIndex()
                    == AgentWorkbenchWidgetTestAccess::workspaceEditTab(workbench)
                    && AgentWorkbenchWidgetTestAccess::currentProposalId(workbench)
                        == exactProposalId
                    && AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                        workbench, sessionId, latestProposalId)
                    && referenceStatus->text().isEmpty(),
                "exact Timeline Proposal read polluted latest cache or failed to focus")) {
        return false;
    }

    QJsonObject drifted = exactResult;
    QJsonObject driftedProposal = drifted.value(QStringLiteral("proposal")).toObject();
    driftedProposal.insert(QStringLiteral("turn_id"), QStringLiteral("turn-drifted"));
    drifted.insert(QStringLiteral("proposal"), driftedProposal);
    AgentWorkbenchWidgetTestAccess::prepareProposalReferenceRequest(
        workbench, QStringLiteral("timeline-reference-drift"), itemId, reference, 2);
    runtime->workspaceEditProposalRead(QStringLiteral("timeline-reference-drift"), drifted);
    if (!expect(AgentWorkbenchWidgetTestAccess::currentProposalId(workbench)
                    == exactProposalId
                    && AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                        workbench, sessionId, latestProposalId)
                    && referenceStatus->text().contains(QStringLiteral("校验失败")),
                "drifted exact Proposal response changed visible or latest state")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::prepareProposalReferenceRequest(
        workbench, QStringLiteral("timeline-reference-race"), itemId, reference, 3);
    const QJsonObject newerLatest = proposalLatestResult(sessionId, QLatin1Char('5'));
    const QString newerLatestId = newerLatest.value(QStringLiteral("proposal")).toObject()
        .value(QStringLiteral("proposal_id")).toString();
    AgentWorkbenchWidgetTestAccess::prepareProposalRequest(
        workbench, QStringLiteral("timeline-reference-racing-latest"), sessionId, 2);
    runtime->workspaceEditProposalLatestRead(
        QStringLiteral("timeline-reference-racing-latest"), newerLatest);
    if (!expect(AgentWorkbenchWidgetTestAccess::currentProposalId(workbench)
                    == exactProposalId
                    && AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                        workbench, sessionId, newerLatestId),
                "latest refresh stole focus from an explicit Timeline reference")) {
        return false;
    }
    runtime->workspaceEditProposalRead(
        QStringLiteral("timeline-reference-race"), exactResult);

    AgentWorkbenchWidgetTestAccess::prepareProposalReferenceRequest(
        workbench, QStringLiteral("timeline-reference-background"), itemId, reference, 4);
    AgentWorkbenchWidgetTestAccess::setWorkSession(
        workbench, QStringLiteral("another-work-session"));
    tabs->setCurrentIndex(0);
    runtime->workspaceEditProposalRead(
        QStringLiteral("timeline-reference-background"), exactResult);
    if (!expect(tabs->currentIndex() == 0
                    && AgentWorkbenchWidgetTestAccess::currentProposalId(workbench)
                        == exactProposalId,
                "background exact Proposal response stole focus or visible state")) {
        return false;
    }

    AgentWorkbenchWidgetTestAccess::setProposalContext(
        workbench, sessionId, QStringLiteral("project-proposal"));
    AgentWorkbenchWidgetTestAccess::prepareProposalReferenceRequest(
        workbench, QStringLiteral("timeline-reference-error"), itemId, reference, 5);
    runtime->requestFailedExact(QStringLiteral("timeline-reference-error"),
                                QStringLiteral("workspace/edit/proposal/read"),
                                QStringLiteral("redacted"),
                                QStringLiteral("-32149"));
    if (!expect(referenceStatus->text().contains(QStringLiteral("错误码 -32149"))
                    && AgentWorkbenchWidgetTestAccess::hasConfirmedProposal(
                        workbench, sessionId, newerLatestId),
                "exact Proposal failure invalidated latest cache or omitted retry state")) {
        return false;
    }
    for (QPushButton *button : workbench.findChildren<QPushButton *>()) {
        const QString text = button->text().toLower();
        if (!expect(!text.contains(QStringLiteral("apply"))
                        && !text.contains(QStringLiteral("approve"))
                        && !text.contains(QStringLiteral("应用"))
                        && !text.contains(QStringLiteral("批准")),
                    "Timeline Proposal reference exposed mutation authority")) {
            return false;
        }
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
#ifdef Q_OS_WIN
    // Headless Windows runners cannot always launch sandboxed WebEngine
    // renderer processes; the render fixtures exercise the workbench, not
    // the Chromium sandbox itself.
    if (qEnvironmentVariableIsEmpty("QTWEBENGINE_DISABLE_SANDBOX")) {
        qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");
    }
    if (qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS")) {
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
                "--disable-gpu --disable-gpu-compositing --no-sandbox "
                "--enable-logging=stderr");
    }
#endif
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

    if (qEnvironmentVariableIsSet("AEGISY_CONTEXT_THRESHOLD_CACHE_TEST_ONLY")) {
        const QString protectedSessionId = QStringLiteral("threshold-cache-protected");
        AgentWorkbenchWidgetTestAccess::setCurrentChatSession(
            workbench, protectedSessionId);
        AgentWorkbenchWidgetTestAccess::storeContextThreshold(
            workbench, protectedSessionId, QJsonObject{
                {QStringLiteral("schema_version"),
                 QStringLiteral("session-context-threshold/0.1")},
                {QStringLiteral("status"), QStringLiteral("no_action")},
                {QStringLiteral("source"), QStringLiteral("runtime-authoritative")},
                {QStringLiteral("history_state"), QStringLiteral("active")},
                {QStringLiteral("automatic_compaction_authority"), false},
            });
        return verifyBoundedContextThresholdCache(workbench, protectedSessionId) ? 0 : 1;
    }
    if (qEnvironmentVariableIsSet("AEGISY_TIMELINE_SNAPSHOT_TEST_ONLY")) {
        AgentRuntimeClient *runtimeClient = workbench.findChild<AgentRuntimeClient *>();
        return verifyTimelineSnapshotRecovery(application, workbench, runtimeClient)
            ? 0 : 1;
    }
    if (qEnvironmentVariableIsSet("AEGISY_TIMELINE_SUBSCRIPTION_TEST_ONLY")) {
        AgentRuntimeClient *runtimeClient = workbench.findChild<AgentRuntimeClient *>();
        return verifyTimelineSubscriptionRecovery(application, workbench, runtimeClient)
            ? 0 : 1;
    }
    if (qEnvironmentVariableIsSet("AEGISY_RUNTIME_DEGRADATION_TEST_ONLY")) {
        AgentRuntimeClient *runtimeClient = workbench.findChild<AgentRuntimeClient *>();
        QLabel *runtimeCapability = workbench.findChild<QLabel *>(
            QStringLiteral("agentRuntimeCapabilityStatus"));
        QTextEdit *composer = workbench.findChild<QTextEdit *>(
            QStringLiteral("agentComposer"));
        QPushButton *sendButton = workbench.findChild<QPushButton *>(
            QStringLiteral("agentSendButton"));
        if (!expect(runtimeClient && runtimeCapability && composer && sendButton,
                    "focused degradation fixture could not find workbench controls")) {
            return 1;
        }
        runtimeClient->runtimeInitialized(QJsonObject{
            {QStringLiteral("backend"), QJsonObject{
                {QStringLiteral("status"), QStringLiteral("ready")},
            }},
        });
        application.processEvents();
        AgentWorkbenchWidgetTestAccess::prepareRuntimeDegradationRequest(
            workbench, QStringLiteral("initial-degradation-fixture"));
        composer->setPlainText(QStringLiteral("must-wait-for-capability-check"));
        AgentWorkbenchWidgetTestAccess::submitPrompt(workbench);
        AgentWorkbenchWidgetTestAccess::setPendingPrompt(
            workbench, QStringLiteral("pending-turn-must-wait"));
        AgentWorkbenchWidgetTestAccess::tryStartPendingTurn(workbench);
        if (!expect(sendButton->text() == QStringLiteral("能力检查中")
                        && !sendButton->isEnabled()
                        && composer->toPlainText()
                            == QStringLiteral("must-wait-for-capability-check")
                        && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                            == QStringLiteral("pending-turn-must-wait"),
                    "pending degradation negotiation did not block every new-Turn path")) {
            return 1;
        }
        runtimeClient->runtimeDegradationsRead(
            QStringLiteral("invalid-before-valid"), QJsonObject{});
        application.processEvents();
        if (!expect(sendButton->text() == QStringLiteral("能力未知")
                        && !sendButton->isEnabled(),
                    "invalid degradation response did not fail closed")) {
            return 1;
        }
        AgentWorkbenchWidgetTestAccess::prepareRuntimeDegradationRequest(
            workbench, QStringLiteral("degradation-request"));
        runtimeClient->requestFailedExact(QStringLiteral("degradation-request"),
                                          QStringLiteral("runtime/degradations"),
                                          QStringLiteral("bounded failure"),
                                          QStringLiteral("-32000"));
        application.processEvents();
        if (!expect(sendButton->text() == QStringLiteral("能力未知")
                        && !sendButton->isEnabled(),
                    "degradation request failure did not fail closed")) {
            return 1;
        }
        AgentWorkbenchWidgetTestAccess::setPendingPrompt(workbench, QString());
        AgentWorkbenchWidgetTestAccess::prepareRuntimeDegradationRequest(
            workbench, QStringLiteral("valid-codex"));
        runtimeClient->runtimeDegradationsRead(
            QStringLiteral("valid-codex"), validCodexRuntimeDegradationSnapshot());
        application.processEvents();
        return expect(runtimeCapability
                          && runtimeCapability->text().contains(QStringLiteral("Agent 只读")),
                      "valid Codex degradation matrix was rejected")
                && expect(AgentWorkbenchWidgetTestAccess::activeTurnSubmitIsInert(
                              workbench, composer, sendButton),
                          "active-turn submit was not inert")
                && verifyRuntimeHealthDegradationRefresh(
                    application, workbench, runtimeClient, runtimeCapability,
                    composer, sendButton)
                && verifyStrictTimelineValidation(application, workbench, runtimeClient)
                && verifySessionScopedTimelineSequences(
                    application, workbench, runtimeClient)
                && verifyTimelineGapRecovery(application, workbench, runtimeClient)
                && verifyTimelineSubscriptionRecovery(application, workbench, runtimeClient)
                && verifyTimelineSnapshotRecovery(application, workbench, runtimeClient)
                && verifyRuntimeDegradationFailures(
                    application, workbench, runtimeClient, runtimeCapability)
                && expect(AgentWorkbenchWidgetTestAccess::activeTurnSubmitIsInert(
                              workbench, composer, sendButton),
                          "invalid degradation state hid or disabled the active Stop action")
            ? 0 : 1;
    }

    QPushButton *runtimeRestart = workbench.findChild<QPushButton *>(
        QStringLiteral("agentRuntimeRestartButton"));
    QLabel *runtimeStatus = workbench.findChild<QLabel *>(QStringLiteral("agentRuntimeStatus"));
    QLabel *runtimeCapability = workbench.findChild<QLabel *>(
        QStringLiteral("agentRuntimeCapabilityStatus"));
    QLabel *executionContext = workbench.findChild<QLabel *>(
        QStringLiteral("agentExecutionContextStrip"));
    QLabel *emptyTimeline = workbench.findChild<QLabel *>(
        QStringLiteral("agentEmptyTimeline"));
    if (!expect(emptyTimeline && !emptyTimeline->isHidden()
                    && emptyTimeline->text().contains(
                        QStringLiteral("从这里开始与 Aegisy Agent 对话")),
                "timeline empty state is missing or not stably addressable")) {
        return 1;
    }

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
    if (!expect(composer && send
                    && waitUntil(application, [send]() { return send->isEnabled(); }),
                "validated runtime capabilities did not enable the composer")) {
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
    const QString previewTurnSessionId =
        AgentWorkbenchWidgetTestAccess::currentSessionId(workbench);
    if (!expect(!previewTurnSessionId.isEmpty()
                    && waitUntil(application, [&workbench, &previewTurnSessionId]() {
                        return !AgentWorkbenchWidgetTestAccess::mutationReconciliationBlocked(
                                   workbench, previewTurnSessionId)
                            && AgentWorkbenchWidgetTestAccess::
                                terminalMutationAcknowledgementConsumed(
                                    workbench, previewTurnSessionId);
                    }),
                "response-before-events mutation anchors were not consumed in order")) {
        return 1;
    }
#endif

    QSplitter *splitter = workbench.findChild<QSplitter *>();
    QPushButton *resetLayout = workbench.findChild<QPushButton *>(
        QStringLiteral("agentResetWorkbenchLayoutButton"));
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
    QLabel *fileStatus = workbench.findChild<QLabel *>(QStringLiteral("agentFileStatus"));
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
    QListWidget *projectList = workbench.findChild<QListWidget *>(
        QStringLiteral("agentProjectList"));
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
    if (splitter && resetLayout) {
        splitter->setSizes({260, 260, 260});
        resetLayout->click();
        const QList<int> restored = splitter->sizes();
        if (!expect(restored.size() == 3 && restored.at(0) >= 150
                        && restored.at(0) <= 230 && restored.at(1) >= 300
                        && restored.at(2) >= 400,
                    "workbench layout reset did not restore the default pane proportions")) {
            return 1;
        }
    }
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
            || !expect(resetLayout
                           && resetLayout->toolTip().contains(QStringLiteral("默认布局")),
                       "workbench layout reset control is missing")
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
            || !expect(editorPath && editorMeta && fileStatus
                           && fileStatus->text()
                               == QStringLiteral("打开文件夹后显示受授权项目内容")
                           && editorSave && editorReload && editorSplit
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
                           && terminalNewForeground && projectList && sessionList && sessionSearch
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

    {
        AgentWorkbenchWidget proposalWorkbench;
        proposalWorkbench.resize(900, 620);
        QTabWidget *proposalTabs = proposalWorkbench.findChild<QTabWidget *>(
            QStringLiteral("agentWorkspaceTabs"));
        QLabel *proposalSummary = proposalWorkbench.findChild<QLabel *>(
            QStringLiteral("agentWorkspaceEditSummary"));
        QTreeWidget *proposalFiles = proposalWorkbench.findChild<QTreeWidget *>(
            QStringLiteral("agentWorkspaceEditFiles"));
        AgentRuntimeClient *proposalRuntime =
            proposalWorkbench.findChild<AgentRuntimeClient *>();
        if (!verifyDurableProposalProjection(
                proposalWorkbench, proposalRuntime, proposalTabs,
                proposalSummary, proposalFiles)) {
            return 1;
        }
    }

    {
        AgentWorkbenchWidget pagingWorkbench;
        QLabel *pagingSummary = pagingWorkbench.findChild<QLabel *>(
            QStringLiteral("agentWorkspaceEditSummary"));
        QPlainTextEdit *pagingDiff = pagingWorkbench.findChild<QPlainTextEdit *>(
            QStringLiteral("agentWorkspaceEditDiff"));
        QPushButton *pagingMore = pagingWorkbench.findChild<QPushButton *>(
            QStringLiteral("agentWorkspaceEditMoreButton"));
        AgentRuntimeClient *pagingRuntime =
            pagingWorkbench.findChild<AgentRuntimeClient *>();
        if (!verifyDurableProposalUtf8Paging(
                pagingWorkbench, pagingRuntime, pagingSummary, pagingDiff, pagingMore)) {
            return 1;
        }
    }

    {
        AgentWorkbenchWidget referenceWorkbench;
        referenceWorkbench.resize(900, 620);
        QTabWidget *referenceTabs = referenceWorkbench.findChild<QTabWidget *>(
            QStringLiteral("agentWorkspaceTabs"));
        QLabel *referenceChangesSummary = referenceWorkbench.findChild<QLabel *>(
            QStringLiteral("agentWorkspaceEditSummary"));
        AgentRuntimeClient *referenceRuntime =
            referenceWorkbench.findChild<AgentRuntimeClient *>();
        if (!verifyTimelineProposalReference(
                referenceWorkbench, referenceRuntime, referenceTabs,
                referenceChangesSummary)) {
            return 1;
        }
    }

    {
        AgentWorkbenchWidget raceWorkbench;
        QLabel *raceSummary = raceWorkbench.findChild<QLabel *>(
            QStringLiteral("agentWorkspaceEditSummary"));
        QPlainTextEdit *raceDiff = raceWorkbench.findChild<QPlainTextEdit *>(
            QStringLiteral("agentWorkspaceEditDiff"));
        QPushButton *raceMore = raceWorkbench.findChild<QPushButton *>(
            QStringLiteral("agentWorkspaceEditMoreButton"));
        AgentRuntimeClient *raceRuntime =
            raceWorkbench.findChild<AgentRuntimeClient *>();
        if (!verifyStaleProposalArtifactResponseDiscarded(
                raceWorkbench, raceRuntime, raceSummary, raceDiff, raceMore)) {
            return 1;
        }
    }

    {
        AgentWorkbenchWidget schemaWorkbench;
        QTreeWidget *schemaFiles = schemaWorkbench.findChild<QTreeWidget *>(
            QStringLiteral("agentWorkspaceEditFiles"));
        AgentRuntimeClient *schemaRuntime =
            schemaWorkbench.findChild<AgentRuntimeClient *>();
        if (!verifyProposalSchemaVariants(
                schemaWorkbench, schemaRuntime, schemaFiles)) {
            return 1;
        }
    }

    runtimeClient->runtimeInitialized(QJsonObject{
        {QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("status"), QStringLiteral("read-only-recovery")},
        }},
    });
    application.processEvents();
    if (!expect(!recoveryBanner->isHidden()
                    && recoveryBanner->text().contains(QStringLiteral("只读诊断"))
                    && sendButton->text() == QStringLiteral("能力检查中")
                    && !sendButton->isEnabled() && !newSession->isEnabled()
                    && !openFolder->isEnabled() && !importSession->isEnabled(),
                "pending degradation negotiation did not override recovery with a safe gate")) {
        return 1;
    }
    if (!expect(runtimeCapability->text() == QStringLiteral("能力未知")
                    && runtimeCapability->toolTip().contains(QStringLiteral("只读门控")),
                "runtime capability status did not fail closed in recovery mode")) {
        return 1;
    }
    const QJsonObject modelProfileFixture{
        {QStringLiteral("schema_version"), QStringLiteral("model-profile-list/0.1")},
        {QStringLiteral("store_schema_version"), QStringLiteral("model-profile-store/0.1")},
        {QStringLiteral("profiles"), QJsonArray{QJsonObject{
            {QStringLiteral("profile_id"), QStringLiteral("fixture-profile")},
            {QStringLiteral("revision"), 0},
        }}},
        {QStringLiteral("selection_allowed"), false},
        {QStringLiteral("routing_authority"), false},
        {QStringLiteral("token_issued"), false},
        {QStringLiteral("turn_started"), false},
    };
    runtimeClient->modelProfilesListed(QStringLiteral("profile-list-unnegotiated"),
                                       modelProfileFixture);
    application.processEvents();
    if (!expect(!modelPicker->toolTip().contains(QStringLiteral("Profile 元数据只读")),
                "unnegotiated model-profile metadata became visible")) {
        return 1;
    }
    runtimeClient->runtimeInitialized(QJsonObject{
        {QStringLiteral("backend"), QJsonObject{
            {QStringLiteral("status"), QStringLiteral("ready")},
        }},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("stable"), QJsonArray{
                QStringLiteral("background-job.recovery.inspect"),
                QStringLiteral("background-notification.outbox.read-only"),
                QStringLiteral("session.compaction.checkpoint-review"),
                QStringLiteral("turn.context.pinned-selected"),
                QStringLiteral("workspace.git-context.read-only"),
                QStringLiteral("workspace.image.import-user"),
                QStringLiteral("workspace.image.preview"),
                QStringLiteral("model.catalog.cache.read-only"),
                QStringLiteral("model.catalog.refresh.status.read-only"),
                QStringLiteral("model.profile.read-only"),
                QStringLiteral("timeline.replay.fixed-watermark"),
                QStringLiteral("session.mutation-acknowledgements"),
            }},
            {QStringLiteral("experimental"), QJsonArray{}},
        }},
    });
    AgentWorkbenchWidgetTestAccess::prepareRuntimeDegradationRequest(
        workbench, QStringLiteral("degradation-fixture"));
    runtimeClient->runtimeDegradationsRead(
        QStringLiteral("degradation-fixture"), validCodexRuntimeDegradationSnapshot());
    runtimeClient->projectionRecoveryStatusRead(QJsonObject{
        {QStringLiteral("startup"), QJsonObject{
            {QStringLiteral("rebuilt_sessions"), 2},
        }},
        {QStringLiteral("current_quarantined_sessions"), 0},
    });
    runtimeClient->modelCatalogCacheRead(QStringLiteral("cache-fixture"), QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("model-catalog-cache/0.1")},
        {QStringLiteral("availability"), QStringLiteral("empty")},
        {QStringLiteral("selection_allowed"), false},
    });
    runtimeClient->modelCatalogRead(QStringLiteral("catalog-offline-fixture"), QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("model-catalog/0.1")},
        {QStringLiteral("state"), QStringLiteral("offline")},
        {QStringLiteral("contains_credentials"), false},
        {QStringLiteral("refresh_supported"), false},
        {QStringLiteral("models"), QJsonArray{QJsonObject{
            {QStringLiteral("model_id"), QStringLiteral("aegisy:fixture")},
        }}},
    });
    runtimeClient->modelCatalogRefreshStatusRead(
        QStringLiteral("refresh-status-fixture"), QJsonObject{
            {QStringLiteral("schema_version"),
             QStringLiteral("model-catalog-refresh-status/0.1")},
            {QStringLiteral("state"), QStringLiteral("unconfigured")},
            {QStringLiteral("authenticated_transport_required"), true},
            {QStringLiteral("conditional_requests_supported"), true},
            {QStringLiteral("response_body_retained"), false},
            {QStringLiteral("credentials_included"), false},
            {QStringLiteral("cache_install_authority"), false},
            {QStringLiteral("selection_allowed"), false},
        });
    application.processEvents();
    if (!expect(!recoveryBanner->isHidden()
                    && recoveryBanner->text().contains(QStringLiteral("自动恢复 2 个"))
                    && sendButton->isEnabled() && newSession->isEnabled()
                    && openFolder->isEnabled() && importSession->isEnabled(),
                "startup projection recovery did not render a non-blocking notice")) {
        return 1;
    }
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("目录缓存为空"))
                    && modelPicker->toolTip().contains(QStringLiteral("目录离线"))
                    && modelPicker->toolTip().contains(QStringLiteral("目录刷新未配置")),
                "model catalog cache and refresh did not remain explicit read-only states")) {
        return 1;
    }
    runtimeClient->modelCapabilityChecked(QStringLiteral("capability-unknown-fixture"),
                                          QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("model-capability-check/0.1")},
        {QStringLiteral("model_id"), QStringLiteral("aegisy:fixture")},
        {QStringLiteral("decision"), QStringLiteral("unknown")},
        {QStringLiteral("selection_allowed"), false},
        {QStringLiteral("checks"), QJsonArray{}},
        {QStringLiteral("mismatches"), QJsonArray{}},
    });
    application.processEvents();
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("能力未知")),
                "unknown capability preflight did not render")) {
        return 1;
    }
    runtimeClient->modelCapabilityChecked(QStringLiteral("capability-blocked-fixture"),
                                          QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("model-capability-check/0.1")},
        {QStringLiteral("model_id"), QStringLiteral("aegisy:fixture")},
        {QStringLiteral("decision"), QStringLiteral("blocked")},
        {QStringLiteral("selection_allowed"), false},
        {QStringLiteral("checks"), QJsonArray{}},
        {QStringLiteral("mismatches"), QJsonArray{QJsonObject{
            {QStringLiteral("code"), QStringLiteral("fixture-blocked")},
        }}},
    });
    application.processEvents();
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("能力受限")),
                "blocked capability preflight did not render")) {
        return 1;
    }
    runtimeClient->modelCapabilityChecked(QStringLiteral("capability-compatible-fixture"),
                                          QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("model-capability-check/0.1")},
        {QStringLiteral("model_id"), QStringLiteral("aegisy:fixture")},
        {QStringLiteral("decision"), QStringLiteral("compatible")},
        {QStringLiteral("selection_allowed"), true},
        {QStringLiteral("checks"), QJsonArray{}},
        {QStringLiteral("mismatches"), QJsonArray{}},
    });
    application.processEvents();
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("能力兼容（仍只读）")),
                "compatible capability preflight did not preserve the read-only boundary")) {
        return 1;
    }
    runtimeClient->modelCapabilityChecked(QStringLiteral("capability-invalid-fixture"),
                                          QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("model-capability-check/0.1")},
        {QStringLiteral("model_id"), QStringLiteral("aegisy:fixture")},
        {QStringLiteral("decision"), QStringLiteral("unknown")},
        {QStringLiteral("selection_allowed"), true},
        {QStringLiteral("checks"), QJsonArray{}},
        {QStringLiteral("mismatches"), QJsonArray{}},
    });
    application.processEvents();
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("能力状态无效")),
                "inconsistent capability authority did not fail closed")) {
        return 1;
    }
    runtimeClient->modelCatalogCacheRead(QStringLiteral("cache-fresh-fixture"), QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("model-catalog-cache/0.1")},
        {QStringLiteral("availability"), QStringLiteral("fresh")},
        {QStringLiteral("selection_allowed"), false},
    });
    application.processEvents();
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("目录缓存新鲜")),
                "fresh catalog cache state did not render")) {
        return 1;
    }
    runtimeClient->modelCatalogCacheRead(QStringLiteral("cache-stale-fixture"), QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("model-catalog-cache/0.1")},
        {QStringLiteral("availability"), QStringLiteral("stale")},
        {QStringLiteral("selection_allowed"), false},
    });
    application.processEvents();
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("目录缓存陈旧")),
                "stale catalog cache state did not render")) {
        return 1;
    }
    runtimeClient->modelCatalogCacheRead(QStringLiteral("cache-expired-fixture"), QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("model-catalog-cache/0.1")},
        {QStringLiteral("availability"), QStringLiteral("expired")},
        {QStringLiteral("selection_allowed"), false},
    });
    application.processEvents();
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("目录缓存过期")),
                "expired catalog cache state did not render")) {
        return 1;
    }
    runtimeClient->modelCatalogCacheRead(QStringLiteral("cache-malformed-fixture"), QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("model-catalog-cache/0.1")},
        {QStringLiteral("availability"), QStringLiteral("fresh")},
        {QStringLiteral("selection_allowed"), true},
    });
    application.processEvents();
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("目录缓存无效")),
                "malformed or authority-bearing cache state did not fail closed")) {
        return 1;
    }
    runtimeClient->modelCatalogRead(QStringLiteral("catalog-malformed-fixture"), QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("model-catalog/0.1")},
        {QStringLiteral("state"), QStringLiteral("unexpected")},
        {QStringLiteral("contains_credentials"), false},
        {QStringLiteral("refresh_supported"), false},
    });
    application.processEvents();
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("目录状态无效")),
                "malformed catalog response did not fail closed")) {
        return 1;
    }
    const bool degradationProjected =
        runtimeCapability->text().contains(QStringLiteral("Agent 只读"))
        && runtimeCapability->text().contains(QStringLiteral("Compact 不可用"))
        && runtimeCapability->text().contains(QStringLiteral("删除不可用"))
        && runtimeCapability->toolTip().contains(QStringLiteral("不会显示为成功"));
    if (!degradationProjected) {
        qCritical() << "runtime degradation projection diagnostics"
                    << runtimeCapability->text()
                    << runtimeCapability->toolTip();
    }
    if (!expect(degradationProjected,
                "runtime degradations were not projected into a fail-closed capability state")) {
        return 1;
    }
    AgentWorkbenchWidgetTestAccess::setMutationAcknowledgementAvailable(
        workbench, false);
    AgentWorkbenchWidgetTestAccess::setPendingPrompt(
        workbench, QStringLiteral("queued-without-durable-acknowledgement"));
    AgentWorkbenchWidgetTestAccess::tryStartPendingTurn(workbench);
    if (!expect(sendButton->text() == QStringLiteral("确认能力未知")
                    && !sendButton->isEnabled()
                    && AgentWorkbenchWidgetTestAccess::pendingPrompt(workbench)
                        == QStringLiteral("queued-without-durable-acknowledgement"),
                "missing durable acknowledgement capability did not gate every new-Turn path")) {
        return 1;
    }
    if (!expect(AgentWorkbenchWidgetTestAccess::activeTurnSubmitIsInert(
                    workbench, operationComposer, sendButton),
                "missing durable acknowledgement capability hid the active Turn Stop action")) {
        return 1;
    }
    AgentWorkbenchWidgetTestAccess::setPendingPrompt(workbench, QString());
    AgentWorkbenchWidgetTestAccess::setMutationAcknowledgementAvailable(
        workbench, true);
    if (!expect(sendButton->text() == QStringLiteral("发送")
                    && sendButton->isEnabled(),
                "negotiated durable acknowledgement capability did not restore normal Send")) {
        return 1;
    }
    if (!expect(AgentWorkbenchWidgetTestAccess::activeTurnSubmitIsInert(
                    workbench, operationComposer, sendButton),
                "active-turn Ctrl+Enter path could start a second turn or hide Stop")) {
        return 1;
    }
    if (!verifyRuntimeDegradationFailures(
            application, workbench, runtimeClient, runtimeCapability)) {
        return 1;
    }
    runtimeClient->modelProfilesListed(QStringLiteral("profile-list-fixture"),
                                       modelProfileFixture);
    application.processEvents();
    if (!expect(modelPicker->toolTip().contains(QStringLiteral("Profile 元数据只读"))
                    && modelPicker->toolTip().contains(QStringLiteral("1 个")),
                "model profile metadata did not remain an explicit read-only projection")) {
        return 1;
    }
    AgentWorkbenchWidgetTestAccess::prepareRuntimeDegradationRequest(
        workbench, QStringLiteral("degradation-invalid"));
    runtimeClient->runtimeDegradationsRead(QStringLiteral("degradation-invalid"), QJsonObject{
        {QStringLiteral("schema_version"), QStringLiteral("runtime-degradations/unknown")},
    });
    application.processEvents();
    if (!expect(runtimeCapability->text() == QStringLiteral("能力未知")
                    && runtimeCapability->toolTip().contains(QStringLiteral("只读门控")),
                "invalid runtime degradation schema did not fail closed")) {
        return 1;
    }
    AgentWorkbenchWidgetTestAccess::prepareRuntimeDegradationRequest(
        workbench, QStringLiteral("degradation-restored-before-recovery"));
    runtimeClient->runtimeDegradationsRead(
        QStringLiteral("degradation-restored-before-recovery"),
        validCodexRuntimeDegradationSnapshot());
    application.processEvents();
    AgentWorkbenchWidgetTestAccess::prepareTimelineSync(
        workbench, QStringLiteral("session-recovery-render"),
        QStringLiteral("render-sync-session-recovery-render"));
    runtimeClient->sessionStarted(QStringLiteral("recovery-render-session"), QJsonObject{
        {QStringLiteral("id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("mode"), QStringLiteral("chat")},
        {QStringLiteral("title"), QStringLiteral("Recovery render")},
        {QStringLiteral("context_threshold"), QJsonObject{
            {QStringLiteral("schema_version"),
             QStringLiteral("session-context-threshold/0.1")},
            {QStringLiteral("status"), QStringLiteral("no_action")},
            {QStringLiteral("source"), QStringLiteral("runtime-authoritative")},
            {QStringLiteral("history_state"), QStringLiteral("empty")},
            {QStringLiteral("automatic_compaction_authority"), false},
        }},
        {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("provider"), QStringLiteral("fixture")},
            {QStringLiteral("model"), QStringLiteral("fixture")},
            {QStringLiteral("adapter"), QStringLiteral("fixture")},
            {QStringLiteral("version"), QStringLiteral("1")},
            {QStringLiteral("permission_profile"), QStringLiteral("read-only")},
        }},
    });
    application.processEvents();
    if (!expect(executionContext->text().contains(QStringLiteral("阈值正常")),
                "valid no-action context threshold did not render")) {
        return 1;
    }
    runtimeClient->sessionStarted(QStringLiteral("threshold-preview-fixture"), QJsonObject{
        {QStringLiteral("id"), QStringLiteral("session-threshold-preview")},
        {QStringLiteral("mode"), QStringLiteral("chat")},
        {QStringLiteral("title"), QStringLiteral("Threshold preview")},
        {QStringLiteral("context_threshold"), QJsonObject{
            {QStringLiteral("schema_version"),
             QStringLiteral("session-context-threshold/0.1")},
            {QStringLiteral("status"), QStringLiteral("preview_required")},
            {QStringLiteral("source"), QStringLiteral("runtime-authoritative")},
            {QStringLiteral("history_state"), QStringLiteral("replayed")},
            {QStringLiteral("automatic_compaction_authority"), false},
        }},
    });
    application.processEvents();
    if (!expect(executionContext->text().contains(QStringLiteral("阈值需预检")),
                "preview-required context threshold did not render")) {
        return 1;
    }
    runtimeClient->sessionStarted(QStringLiteral("threshold-hard-fixture"), QJsonObject{
        {QStringLiteral("id"), QStringLiteral("session-threshold-hard")},
        {QStringLiteral("mode"), QStringLiteral("chat")},
        {QStringLiteral("title"), QStringLiteral("Threshold hard")},
        {QStringLiteral("context_threshold"), QJsonObject{
            {QStringLiteral("schema_version"),
             QStringLiteral("session-context-threshold/0.1")},
            {QStringLiteral("status"), QStringLiteral("hard_limit_exceeded")},
            {QStringLiteral("source"), QStringLiteral("runtime-authoritative")},
            {QStringLiteral("history_state"), QStringLiteral("active")},
            {QStringLiteral("automatic_compaction_authority"), false},
        }},
    });
    application.processEvents();
    if (!expect(executionContext->text().contains(QStringLiteral("阈值已达上限")),
                "hard-limit context threshold did not render")) {
        return 1;
    }
    runtimeClient->sessionStarted(QStringLiteral("threshold-invalid-fixture"), QJsonObject{
        {QStringLiteral("id"), QStringLiteral("session-threshold-invalid")},
        {QStringLiteral("mode"), QStringLiteral("chat")},
        {QStringLiteral("title"), QStringLiteral("Threshold invalid")},
        {QStringLiteral("context_threshold"), QJsonObject{
            {QStringLiteral("schema_version"),
             QStringLiteral("session-context-threshold/0.1")},
            {QStringLiteral("status"), QStringLiteral("no_action")},
            {QStringLiteral("source"), QStringLiteral("runtime-authoritative")},
            {QStringLiteral("history_state"), QStringLiteral("active")},
            {QStringLiteral("automatic_compaction_authority"), true},
        }},
    });
    application.processEvents();
    if (!expect(executionContext->text().contains(QStringLiteral("阈值未知"))
                    && !executionContext->text().contains(QStringLiteral("阈值正常")),
                "automatic compaction authority did not fail closed to unknown")) {
        return 1;
    }
    runtimeClient->sessionStarted(QStringLiteral("recovery-render-session-restored"), QJsonObject{
        {QStringLiteral("id"), QStringLiteral("session-recovery-render")},
        {QStringLiteral("mode"), QStringLiteral("chat")},
        {QStringLiteral("title"), QStringLiteral("Recovery render")},
        {QStringLiteral("context_threshold"), QJsonObject{
            {QStringLiteral("schema_version"),
             QStringLiteral("session-context-threshold/0.1")},
            {QStringLiteral("status"), QStringLiteral("no_action")},
            {QStringLiteral("source"), QStringLiteral("runtime-authoritative")},
            {QStringLiteral("history_state"), QStringLiteral("active")},
            {QStringLiteral("automatic_compaction_authority"), false},
        }},
    });
    application.processEvents();
    const QString recoveryTimelineSync =
        AgentWorkbenchWidgetTestAccess::timelineSyncRequestId(
            workbench, QStringLiteral("session-recovery-render"));
    const QJsonObject emptyTimelineAnchor{
        {QStringLiteral("sequence"), 0},
        {QStringLiteral("event_id"), QJsonValue::Null},
    };
    if (!expect(!recoveryTimelineSync.isEmpty(),
                "restored Session did not request its initial Timeline sync")) {
        return 1;
    }
    runtimeClient->timelineSynced(
        recoveryTimelineSync,
        timelineSyncPage(QStringLiteral("session-recovery-render"),
                         emptyTimelineAnchor, emptyTimelineAnchor, {}, true));
    application.processEvents();
    if (!verifyBoundedContextThresholdCache(
            workbench, QStringLiteral("session-recovery-render"))) {
        return 1;
    }
    if (!expect(waitUntil(application, [&workbench]() {
                    return AgentWorkbenchWidgetTestAccess::sessionListRefreshIdle(workbench);
                }),
                "synthetic session fixture did not drain real session-list refreshes")) {
        return 1;
    }
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
                qPrintable(QStringLiteral(
                    "session quarantine did not disable the active composer: banner=%1 send=%2 enabled=%3")
                    .arg(recoveryBanner->text(), sendButton->text(),
                         sendButton->isEnabled() ? QStringLiteral("true")
                                                 : QStringLiteral("false"))))) {
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
    runtimeClient->requestFailedExact({}, QStringLiteral("runtime/restart"),
                                      QStringLiteral("Codex App Server restart failed: opaque provider payload"),
                                      QStringLiteral("-32110"));
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
    AgentWorkbenchWidgetTestAccess::prepareRuntimeDegradationRequest(
        workbench, QStringLiteral("post-health-render-fixture"));
    runtimeClient->runtimeDegradationsRead(
        QStringLiteral("post-health-render-fixture"),
        validCodexRuntimeDegradationSnapshot());
    application.processEvents();

    const QJsonObject commandBase{
        {QStringLiteral("id"), QStringLiteral("command-render-fixture")},
        {QStringLiteral("kind"), QStringLiteral("command")},
        {QStringLiteral("role"), QStringLiteral("tool")},
        {QStringLiteral("content"), QStringLiteral("$ printf '<unsafe>'\n")},
    };
    const QString timelineSessionId = QStringLiteral("session-render-fixture");
    const QString timelineTurnId = QStringLiteral("turn-render-fixture");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, timelineSessionId);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), timelineSessionId, timelineTurnId));
    QJsonObject startedCommand = commandBase;
    startedCommand.insert(QStringLiteral("state"), QStringLiteral("started"));
    startedCommand.insert(QStringLiteral("data"), QJsonObject{
        {QStringLiteral("session_id"), timelineSessionId},
        {QStringLiteral("risk"), QJsonObject{{QStringLiteral("level"), QStringLiteral("low")}}},
        {QStringLiteral("output"), QJsonObject{{QStringLiteral("artifact"), QJsonValue::Null}}},
    });
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.started"), timelineSessionId, timelineTurnId,
        startedCommand));
    QJsonObject completedCommand = startedCommand;
    completedCommand.insert(QStringLiteral("state"), QStringLiteral("completed"));
    completedCommand.insert(QStringLiteral("data"), QJsonObject{
        {QStringLiteral("session_id"), timelineSessionId},
        {QStringLiteral("risk"), QJsonObject{{QStringLiteral("level"), QStringLiteral("low")}}},
        {QStringLiteral("output"), QJsonObject{
            {QStringLiteral("artifact"), QJsonObject{
                {QStringLiteral("reference"), QStringLiteral(
                    "command-output:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")}
            }}
        }},
    });
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("item.completed"), timelineSessionId, timelineTurnId,
        completedCommand));
    QPushButton *commandArtifactButton = workbench.findChild<QPushButton *>(
        QStringLiteral("timelineCommandArtifactButton"));
    QLabel *commandContent = workbench.findChild<QLabel *>(
        QStringLiteral("timelineCommandContent"));
    if (!expect(commandArtifactButton && !commandArtifactButton->isHidden()
                    && commandArtifactButton->property("artifactReference").toString()
                        .startsWith(QStringLiteral("command-output:sha256:"))
                    && commandArtifactButton->property("artifactItemId").toString()
                        == QStringLiteral("command-render-fixture")
                    && commandContent && commandContent->textFormat() == Qt::PlainText
                    && commandContent->text().contains(QStringLiteral("<unsafe>")),
                "structured command timeline did not expose a plain-text artifact action")) {
        return 1;
    }

    const QJsonObject usageAuthority{
        {QStringLiteral("schema_version"), QStringLiteral("usage-authority/0.1")},
        {QStringLiteral("entries"), QJsonArray{
            QJsonObject{
                {QStringLiteral("metric"), QStringLiteral("token")},
                {QStringLiteral("authority"), QStringLiteral("observed")},
                {QStringLiteral("authoritative"), true},
                {QStringLiteral("value"), QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("token")},
                    {QStringLiteral("input_tokens"), 2},
                    {QStringLiteral("output_tokens"), 3},
                    {QStringLiteral("total_tokens"), 5},
                }},
            },
            QJsonObject{
                {QStringLiteral("metric"), QStringLiteral("context")},
                {QStringLiteral("authority"), QStringLiteral("observed")},
                {QStringLiteral("authoritative"), true},
                {QStringLiteral("value"), QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("context")},
                    {QStringLiteral("used_tokens"), 2},
                    {QStringLiteral("window_tokens"), 128000},
                }},
            },
            QJsonObject{
                {QStringLiteral("metric"), QStringLiteral("cost")},
                {QStringLiteral("authority"), QStringLiteral("unknown")},
                {QStringLiteral("authoritative"), false},
                {QStringLiteral("value"), QJsonValue::Null},
            },
            QJsonObject{
                {QStringLiteral("metric"), QStringLiteral("reasoning")},
                {QStringLiteral("authority"), QStringLiteral("observed")},
                {QStringLiteral("authoritative"), true},
                {QStringLiteral("value"), QJsonObject{
                    {QStringLiteral("kind"), QStringLiteral("reasoning")},
                    {QStringLiteral("output_tokens"), 0},
                }},
            },
        }},
        {QStringLiteral("compaction_threshold"), QJsonObject{
            {QStringLiteral("schema_version"), QStringLiteral("context-threshold/0.1")},
            {QStringLiteral("status"), QStringLiteral("no_action")},
            {QStringLiteral("automatic_compaction_authority"), false},
        }},
    };
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("usage.updated"), timelineSessionId, timelineTurnId,
        QJsonObject{
            {QStringLiteral("id"), QStringLiteral("usage-render-fixture")},
            {QStringLiteral("kind"), QStringLiteral("usage")},
            {QStringLiteral("role"), QStringLiteral("system")},
            {QStringLiteral("state"), QStringLiteral("updated")},
            {QStringLiteral("content"), QStringLiteral("Token usage updated")},
            {QStringLiteral("data"), QJsonObject{{QStringLiteral("authority"), usageAuthority}}},
        }));
    application.processEvents();
    QLabel *usagePresentation = workbench.findChild<QLabel *>(
        QStringLiteral("timelineUsageAuthority"));
    if (!expect(usagePresentation && usagePresentation->text().contains(QStringLiteral("输入 2"))
                    && usagePresentation->text().contains(QStringLiteral("上下文正常"))
                    && usagePresentation->toolTip().contains(QStringLiteral("不包含提示词")),
                "usage authority was not rendered as bounded read-only metadata")) {
        return 1;
    }
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("usage.updated"), timelineSessionId, timelineTurnId,
        QJsonObject{
            {QStringLiteral("id"), QStringLiteral("usage-invalid-render-fixture")},
            {QStringLiteral("kind"), QStringLiteral("usage")},
            {QStringLiteral("role"), QStringLiteral("system")},
            {QStringLiteral("state"), QStringLiteral("updated")},
            {QStringLiteral("content"), QStringLiteral("Token usage updated")},
            {QStringLiteral("data"), QJsonObject{{QStringLiteral("authority"), QJsonObject{
                {QStringLiteral("schema_version"), QStringLiteral("usage-authority/9.9")},
            }}}},
        }));
    application.processEvents();
    const QList<QLabel *> usagePresentations = workbench.findChildren<QLabel *>(
        QStringLiteral("timelineUsageAuthority"));
    const bool unknownUsageVisible = std::any_of(
        usagePresentations.cbegin(), usagePresentations.cend(), [](const QLabel *label) {
            return label->text() == QStringLiteral("用量来源未知");
        });
    if (!expect(usagePresentations.size() >= 2 && unknownUsageVisible,
                "malformed usage authority did not fail closed in Qt")) {
        return 1;
    }
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), timelineSessionId, timelineTurnId));

    const QString cancelSessionId = QStringLiteral("session-cancel-fixture");
    const QString cancelTurnId = QStringLiteral("turn-cancel-fixture");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench, cancelSessionId);
    if (!expect(waitUntil(application, [&workbench]() {
                    return AgentWorkbenchWidgetTestAccess::operationStatusRequestIdle(workbench);
                }),
                "synthetic cancellation fixture did not drain operation status request")) {
        return 1;
    }
    AgentWorkbenchWidgetTestAccess::prepareOperationStatusSession(workbench,
                                                                  cancelSessionId);
    runtimeClient->operationStatusRead({}, QJsonObject{
        {QStringLiteral("schema_version"),
         QStringLiteral("operation-reconciliation-status/0.1")},
        {QStringLiteral("session_id"), cancelSessionId},
        {QStringLiteral("blocked"), false},
        {QStringLiteral("operation"), QJsonValue::Null},
        {QStringLiteral("recovery_action_available"), false},
    });
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::operationStatusAllowsSession(
                    workbench, cancelSessionId),
                "synthetic cancellation fixture did not establish an unblocked operation state")) {
        return 1;
    }
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), cancelSessionId, cancelTurnId));
    application.processEvents();
    if (!expect(sendButton->text() == QStringLiteral("停止") && sendButton->isEnabled()
                    && sendButton->width() == 84,
                "running turn did not expose a stable stop action")) {
        return 1;
    }
    QLabel *heartbeatRuntimeStatus = workbench.findChild<QLabel *>(
        QStringLiteral("agentRuntimeStatus"));
    runtimeClient->runtimeLivenessChanged(
        false, QStringLiteral("运行时心跳超时，连接状态未知"));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && !AgentWorkbenchWidgetTestAccess::turnCancelling(workbench)
                    && sendButton->text() == QStringLiteral("停止")
                    && sendButton->isEnabled()
                    && heartbeatRuntimeStatus
                    && heartbeatRuntimeStatus->text()
                        == QStringLiteral("◇ 运行时状态未知"),
                "heartbeat Unknown cleared the active Turn or disabled out-of-band Stop")) {
        return 1;
    }
    runtimeClient->runtimeLivenessChanged(true, QStringLiteral("运行时响应正常"));
    application.processEvents();
    if (!expect(AgentWorkbenchWidgetTestAccess::turnRunning(workbench)
                    && sendButton->text() == QStringLiteral("停止")
                    && sendButton->isEnabled()
                    && heartbeatRuntimeStatus
                    && heartbeatRuntimeStatus->text()
                        == QStringLiteral("● 运行时就绪"),
                "heartbeat recovery did not preserve the active Turn and restore status")) {
        return 1;
    }
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.cancellation-acknowledged"), cancelSessionId,
        cancelTurnId));
    application.processEvents();
    if (!expect(sendButton->text() == QStringLiteral("正在停止") && !sendButton->isEnabled(),
                "accepted cancellation was incorrectly presented as a terminal state")) {
        return 1;
    }
    const int statusNoticeCountBeforeInterrupt = workbench.findChildren<QLabel *>(
        QStringLiteral("agentStatusNotice")).size();
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.interrupted"), cancelSessionId, cancelTurnId));
    application.processEvents();
    const QList<QLabel *> interruptionNotices = workbench.findChildren<QLabel *>(
        QStringLiteral("agentStatusNotice"));
    const bool interruptionVisible = interruptionNotices.size()
            == statusNoticeCountBeforeInterrupt + 1
        && interruptionNotices.constLast()->text() == QStringLiteral("任务已停止。")
        && interruptionNotices.constLast()->property("severity").toString()
            == QStringLiteral("status");
    if (!expect(sendButton->text() == QStringLiteral("发送") && sendButton->isEnabled()
                    && interruptionVisible,
                qPrintable(QStringLiteral(
                    "interrupted turn did not expose its terminal state or restore the composer: send=%1 enabled=%2")
                    .arg(sendButton->text(), sendButton->isEnabled()
                            ? QStringLiteral("true") : QStringLiteral("false"))))) {
        return 1;
    }
    const QString failedTurnId = QStringLiteral("turn-failed-fixture");
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), cancelSessionId, failedTurnId));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.failed"), cancelSessionId, failedTurnId,
        QJsonObject{
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
        }));
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
    const QString errorClassSessionId = QStringLiteral("session-error-class-fixture");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench,
                                                          errorClassSessionId);
    for (int index = 0; index < stableErrorClasses.size(); ++index) {
        const auto &errorClass = stableErrorClasses.at(index);
        const QString errorTurnId = QStringLiteral("turn-error-class-%1").arg(index);
        runtimeClient->timelineEvent(timelineEnvelope(
            QStringLiteral("turn.started"), errorClassSessionId, errorTurnId));
        runtimeClient->timelineEvent(timelineEnvelope(
            QStringLiteral("turn.failed"), errorClassSessionId, errorTurnId,
            QJsonObject{
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
            }));
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
    runtimeClient->requestFailedExact({}, QStringLiteral("session/archive"),
                                      rawProviderFailure, QStringLiteral("-32143"));
    runtimeClient->requestFailedExact({}, QStringLiteral("session/unarchive"),
                                      QStringLiteral("Codex provider state is not loaded; opaque detail"),
                                      QStringLiteral("-32141"));
    runtimeClient->requestFailedExact({}, QStringLiteral("session/fork"),
                                      QStringLiteral("Codex provider archive was acknowledged but local persistence failed and compensation also failed"),
                                      QStringLiteral("-32145"));
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
    const QString diagnosticSessionId = QStringLiteral("session-command-diagnostic");
    const QString diagnosticTurnId = QStringLiteral("turn-command-diagnostic");
    AgentWorkbenchWidgetTestAccess::setCurrentChatSession(workbench,
                                                          diagnosticSessionId);
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.started"), diagnosticSessionId, diagnosticTurnId));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("diagnostics.observed"), diagnosticSessionId,
        diagnosticTurnId, QJsonObject{
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
        }));
    runtimeClient->timelineEvent(timelineEnvelope(
        QStringLiteral("turn.completed"), diagnosticSessionId,
        diagnosticTurnId));
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
    gitRefresh->click();
    if (!expect(waitUntil(application, [gitSummary, &fixtureBranch]() {
                    return gitSummary->text().contains(fixtureBranch);
                })
                    && !executionContext->text().contains(
                        QStringLiteral("分支 %1").arg(fixtureBranch)),
                "live Git observation overwrote the active Session Workspace binding")) {
        return 1;
    }
    if (!expect(retentionSettings->isEnabled(),
                "project retention settings did not enable for an opened project")) {
        return 1;
    }
    // Synthetic Timeline fixtures above bypass the sidecar and use their own
    // sequence space. Start the real sidecar workflow with a clean client view.
    AgentWorkbenchWidgetTestAccess::resetTimelineValidation(workbench);
    AgentWorkbenchWidgetTestAccess::setTimelineSyncAvailable(workbench, true);
    AgentWorkbenchWidgetTestAccess::setTimelineSubscriptionAvailable(workbench, true);
    int realTimelineSubscribed = 0;
    int realTimelineSynced = 0;
    int realTimelineActivated = 0;
    QStringList realTimelineFailures;
    QObject::connect(runtime, &AgentRuntimeClient::timelineSubscribed,
                     &workbench, [&realTimelineSubscribed](const QString &,
                                                          const QJsonObject &) {
        ++realTimelineSubscribed;
    });
    QObject::connect(runtime, &AgentRuntimeClient::timelineSubscriptionSynced,
                     &workbench, [&realTimelineSynced](const QString &,
                                                      const QJsonObject &) {
        ++realTimelineSynced;
    });
    QObject::connect(runtime, &AgentRuntimeClient::timelineSubscriptionActivated,
                     &workbench, [&realTimelineActivated](const QString &,
                                                         const QJsonObject &) {
        ++realTimelineActivated;
    });
    QObject::connect(runtime, &AgentRuntimeClient::timelineSubscriptionFailed,
                     &workbench, [&realTimelineFailures](const QString &requestId,
                                                        const QJsonObject &failure) {
        realTimelineFailures.append(QStringLiteral("%1:%2:%3")
            .arg(requestId,
                 failure.value(QStringLiteral("stage")).toString(),
                 failure.value(QStringLiteral("reason")).toString()));
    });
    runtime->startSession(QStringLiteral("work"), openedProjectId);
    if (!expect(waitUntil(application, [&previewSessionId]() {
                    return !previewSessionId.isEmpty();
                }),
                "workspace edit preview fixture did not create a Work session")) {
        return 1;
    }
    if (!expect(waitUntil(application, [&workbench, &previewSessionId,
                                         &realTimelineSubscribed, &realTimelineSynced,
                                         &realTimelineActivated,
                                         &realTimelineFailures]() {
                    return AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                               workbench, previewSessionId) == QStringLiteral("live")
                        && realTimelineSubscribed == 1
                        && realTimelineSynced == 1
                        && realTimelineActivated == 1
                        && realTimelineFailures.isEmpty();
                }),
                "new Work session did not complete its initial Timeline subscription")) {
        return 1;
    }
    if (!expect(AgentWorkbenchWidgetTestAccess::activateMode(
                    workbench, QStringLiteral("work")),
                "workspace fixture could not activate its new Work session")) {
        return 1;
    }
    if (!expect(waitUntil(application, [&workbench, &previewSessionId]() {
                    return AgentWorkbenchWidgetTestAccess::operationStatusRequestIdle(workbench)
                        && AgentWorkbenchWidgetTestAccess::operationStatusAllowsSession(
                            workbench, previewSessionId);
                }),
                "new Work session did not complete operation-status reconciliation")) {
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
                qPrintable(QStringLiteral(
                    "real Git worktree diff did not enable persistent pinning: %1")
                    .arg(AgentWorkbenchWidgetTestAccess::gitPinGateState(workbench))))) {
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
    const bool deletionUndoRestored = waitUntil(
        application, [sessionList, send, &deletionReceipt]() {
                    return deletionReceipt.value(QStringLiteral("state")).toString()
                               == QStringLiteral("cancelled")
                        && sessionList->count() > 0
                        && !sessionList->item(0)->text().contains(QStringLiteral("待删除"))
                        && send->text() == QStringLiteral("发送")
                        && send->isEnabled();
                });
    if (!expect(deletionUndoRestored,
                qPrintable(QStringLiteral(
                    "session deletion undo did not restore the Qt workflow: state=%1 send=%2 enabled=%3")
                    .arg(deletionReceipt.value(QStringLiteral("state")).toString(),
                         send->text(), send->isEnabled() ? QStringLiteral("true")
                                                        : QStringLiteral("false"))))) {
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
    if (!expect(waitUntil(application, [executionContext, modelPicker, sendButton,
                                         &fixtureBranch]() {
                    return executionContext->text().contains(
                               QStringLiteral("Runtime preview"))
                        && executionContext->text().contains(
                            QStringLiteral("模型 deterministic-echo"))
                        && executionContext->text().contains(QStringLiteral("权限 只读"))
                        && executionContext->text().contains(
                            QStringLiteral("工作区 root-1"))
                        && executionContext->text().contains(
                            QStringLiteral("分支 %1").arg(fixtureBranch))
                        && modelPicker->currentText()
                            == QStringLiteral("local / deterministic-echo")
                        && modelPicker->toolTip().contains(QStringLiteral("preview"))
                        && modelPicker->toolTip().contains(QStringLiteral("read-only"))
                        && sendButton->isEnabled();
                }),
                "active Work session did not finish Timeline recovery and project its persisted Runtime binding")) {
        return 1;
    }
    composer->setPlainText(QStringLiteral("检查选定上下文"));
    QStringList turnRequestFailures;
    int subscriptionEventsAfterSend = 0;
    int bareEventsAfterSend = 0;
    QObject::connect(runtime, &AgentRuntimeClient::requestFailed, &workbench,
                     [&turnRequestFailures](const QString &, const QString &method,
                                            const QString &, int code) {
        if (method == QStringLiteral("turn/start")) {
            turnRequestFailures.append(QStringLiteral("%1:%2").arg(method).arg(code));
        }
    });
    QObject::connect(runtime, &AgentRuntimeClient::timelineSubscriptionEvent,
                     &workbench, [&subscriptionEventsAfterSend](const QJsonObject &) {
        ++subscriptionEventsAfterSend;
    });
    QObject::connect(runtime, &AgentRuntimeClient::timelineEvent,
                     &workbench, [&bareEventsAfterSend](const QJsonObject &) {
        ++bareEventsAfterSend;
    });
    sendButton->click();
    const bool structuredContextRendered = waitUntil(
        application, [&workbench, contextPanel, contextList]() {
                    const QList<QLabel *> labels = workbench.findChildren<QLabel *>();
                    return contextPanel->isHidden() && contextList->count() == 0
                        && std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
                            return label->text().contains(QStringLiteral("untrusted data"))
                                && label->text().contains(QStringLiteral("original"));
                        });
                });
    if (!structuredContextRendered) {
        qCritical() << "structured context diagnostics"
                    << "session" << previewSessionId
                    << "recovery" << AgentWorkbenchWidgetTestAccess::timelineRecoveryState(
                           workbench, previewSessionId)
                    << "contextHidden" << contextPanel->isHidden()
                    << "contextCount" << contextList->count()
                    << "send" << sendButton->text() << sendButton->isEnabled()
                    << "turnFailures" << turnRequestFailures
                    << "subscribed" << realTimelineSubscribed
                    << "synced" << realTimelineSynced
                    << "activated" << realTimelineActivated
                    << "subscriptionFailures" << realTimelineFailures
                    << "subscriptionEvents" << subscriptionEventsAfterSend
                    << "bareEvents" << bareEventsAfterSend;
    }
    if (!expect(structuredContextRendered,
                "structured context was not sent through AAP with authoritative file data")) {
        return 1;
    }
    const QByteArray artifactContent = QByteArray(65534, 'a')
        + QStringLiteral("中tail\n").toUtf8();
    const QString artifactItemId = QStringLiteral("command.\"slash/\\:render");
    const auto artifactPageResponse = [&](const QString &sessionId,
                                          const QByteArray &source,
                                          qsizetype offset) {
        const QString sha256 = QString::fromLatin1(QCryptographicHash::hash(
            source, QCryptographicHash::Sha256).toHex());
        const QString reference = QStringLiteral("command-output:sha256:") + sha256;
        const QString mediaType = QStringLiteral("text/plain; charset=utf-8");
        constexpr qsizetype pageSize = 64 * 1024;
        qsizetype end = qMin(source.size(), offset + pageSize);
        while (end > offset
                && QString::fromUtf8(source.first(end)).toUtf8() != source.first(end)) {
            --end;
        }
        const QByteArray inlineBytes = source.mid(offset, end - offset);
        qsizetype previewEnd = qMin(source.size(), pageSize);
        while (previewEnd > 0
                && QString::fromUtf8(source.first(previewEnd)).toUtf8()
                    != source.first(previewEnd)) {
            --previewEnd;
        }
        QJsonObject limits{
            {QStringLiteral("schema_version"),
             QStringLiteral("content-inline-limits/0.1")},
            {QStringLiteral("max_item_bytes"), pageSize},
            {QStringLiteral("max_total_bytes"), pageSize},
        };
        limits.insert(QStringLiteral("identity"),
                      AgentRuntimeClient::contentInlineLimitsIdentity(limits));
        QJsonObject preview{
            {QStringLiteral("schema_version"), QStringLiteral("content-preview/0.1")},
            {QStringLiteral("reference"), reference},
            {QStringLiteral("sha256"), sha256},
            {QStringLiteral("media_type"), mediaType},
            {QStringLiteral("content_bytes"), source.size()},
            {QStringLiteral("preview_bytes"), previewEnd},
            {QStringLiteral("truncated"), previewEnd < source.size()},
            {QStringLiteral("line_count"), source.isEmpty() ? 0 : 1},
            {QStringLiteral("width"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("height"), QJsonValue(QJsonValue::Null)},
        };
        preview.insert(QStringLiteral("identity"),
                       AgentRuntimeClient::contentPreviewIdentity(preview));
        const QJsonObject content{
            {QStringLiteral("schema_version"), QStringLiteral("content-reference/0.1")},
            {QStringLiteral("reference"), reference},
            {QStringLiteral("sha256"), sha256},
            {QStringLiteral("bytes"), source.size()},
            {QStringLiteral("media_type"), mediaType},
            {QStringLiteral("preview"), preview},
        };
        QJsonObject response{
            {QStringLiteral("schema_version"),
             QStringLiteral("command-output-artifact-page/0.1")},
            {QStringLiteral("session_id"), sessionId},
            {QStringLiteral("item_id"), artifactItemId},
            {QStringLiteral("created_at_ms"), 1'700'000'000'123.0},
            {QStringLiteral("content_reference"), content},
            {QStringLiteral("source_bytes"), source.size()},
            {QStringLiteral("redacted_count"), 0},
            {QStringLiteral("redacted"), false},
            {QStringLiteral("total_bytes"), source.size()},
            {QStringLiteral("retained_bytes"), source.size()},
            {QStringLiteral("omitted_bytes"), 0},
            {QStringLiteral("truncated"), false},
            {QStringLiteral("read_only"), true},
        };
        response.insert(QStringLiteral("binding_identity"),
            AgentRuntimeClient::commandArtifactPageBindingIdentity(response));
        const QString binding = response.value(
            QStringLiteral("binding_identity")).toString();
        QJsonValue nextCursor(QJsonValue::Null);
        if (end < source.size()) {
            QJsonObject cursor{
                {QStringLiteral("schema_version"),
                 QStringLiteral("content-reference-cursor/0.1")},
                {QStringLiteral("reference"), reference},
                {QStringLiteral("sha256"), sha256},
                {QStringLiteral("bytes"), source.size()},
                {QStringLiteral("media_type"), mediaType},
                {QStringLiteral("offset"), end},
                {QStringLiteral("page_size"), pageSize},
                {QStringLiteral("limits"), limits},
                {QStringLiteral("binding_identity"), binding},
            };
            cursor.insert(QStringLiteral("identity"),
                AgentRuntimeClient::contentReferenceCursorIdentity(cursor));
            nextCursor = cursor;
        }
        QJsonObject page{
            {QStringLiteral("schema_version"), QStringLiteral("content-reference-page/0.1")},
            {QStringLiteral("reference"), reference},
            {QStringLiteral("sha256"), sha256},
            {QStringLiteral("bytes"), source.size()},
            {QStringLiteral("media_type"), mediaType},
            {QStringLiteral("offset"), offset},
            {QStringLiteral("page_size"), pageSize},
            {QStringLiteral("page_bytes"), inlineBytes.size()},
            {QStringLiteral("inline"), QString::fromUtf8(inlineBytes)},
            {QStringLiteral("inline_truncated"), false},
            {QStringLiteral("limits"), limits},
            {QStringLiteral("binding_identity"), binding},
            {QStringLiteral("next_cursor"), nextCursor},
        };
        page.insert(QStringLiteral("identity"),
                    AgentRuntimeClient::contentReferencePageIdentity(page));
        response.insert(QStringLiteral("page"), page);
        return response;
    };
    const QString artifactSha = QString::fromLatin1(QCryptographicHash::hash(
        artifactContent, QCryptographicHash::Sha256).toHex());
    const QString artifactReference = QStringLiteral("command-output:sha256:")
        + artifactSha;
    const QJsonObject artifactFirst = artifactPageResponse(
        previewSessionId, artifactContent, 0);
    AgentWorkbenchWidgetTestAccess::prepareCommandArtifactWorkflow(
        workbench, QStringLiteral("artifact-first-page"), previewSessionId,
        artifactItemId, artifactReference);
    AgentWorkbenchWidgetTestAccess::deliverCommandArtifactPage(
        workbench, QStringLiteral("artifact-first-page"), artifactFirst);
    auto *artifactPin = workbench.findChild<QPushButton *>(
        QStringLiteral("commandArtifactPinButton"));
    auto *artifactLoadMore = workbench.findChild<QPushButton *>(
        QStringLiteral("commandArtifactLoadMoreButton"));
    auto *artifactPreview = workbench.findChild<QPlainTextEdit *>(
        QStringLiteral("commandArtifactPreview"));
    const QJsonObject artifactCursor = artifactFirst.value(QStringLiteral("page"))
        .toObject().value(QStringLiteral("next_cursor")).toObject();
    if (!expect(artifactPin && artifactLoadMore && artifactPreview
                    && !artifactPin->isEnabled() && artifactLoadMore->isEnabled()
                    && !artifactCursor.isEmpty()
                    && artifactPreview->toPlainText().toUtf8()
                        == artifactContent.first(
                            artifactCursor.value(QStringLiteral("offset")).toInt()),
                "partial command Artifact page did not keep Pin disabled and Load More enabled")) {
        return 1;
    }
    AgentWorkbenchWidgetTestAccess::prepareCommandArtifactContinuation(
        workbench, QStringLiteral("artifact-last-page"), artifactCursor);
    AgentWorkbenchWidgetTestAccess::deliverCommandArtifactPage(
        workbench, QStringLiteral("artifact-last-page"), artifactPageResponse(
            previewSessionId, artifactContent,
            artifactCursor.value(QStringLiteral("offset")).toInt()));
    if (!expect(artifactPin->isEnabled() && !artifactLoadMore->isEnabled()
                    && artifactPin->toolTip().contains(QStringLiteral("不会自动发送"))
                    && artifactPreview->toPlainText().toUtf8() == artifactContent,
                "complete command Artifact did not pass length/SHA and enable explicit Pin")) {
        return 1;
    }
    if (QDialog *dialog = qobject_cast<QDialog *>(artifactPin->window())) dialog->close();
    application.processEvents();

    const QByteArray otherArtifact("other session artifact\n");
    const QString otherSha = QString::fromLatin1(QCryptographicHash::hash(
        otherArtifact, QCryptographicHash::Sha256).toHex());
    const QString otherReference = QStringLiteral("command-output:sha256:") + otherSha;
    AgentWorkbenchWidgetTestAccess::prepareCommandArtifactWorkflow(
        workbench, QStringLiteral("artifact-other-session"),
        QStringLiteral("session-other-render-fixture"), artifactItemId, otherReference);
    AgentWorkbenchWidgetTestAccess::deliverCommandArtifactPage(
        workbench, QStringLiteral("artifact-other-session"), artifactPageResponse(
            QStringLiteral("session-other-render-fixture"), otherArtifact, 0));
    artifactPin = workbench.findChild<QPushButton *>(
        QStringLiteral("commandArtifactPinButton"));
    if (!expect(artifactPin && !artifactPin->isEnabled()
                    && artifactPin->toolTip().contains(QStringLiteral("当前 Work 会话")),
                "cross-session command Artifact pinning did not fail closed")) {
        return 1;
    }
    if (QDialog *dialog = qobject_cast<QDialog *>(artifactPin->window())) dialog->close();
    application.processEvents();

    AgentWorkbenchWidgetTestAccess::prepareCommandArtifactWorkflow(
        workbench, QStringLiteral("artifact-late-page"), previewSessionId,
        artifactItemId, artifactReference);
    AgentWorkbenchWidgetTestAccess::invalidateCommandArtifactWorkflow(workbench);
    application.processEvents();
    AgentWorkbenchWidgetTestAccess::deliverCommandArtifactPage(
        workbench, QStringLiteral("artifact-late-page"), artifactFirst);
    const QList<QLabel *> lateArtifactStatuses = workbench.findChildren<QLabel *>(
        QStringLiteral("commandArtifactStatus"));
    if (!expect(std::none_of(lateArtifactStatuses.cbegin(),
                            lateArtifactStatuses.cend(),
                            [](QLabel *label) { return label->isVisible(); }),
                "invalidated command Artifact workflow accepted a late page")) {
        return 1;
    }
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
                }) && fileStatus
                    && fileStatus->text().contains(QStringLiteral("外部修改"))
                    && fileStatus->text().contains(QStringLiteral("重新载入"))
                    && !editorSave->isEnabled(),
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

    QPushButton *compactChat = workbench.findChild<QPushButton *>(
        QStringLiteral("agentCompactPaneChatButton"));
    QPushButton *compactProject = workbench.findChild<QPushButton *>(
        QStringLiteral("agentCompactPaneProjectButton"));
    QPushButton *compactCanvas = workbench.findChild<QPushButton *>(
        QStringLiteral("agentCompactPaneCanvasButton"));
    QWidget *compactBar = workbench.findChild<QWidget *>(QStringLiteral("agentCompactPaneBar"));
    workbench.resize(820, 620);
    application.processEvents();
    if (!expect(compactBar && compactBar->isVisible() && compactChat && compactProject
                    && compactCanvas && !splitter->widget(0)->isVisible()
                    && splitter->widget(1)->isVisible() && !splitter->widget(2)->isVisible(),
                "narrow workbench did not switch to the compact chat pane")) {
        return 1;
    }
    compactProject->click();
    application.processEvents();
    if (!expect(splitter->widget(0)->isVisible() && !splitter->widget(1)->isVisible()
                    && !splitter->widget(2)->isVisible(),
                "compact project pane did not replace the chat pane")) {
        return 1;
    }
    compactCanvas->click();
    application.processEvents();
    if (!expect(!splitter->widget(0)->isVisible() && !splitter->widget(1)->isVisible()
                    && splitter->widget(2)->isVisible(),
                "compact workspace pane did not replace the project pane")) {
        return 1;
    }
    compactChat->click();
    application.processEvents();
    workbench.resize(1100, 700);
    application.processEvents();
    if (!expect(!compactBar->isVisible()
                    && splitter->widget(0)->width() >= splitter->widget(0)->minimumWidth()
                    && splitter->widget(1)->width() >= splitter->widget(1)->minimumWidth()
                    && splitter->widget(2)->width() >= splitter->widget(2)->minimumWidth(),
                "wide workbench did not restore all primary panes")) {
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

    const int failureNoticeCountBeforeOffline = workbench.findChildren<QLabel *>(
        QStringLiteral("agentFailureNotice")).size();
    runtimeClient->connectionStateChanged(
        false, QStringLiteral("运行时连接已断开（可见状态测试）"));
    application.processEvents();
    const QList<QLabel *> offlineNotices = workbench.findChildren<QLabel *>(
        QStringLiteral("agentFailureNotice"));
    const bool offlineNoticeVisible = offlineNotices.size()
            == failureNoticeCountBeforeOffline + 1
        && offlineNotices.constLast()->text()
            == QStringLiteral("运行时连接已断开（可见状态测试）")
        && offlineNotices.constLast()->property("severity").toString()
            == QStringLiteral("failure");
    if (!expect(runtimeStatus && runtimeStatus->text() == QStringLiteral("○ 运行时离线")
                    && offlineNoticeVisible && !sendButton->isEnabled(),
                "runtime disconnect did not expose a fail-closed offline state")) {
        return 1;
    }
    runtimeClient->connectionStateChanged(true, QStringLiteral("运行时响应正常"));
    application.processEvents();
    if (!expect(runtimeStatus->text() == QStringLiteral("● 运行时就绪"),
                "runtime reconnect did not clear the visible offline status")) {
        return 1;
    }
    return 0;
}
