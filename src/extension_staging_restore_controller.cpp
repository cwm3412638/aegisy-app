#include "extension_staging_restore_controller.h"

namespace {

const QString kErrorPrefix =
    QStringLiteral("extension-restore-controller");

QString code(const char *suffix)
{
    return kErrorPrefix + QLatin1Char('-') + QLatin1String(suffix);
}

ExtensionStagingRestoreRecordResult failure(const QString &errorCode)
{
    ExtensionStagingRestoreRecordResult result;
    result.errorCode = errorCode;
    return result;
}

} // namespace

ExtensionStagingRestoreAuditStoreResult ExtensionStagingRestoreController::inspect(
    ExtensionStagingRestoreAuditLedgerStore *store)
{
    if (!store) {
        ExtensionStagingRestoreAuditStoreResult result;
        result.state = ExtensionStagingRestoreAuditStoreState::Unavailable;
        result.errorCode = code("store-unavailable");
        return result;
    }
    return store->load();
}

ExtensionStagingRestoreRecordResult ExtensionStagingRestoreController::record(
    const ExtensionStagingRestorePrompt &prompt,
    ExtensionStagingBackupEntryVerification backupVerification,
    const ExtensionStagingRestoreApprovalAcknowledgement &acknowledgement,
    const QDateTime &decidedAt,
    ExtensionStagingRestoreAuditLedgerStore *store)
{
    if (!store) {
        return failure(code("store-unavailable"));
    }

    // 判定完全委托给审批策略：本层不重新推导任何批准维度，两份副本会各自漂移。
    ExtensionStagingRestoreRecordResult result;
    result.verdict = ExtensionStagingRestoreApprovalPolicy::evaluate(
        prompt, backupVerification, acknowledgement);

    // 区分"人做的决定"与"策略的拒绝"。批准是 Authorized；真正的人为拒绝是策略以
    // `declined` 代号拒绝【并且】提示确实可展示可批准——只有那时才有一个有效的
    // 问题被问过并被回答了"不"。其余一切拒绝（提示不可展示、验证状态不对、任何
    // 维度漂移、缺少确认，包括对一份本就不可能批准的提示说"不"）都不是决定：
    // 零写入，调用方可用账本身份前后比较验证。
    const bool humanDecline =
        result.verdict.state == ExtensionStagingRestoreApprovalState::Refused
        && result.verdict.errorCode
            == QStringLiteral("extension-restore-approval-declined")
        && prompt.state == ExtensionStagingRestorePromptState::Ready;
    const bool humanApprove =
        result.verdict.state == ExtensionStagingRestoreApprovalState::Authorized;
    if (!humanApprove && !humanDecline) {
        result.errorCode = result.verdict.errorCode;
        return result;
    }

    // 读不出的审计链阻止记录：当前内容未知时把这次决定写成历史，等于把一次篡改
    // 表述成历史。这里绝不容忍非 Ready/Empty 之外的任何状态。
    const ExtensionStagingRestoreAuditStoreResult current = store->load();
    if (current.state != ExtensionStagingRestoreAuditStoreState::Ready
            && current.state != ExtensionStagingRestoreAuditStoreState::Empty) {
        result.ledger = current;
        result.errorCode = current.errorCode.isEmpty()
            ? code("ledger-unusable")
            : current.errorCode;
        return result;
    }

    // 条目绑定的是渲染出的提示，而不是回传：审批策略已经逐项证明回传与提示逐
    // 字节相等（批准的凭据身份与这里写入的身份是同一份），而人为拒绝的记录绑
    // 定"被问过的问题"本身。警告集合沿用呈现层的固定枚举序，因此每个逻辑集合
    // 只有一个规范字节形。
    ExtensionStagingRestoreAuditEntry entry;
    entry.subject = prompt.subject;
    entry.backupId = prompt.backupId;
    entry.destinationRoot = prompt.destinationRoot;
    entry.planIdentity = prompt.echoedPlanIdentity;
    entry.treeIdentity = prompt.echoedTreeIdentity;
    entry.warnings = prompt.warnings;
    entry.decision = humanApprove
        ? ExtensionStagingRestoreAuditDecision::Approved
        : ExtensionStagingRestoreAuditDecision::Declined;
    entry.decidedAt = decidedAt;

    // 追加即"读出当前集合、末尾追加、连同读到的代号整体提交"：并发决定由比较
    // 并交换裁决。冲突以存储的独立代号透传报告（不静默重试，也不是最后写入者
    // 获胜），由调用方重新加载并重新提问。
    QList<ExtensionStagingRestoreAuditEntry> next = current.entries;
    next.append(entry);
    ExtensionStagingRestoreAuditStoreResult committed;
    QString errorCode;
    if (!store->replace(next, current.generation, &committed, &errorCode)) {
        result.ledger = current;
        result.errorCode = errorCode.isEmpty()
            ? code("store-write-failed")
            : errorCode;
        return result;
    }

    // 提交之后重新读取，而不是相信追加的结果：只有重新读到的字节才是真正生效
    // 的记录。重读失败时绝不能报告"决定已记录"。
    const ExtensionStagingRestoreAuditStoreResult refreshed = store->load();
    result.ledger = refreshed;
    if (refreshed.state != ExtensionStagingRestoreAuditStoreState::Ready) {
        result.errorCode = refreshed.errorCode.isEmpty()
            ? code("store-refresh-failed")
            : refreshed.errorCode;
        return result;
    }
    result.recorded = true;
    result.decision = entry.decision;
    return result;
}

