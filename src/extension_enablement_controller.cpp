#include "extension_enablement_controller.h"

namespace {

ExtensionEnablementSnapshot collectWithLedger(
    const ExtensionInventoryInputs &inputs,
    const ExtensionEnablementLedgerStoreResult &ledger)
{
    ExtensionEnablementSnapshot snapshot;
    snapshot.ledgerState = ledger.state;
    snapshot.generation = ledger.generation;
    snapshot.identity = ledger.identity;
    snapshot.ledgerErrorCode = ledger.errorCode;
    // 读不出授权集合时不返回任何授权：一份不完整的集合会让界面显示"这些扩展没有被
    // 授权过"，而实际情况是当前授权未知。
    if (ledger.state == ExtensionEnablementLedgerStoreState::Ready
            || ledger.state == ExtensionEnablementLedgerStoreState::Empty) {
        snapshot.grants = ledger.grants;
    }

    // 清单按原样收集：授权**不**进入协调器输入。协调器会据此写 effectiveEnabled，
    // 也就是真正运行扩展内容的权限，而那道门在权限、审批、沙箱与恢复门禁完成之前
    // 必须保持关闭。
    snapshot.inventory = ExtensionInventoryCoordinator::collect(inputs);

    // 判定只作为与记录一一对应的投影返回，绝不写回记录。授权读不出来时每一条都按
    // "未授权"呈现，因为那正是判定在缺少授权时的结论。
    snapshot.decisions.reserve(snapshot.inventory.records.size());
    for (const ExtensionRegistryRecord &record : snapshot.inventory.records) {
        snapshot.decisions.append(
            ExtensionEnablementPolicy::evaluate(record, snapshot.grants));
    }
    return snapshot;
}

ExtensionEnablementOperationResult failure(
    const ExtensionEnablementSnapshot &snapshot, const QString &errorCode)
{
    ExtensionEnablementOperationResult result;
    result.snapshot = snapshot;
    result.errorCode = errorCode;
    return result;
}

} // namespace

ExtensionEnablementSnapshot ExtensionEnablementController::inspect(
    const ExtensionInventoryInputs &inputs,
    ExtensionEnablementLedgerStore *store)
{
    if (!store) {
        ExtensionEnablementLedgerStoreResult ledger;
        ledger.state = ExtensionEnablementLedgerStoreState::Unavailable;
        ledger.errorCode =
            QStringLiteral("extension-enablement-store-unavailable");
        return collectWithLedger(inputs, ledger);
    }
    return collectWithLedger(inputs, store->load());
}

ExtensionEnablementOperationResult ExtensionEnablementController::apply(
    const ExtensionInventoryInputs &inputs,
    const ExtensionEnablementRequest &request,
    ExtensionEnablementLedgerStore *store)
{
    if (!store) {
        return failure({},
            QStringLiteral("extension-enablement-store-unavailable"));
    }
    const ExtensionEnablementLedgerStoreResult ledger = store->load();
    const ExtensionEnablementSnapshot current =
        collectWithLedger(inputs, ledger);
    if (ledger.state != ExtensionEnablementLedgerStoreState::Ready
            && ledger.state != ExtensionEnablementLedgerStoreState::Empty) {
        return failure(current, ledger.errorCode.isEmpty()
            ? QStringLiteral("extension-enablement-ledger-unusable")
            : ledger.errorCode);
    }
    const ExtensionEnablementPlan plan = ExtensionEnablementWorkflow::plan(
        request, current.inventory.records, ledger);
    if (plan.state != ExtensionEnablementPlanState::Ready) {
        return failure(current, plan.errorCode.isEmpty()
            ? QStringLiteral("extension-enablement-plan-rejected")
            : plan.errorCode);
    }
    // 集合没有变化时不提交：提交只会白白推进代号并改变身份摘要。
    if (!plan.changed) {
        ExtensionEnablementOperationResult result;
        result.committed = true;
        result.snapshot = current;
        return result;
    }
    ExtensionEnablementLedgerStoreResult updated;
    QString errorCode;
    if (!store->replace(plan.grants, plan.expectedGeneration, &updated,
                        &errorCode)) {
        return failure(current, errorCode.isEmpty()
            ? QStringLiteral("extension-enablement-store-write-failed")
            : errorCode);
    }
    // 提交之后重新读取，而不是相信规划的结果：只有重新读到的字节才是真正生效的授权。
    ExtensionEnablementSnapshot refreshed = collectWithLedger(inputs, updated);
    if (refreshed.ledgerState != ExtensionEnablementLedgerStoreState::Ready) {
        return failure(refreshed, refreshed.ledgerErrorCode.isEmpty()
            ? QStringLiteral("extension-enablement-store-refresh-failed")
            : refreshed.ledgerErrorCode);
    }
    ExtensionEnablementOperationResult result;
    result.committed = true;
    result.changed = true;
    result.snapshot = refreshed;
    return result;
}
