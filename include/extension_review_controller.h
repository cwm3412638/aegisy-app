#ifndef EXTENSION_REVIEW_CONTROLLER_H
#define EXTENSION_REVIEW_CONTROLLER_H

#include "extension_inventory_coordinator.h"
#include "extension_review_ledger_store.h"
#include "extension_review_workflow.h"

struct ExtensionReviewSnapshot
{
    ExtensionInventorySnapshot inventory;
    ExtensionReviewLedgerStoreState ledgerState =
        ExtensionReviewLedgerStoreState::Invalid;
    QList<ExtensionReviewPin> pins;
    qint64 generation = 0;
    QString identity;
    QString ledgerErrorCode;
};

struct ExtensionReviewOperationResult
{
    bool committed = false;
    bool changed = false;
    ExtensionReviewSnapshot snapshot;
    QString errorCode;
};

class ExtensionReviewController
{
public:
    static ExtensionReviewSnapshot inspect(
        const ExtensionInventoryInputs &inputs,
        ExtensionReviewLedgerStore *store);
    static ExtensionReviewOperationResult apply(
        const ExtensionInventoryInputs &inputs,
        const ExtensionReviewRequest &request,
        ExtensionReviewLedgerStore *store);
};

#endif // EXTENSION_REVIEW_CONTROLLER_H
