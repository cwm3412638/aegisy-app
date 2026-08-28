#include "extension_recovery_gate.h"

namespace {

ExtensionRecoveryAssessment assessment(ExtensionRecoveryNeed need,
                                       bool confirmationRequired,
                                       const QString &errorCode)
{
    ExtensionRecoveryAssessment value;
    value.need = need;
    value.operatorConfirmationRequired = confirmationRequired;
    // 这一层能计划的每一个结果都只收回授权。这个不变量是硬编码的真值,而不是从计划里
    // 推导出来的:一旦某条路径开始产出授权,它就不再是恢复。
    value.withdrawsAuthorityOnly = true;
    value.errorCode = errorCode;
    return value;
}

ExtensionRecoveryPlan refuse(const QString &code)
{
    ExtensionRecoveryPlan plan;
    plan.state = ExtensionRecoveryPlanState::Refused;
    plan.clearsTransaction = false;
    plan.errorCode = code;
    return plan;
}

} // namespace

bool ExtensionRecoveryGate::authoritative(
    ExtensionEnablementLedgerStoreState state)
{
    switch (state) {
    case ExtensionEnablementLedgerStoreState::Empty:
    case ExtensionEnablementLedgerStoreState::Ready:
        return true;
    case ExtensionEnablementLedgerStoreState::Invalid:
    case ExtensionEnablementLedgerStoreState::Unavailable:
    case ExtensionEnablementLedgerStoreState::OutcomeUnknown:
        return false;
    }
    // 未知状态按不可读处理:新增的存储状态不应默认被当成可信的授权集合。
    return false;
}

ExtensionRecoveryAssessment ExtensionRecoveryGate::assess(
    const ExtensionEnablementLedgerStoreResult &ledger)
{
    switch (ledger.state) {
    case ExtensionEnablementLedgerStoreState::Empty:
    case ExtensionEnablementLedgerStoreState::Ready:
        // 账本状态明确时不存在需要恢复的损坏。对一个健康账本提供恢复动作等于提供一条
        // 绕过审批的批量撤销后门,因此这里必须是明确的"无需恢复"。
        return assessment(ExtensionRecoveryNeed::None, false,
                          QStringLiteral("extension-recovery-not-required"));
    case ExtensionEnablementLedgerStoreState::Unavailable:
        // 后端被锁定或不可读时当前内容未知。什么都不做是唯一安全的动作:清空一份读不到
        // 的授权集合会销毁看不见的授权,而这些授权此刻无法被展示给操作者确认。
        return assessment(ExtensionRecoveryNeed::Blocked, false,
                          QStringLiteral("extension-recovery-store-unavailable"));
    case ExtensionEnablementLedgerStoreState::OutcomeUnknown:
        // 上一次发布的结果未知。必须重新读取以确立当下状态,而不是在未知之上写入:那可能
        // 覆盖一次其实已经提交的发布。
        return assessment(ExtensionRecoveryNeed::Reconfirm, false,
                          QStringLiteral("extension-recovery-outcome-unknown"));
    case ExtensionEnablementLedgerStoreState::Invalid:
        // 证据自相矛盾。恢复不推断过去:无法知道哪一条授权曾经有效,因此唯一诚实的重建是
        // 空集合。收回授权仍然是一次真实的授权变更,所以要求显式确认。
        return assessment(ExtensionRecoveryNeed::ClearGrants, true,
                          QStringLiteral("extension-recovery-evidence-invalid"));
    }
    // 未知状态按不可读处理,并且不提供任何恢复动作。
    return assessment(ExtensionRecoveryNeed::Blocked, false,
                      QStringLiteral("extension-recovery-state-unknown"));
}

ExtensionRecoveryPlan ExtensionRecoveryGate::plan(
    const ExtensionEnablementLedgerStoreResult &ledger,
    const ExtensionRecoveryRequest &request)
{
    // 可读的账本不得被恢复动作触碰。恢复会清空全部授权,如果它能作用在健康账本上,它就是
    // 一条不经审批就撤销一切的路径。
    if (authoritative(ledger.state)) {
        return refuse(QStringLiteral("extension-recovery-not-required"));
    }

    const ExtensionRecoveryAssessment current = assess(ledger);
    // 界面展示的结论必须与重新评估的结论一致。不一致说明操作者看到的是过期状态,而恢复
    // 决定必须针对当下真实的损坏做出。
    if (request.acknowledgedNeed != current.need) {
        return refuse(QStringLiteral("extension-recovery-assessment-stale"));
    }

    switch (current.need) {
    case ExtensionRecoveryNeed::Blocked:
        // 读不到内容时什么都不做。
        return refuse(QStringLiteral("extension-recovery-blocked"));
    case ExtensionRecoveryNeed::Reconfirm:
        // 结果未知时必须先重新读取,不能在未知之上写入。
        return refuse(QStringLiteral("extension-recovery-reread-required"));
    case ExtensionRecoveryNeed::None:
        return refuse(QStringLiteral("extension-recovery-not-required"));
    case ExtensionRecoveryNeed::ClearGrants:
        break;
    }

    if (!request.operatorConfirmed) {
        return refuse(QStringLiteral("extension-recovery-confirmation-required"));
    }
    // 恢复必须提交操作者读到的代号:一次并发的授予不允许被恢复静默覆盖。
    if (request.expectedGeneration != ledger.generation) {
        return refuse(QStringLiteral("extension-recovery-generation-stale"));
    }

    ExtensionRecoveryPlan plan;
    plan.state = ExtensionRecoveryPlanState::Ready;
    // 空集合是唯一诚实的重建。任何非空结果都是在伪造从未被人做出的授权决定。
    plan.grants.clear();
    plan.expectedGeneration = request.expectedGeneration;
    // 事务不在这里清除。必须在提交后重新读取并验证,否则一次部分完成的恢复会被当成已
    // 完成的恢复。
    plan.clearsTransaction = false;
    return plan;
}

bool ExtensionRecoveryGate::completed(
    const ExtensionEnablementLedgerStoreResult &reread)
{
    // 只有重新读取确实得到"从未授权过"才算完成。`Ready` 说明仍有授权残留,`Invalid`
    // 说明损坏依旧,`Unavailable` 与 `OutcomeUnknown` 说明当下无从判断——这三类都必须让
    // 事务保持打开,而不是被乐观地关掉。
    return reread.state == ExtensionEnablementLedgerStoreState::Empty
        && reread.grants.isEmpty();
}
