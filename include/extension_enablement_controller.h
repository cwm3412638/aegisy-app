#ifndef EXTENSION_ENABLEMENT_CONTROLLER_H
#define EXTENSION_ENABLEMENT_CONTROLLER_H

#include "extension_enablement_ledger_store.h"
#include "extension_enablement_workflow.h"
#include "extension_inventory_coordinator.h"

// 把启用授权的规划与持久化接在一起：读取当前授权集合、规划、通过比较并交换提交、
// 再重新读取。与复核控制器同构。
//
// 关键的授权边界：本层**不**把授权喂给清单协调器。协调器一旦收到授权就会写
// `record.effectiveEnabled`，那是真正运行扩展内容的权限。在权限、审批、沙箱与恢复
// 门禁完成之前这道门必须保持关闭，因此授权判定只作为与记录一一对应的诊断投影
// (`decisions`) 返回，绝不写回记录本身。注册表的 `Verified + Compatible` 门也因此
// 保持原样：每一条出货记录仍然是未启用的。
struct ExtensionEnablementSnapshot
{
    ExtensionInventorySnapshot inventory;
    ExtensionEnablementLedgerStoreState ledgerState =
        ExtensionEnablementLedgerStoreState::Invalid;
    QList<ExtensionEnablementGrant> grants;
    qint64 generation = 0;
    QString identity;
    QString ledgerErrorCode;
    // 与 `inventory.records` 逐位对应的启用判定。仅供展示与诊断。
    QList<ExtensionEnablementDecision> decisions;
};

struct ExtensionEnablementOperationResult
{
    bool committed = false;
    bool changed = false;
    ExtensionEnablementSnapshot snapshot;
    QString errorCode;
};

class ExtensionEnablementController
{
public:
    static ExtensionEnablementSnapshot inspect(
        const ExtensionInventoryInputs &inputs,
        ExtensionEnablementLedgerStore *store);
    static ExtensionEnablementOperationResult apply(
        const ExtensionInventoryInputs &inputs,
        const ExtensionEnablementRequest &request,
        ExtensionEnablementLedgerStore *store);
};

#endif // EXTENSION_ENABLEMENT_CONTROLLER_H
