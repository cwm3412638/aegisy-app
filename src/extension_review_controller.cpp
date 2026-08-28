#include "extension_review_controller.h"

namespace {

ExtensionReviewSnapshot collectWithLedger(
    const ExtensionInventoryInputs &inputs,
    const ExtensionReviewLedgerStoreResult &ledger)
{
    ExtensionReviewSnapshot snapshot;
    snapshot.ledgerState = ledger.state;
    snapshot.pins = ledger.pins;
    snapshot.generation = ledger.generation;
    snapshot.identity = ledger.identity;
    snapshot.ledgerErrorCode = ledger.errorCode;
    ExtensionInventoryInputs bound = inputs;
    if (ledger.state == ExtensionReviewLedgerStoreState::Ready
            || ledger.state == ExtensionReviewLedgerStoreState::Empty) {
        bound.reviewPins = ledger.pins;
    } else {
        bound.reviewPins.clear();
    }
    snapshot.inventory = ExtensionInventoryCoordinator::collect(bound);
    return snapshot;
}

ExtensionReviewOperationResult failure(
    const ExtensionReviewSnapshot &snapshot, const QString &errorCode)
{
    ExtensionReviewOperationResult result;
    result.snapshot = snapshot;
    result.errorCode = errorCode;
    return result;
}

} // namespace

ExtensionReviewSnapshot ExtensionReviewController::inspect(
    const ExtensionInventoryInputs &inputs,
    ExtensionReviewLedgerStore *store)
{
    if (!store) {
        ExtensionReviewSnapshot snapshot;
        snapshot.ledgerState = ExtensionReviewLedgerStoreState::Unavailable;
        snapshot.ledgerErrorCode = QStringLiteral("extension-review-store-unavailable");
        snapshot.inventory = ExtensionInventoryCoordinator::collect(inputs);
        return snapshot;
    }
    return collectWithLedger(inputs, store->load());
}

ExtensionReviewOperationResult ExtensionReviewController::apply(
    const ExtensionInventoryInputs &inputs,
    const ExtensionReviewRequest &request,
    ExtensionReviewLedgerStore *store)
{
    if (!store) {
        return failure({}, QStringLiteral("extension-review-store-unavailable"));
    }
    const ExtensionReviewLedgerStoreResult ledger = store->load();
    const ExtensionReviewSnapshot current = collectWithLedger(inputs, ledger);
    if (ledger.state != ExtensionReviewLedgerStoreState::Ready
            && ledger.state != ExtensionReviewLedgerStoreState::Empty) {
        return failure(current, ledger.errorCode.isEmpty()
            ? QStringLiteral("extension-review-ledger-unusable") : ledger.errorCode);
    }
    const ExtensionReviewPlan plan = ExtensionReviewWorkflow::plan(
        request, current.inventory.records, ledger);
    if (plan.state != ExtensionReviewPlanState::Ready) {
        return failure(current, plan.errorCode.isEmpty()
            ? QStringLiteral("extension-review-plan-rejected") : plan.errorCode);
    }
    if (!plan.changed) {
        ExtensionReviewOperationResult result;
        result.committed = true;
        result.snapshot = current;
        return result;
    }
    ExtensionReviewLedgerStoreResult updated;
    QString errorCode;
    if (!store->replace(plan.pins, plan.expectedGeneration, &updated, &errorCode)) {
        return failure(current, errorCode.isEmpty()
            ? QStringLiteral("extension-review-store-write-failed") : errorCode);
    }
    ExtensionReviewSnapshot refreshed = collectWithLedger(inputs, updated);
    if (refreshed.ledgerState != ExtensionReviewLedgerStoreState::Ready) {
        return failure(refreshed, refreshed.ledgerErrorCode.isEmpty()
            ? QStringLiteral("extension-review-store-refresh-failed")
            : refreshed.ledgerErrorCode);
    }
    ExtensionReviewOperationResult result;
    result.committed = true;
    result.changed = true;
    result.snapshot = refreshed;
    return result;
}
