#include "extension_recovery_controller.h"

namespace {

// 每一条返回路径都必须写出这四个恒定字段。一次被拒绝的恢复同样只会收回授权、同样不碰
// 复核账本、同样不清除事务,而把它们留给结构体默认值意味着源码里没有任何一处声明这件事。
ExtensionRecoveryResult refuse(ExtensionRecoveryNeed need,
                               const ExtensionEnablementLedgerStoreResult &ledger,
                               const QString &code)
{
    ExtensionRecoveryResult result;
    result.outcome = ExtensionRecoveryOutcome::Refused;
    result.need = need;
    result.withdrawsAuthorityOnly = true;
    result.reviewLedgerTouched = false;
    result.clearsTransaction = false;
    result.grantState = ledger.state;
    result.grantGeneration = ledger.generation;
    // 读不出来的账本不报告条数:一份状态未知的集合里"有几条"这个数字本身是编出来的。
    result.survivingGrants = ExtensionRecoveryGate::authoritative(ledger.state)
        ? ledger.grants.size() : 0;
    result.errorCode = code;
    return result;
}

ExtensionEnablementLedgerStoreResult unavailable()
{
    ExtensionEnablementLedgerStoreResult value;
    value.state = ExtensionEnablementLedgerStoreState::Unavailable;
    value.errorCode = QStringLiteral("extension-enablement-store-unavailable");
    return value;
}

} // namespace

ExtensionRecoveryView ExtensionRecoveryController::assess(
    ExtensionEnablementLedgerStore *grantStore)
{
    // 没有存储就是读不出来,而读不出来时唯一安全的动作是什么都不做。这里绝不能退化成
    // "从未授权过":那会清掉看不见的授权。
    const ExtensionEnablementLedgerStoreResult ledger =
        grantStore ? grantStore->load() : unavailable();
    ExtensionRecoveryView view;
    view.assessment = ExtensionRecoveryGate::assess(ledger);
    // 代号原样交出。界面必须回传它读到的这一份,而不是它记着的上一份。
    view.generation = ledger.generation;
    view.grantState = ledger.state;
    view.visibleGrants = ExtensionRecoveryGate::authoritative(ledger.state)
        ? ledger.grants.size() : 0;
    return view;
}

ExtensionRecoveryResult ExtensionRecoveryController::apply(
    ExtensionEnablementLedgerStore *grantStore,
    const ExtensionRecoveryRequest &request)
{
    if (!grantStore) {
        const ExtensionEnablementLedgerStoreResult ledger = unavailable();
        return refuse(ExtensionRecoveryGate::assess(ledger).need, ledger,
                      ledger.errorCode);
    }

    // 判定所依据的必须是此刻重新读到的账本,而不是发起恢复时缓存的那一份:恢复会把授权
    // 归零,拿一份过期的读取去决定这件事等于在猜测当下的状态。
    const ExtensionEnablementLedgerStoreResult ledger = grantStore->load();
    const ExtensionRecoveryAssessment assessment =
        ExtensionRecoveryGate::assess(ledger);
    const ExtensionRecoveryPlan plan =
        ExtensionRecoveryGate::plan(ledger, request);
    if (plan.state != ExtensionRecoveryPlanState::Ready) {
        // 判定层的诊断原样带出。这一层再编一个代号会让操作者拿着一个查不到出处的东西,
        // 而恢复恰好是最需要能查出处的那条路径。
        return refuse(assessment.need, ledger, plan.errorCode);
    }

    // 走 `discard` 而不是 `replace`:`replace` 拒绝在 `Invalid` 之上写入,而那正是恢复要
    // 处理的那个状态,所以恢复没有 `discard` 就完全无法执行。这一点也是安全性的一部分——
    // `discard` 结构上只会清空,不接受任何条目,因此这条路径连"写入一份非空授权集合"这个
    // 动作都无法表达。任何能产出非空集合的恢复路径都是一条制造同意的路径。
    ExtensionEnablementLedgerStoreResult acknowledged;
    QString errorCode;
    const bool discarded = grantStore->discard(&acknowledged, &errorCode);

    ExtensionRecoveryResult result;
    result.need = assessment.need;
    result.withdrawsAuthorityOnly = true;
    result.reviewLedgerTouched = false;
    result.clearsTransaction = false;

    // 一次被确认的写入不是证据。结论只能来自重新读出来的字节:一个确认了写入却没有真的
    // 持久化的后端会让"授权已全部收回"成为一句谎报,而操作者会因此不再回来看。因此即使
    // `discard` 返回失败,这里也仍然重新读一次:一次中断可能已经销毁了授权密钥,而那时
    // 授权确实已经不可能再生效,把它报成"什么都没发生"会让操作者以为损坏还在原处。
    const ExtensionEnablementLedgerStoreResult reread = grantStore->load();
    result.grantState = reread.state;
    result.grantGeneration = reread.generation;
    result.survivingGrants = ExtensionRecoveryGate::authoritative(reread.state)
        ? reread.grants.size() : 0;
    // 完成与否只有一个来源:判定层的 `completed`。这里另算一遍必然会与它漂移,而漂移的
    // 方向是把一次没做完的恢复报成做完了。
    if (!ExtensionRecoveryGate::completed(reread)) {
        result.outcome = ExtensionRecoveryOutcome::Incomplete;
        // 存储层的诊断优先:它知道是哪一阶段断掉的,而这一层只知道"没读到空集合"。
        result.errorCode = !errorCode.isEmpty() ? errorCode
            : (reread.errorCode.isEmpty()
                   ? QStringLiteral("extension-recovery-incomplete")
                   : reread.errorCode);
        return result;
    }
    // 重新读取得到空集合。此时即使 `discard` 报了失败也仍然是完成:结论来自字节,不来自
    // 那个返回值。反过来说,`discard` 成功但重新读取不是空集合时上面那一支已经拦住了。
    Q_UNUSED(discarded);
    result.outcome = ExtensionRecoveryOutcome::Cleared;
    return result;
}