ExtensionStagingRestoreOutcomeRecordResult
ExtensionStagingRestoreController::recordOutcome(
    const ExtensionStagingRestorePrompt &prompt,
    const ExtensionStagingRestoreExecutionResult &execution,
    const QString &preRestoreBackupId,
    const QDateTime &recordedAt,
    ExtensionStagingRestoreAuditLedgerStore *store)
{
    ExtensionStagingRestoreOutcomeRecordResult result;
    if (!store) {
        result.errorCode = code("store-unavailable");
        return result;
    }
    if (!recordedAt.isValid() || recordedAt.timeSpec() != Qt::UTC) {
        result.errorCode = code("outcome-timestamp-invalid");
        return result;
    }
    // 绑定纪律一：被批准的对象与被执行的对象必须是同一份。执行器回显它真实执行的
    // 计划身份与树身份；与提示回显不符就是落差，拒绝记录（这不是执行失败——执行的
    // 真相在另一个字段里，本层只管"这个结果能否归属到这次批准"）。
    if (prompt.echoedPlanIdentity != execution.planIdentity
            || prompt.echoedTreeIdentity != execution.treeIdentity) {
        result.errorCode = code("outcome-plan-mismatch");
        return result;
    }

    // 读不出的审计链阻止记录：当前内容未知时把执行结果写成历史，等于把一次篡改表述
    // 成历史。结果记录要求账本已存在（决定必须先于结果），因此 Empty 同样是拒绝。
    const ExtensionStagingRestoreAuditStoreResult current = store->load();
    if (current.state != ExtensionStagingRestoreAuditStoreState::Ready) {
        result.ledger = current;
        result.errorCode = current.errorCode.isEmpty()
            ? code(current.state == ExtensionStagingRestoreAuditStoreState::Empty
                   ? "outcome-without-decision" : "outcome-ledger-unusable")
            : current.errorCode;
        return result;
    }
    // 绑定纪律二：审计链里必须已存在一条携带同一计划身份与树身份的 approved 决定
    // 条目。declined 条目不携带授权，拒绝条目不存在——没有已记录的批准，执行结果
    // 就是无源事实。
    bool approvedDecisionFound = false;
    for (const ExtensionStagingRestoreAuditEntry &entry : current.entries) {
        if (entry.decision == ExtensionStagingRestoreAuditDecision::Approved
                && entry.planIdentity == execution.planIdentity
                && entry.treeIdentity == execution.treeIdentity) {
            approvedDecisionFound = true;
            break;
        }
    }
    if (!approvedDecisionFound) {
        result.ledger = current;
        result.errorCode = code("outcome-without-decision");
        return result;
    }

    // 结果条目绑定被批准的对象（字段取自提示，与决定条目逐字节同形），结果分类与
    // 计数取自执行器原文；回退指针（恢复前备份 id）随结果入链。
    ExtensionStagingRestoreOutcomeEntry outcome;
    outcome.subject = prompt.subject;
    outcome.backupId = prompt.backupId;
    outcome.destinationRoot = prompt.destinationRoot;
    outcome.planIdentity = prompt.echoedPlanIdentity;
    outcome.treeIdentity = prompt.echoedTreeIdentity;
    outcome.outcome = execution.state;
    outcome.failureIndex = execution.failureIndex;
    outcome.doneCount = execution.doneCount;
    outcome.skippedVerifiedCount = execution.skippedVerifiedCount;
    outcome.failedCount = execution.failedCount;
    outcome.preRestoreBackupId = preRestoreBackupId;
    outcome.recordedAt = recordedAt;

    // 追加即"读出当前集合、末尾追加、连同读到的代号整体提交"：并发记录由比较并
    // 交换裁决，冲突以存储的独立代号透传（不静默重试，也不是最后写入者获胜）。
    QList<ExtensionStagingRestoreOutcomeEntry> nextOutcomes = current.outcomes;
    nextOutcomes.append(outcome);
    ExtensionStagingRestoreAuditStoreResult committed;
    QString errorCode;
    if (!store->replace(current.entries, current.generation, &committed,
                        &errorCode, nextOutcomes)) {
        result.ledger = current;
        result.errorCode = errorCode.isEmpty()
            ? code("store-write-failed")
            : errorCode;
        return result;
    }

    // 提交之后重新读取，而不是相信追加：只有重新读到的字节才是真正生效的记录。
    const ExtensionStagingRestoreAuditStoreResult refreshed = store->load();
    result.ledger = refreshed;
    if (refreshed.state != ExtensionStagingRestoreAuditStoreState::Ready) {
        result.errorCode = refreshed.errorCode.isEmpty()
            ? code("store-refresh-failed")
            : refreshed.errorCode;
        return result;
    }
    result.recorded = true;
    return result;
}
