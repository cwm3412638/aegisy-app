#ifndef EXTENSION_LIFECYCLE_CONTROLLER_H
#define EXTENSION_LIFECYCLE_CONTROLLER_H

#include "extension_enablement_ledger_store.h"
#include "extension_inventory_coordinator.h"
#include "extension_review_ledger_store.h"
#include "extension_update_policy.h"

// 把更新与移除的判定接到两份带认证的账本上。`ExtensionUpdatePolicy` 已经能判定一次
// 更新或移除的结论,但判定本身不改动任何持久状态,于是"移除必须收回授权"这条结论到此
// 为止没有任何东西去执行它。这一层执行它。
//
// 更新的执行部分恰恰是**什么都不写**。校验通过只让候选可以被暂存,因此这一层在暂存
// 路径上绝不为候选创建复核记录或启用授权:候选按定义是另一份内容,它必须重新经过人工
// 复核与一次独立授权。旧内容的记录留在账本里,但它绑定的是旧内容摘要,因此对候选而言
// 是漂移,而不是可继承的权威。
//
// 移除的执行部分要写两份账本,而两次写入不可能是一次原子操作。顺序因此是安全性的一部分:
// **先收回启用授权,再收回复核记录**。授权是真正运行内容的那一半,先收回它意味着任何
// 中间失败都停在"没有授权、复核记录尚存"上——注册表的双重门禁下那是未启用,是安全的
// 一侧。反过来先删复核记录会短暂留下"有授权、无复核"的状态,那是更坏的中间态,而且它
// 抹掉的正是事后审计需要的证据。
//
// 部分完成必须可分辨,不能报成成功:一个只收回了授权的移除仍然留着复核记录,而人需要
// 知道这一点才会去把它清掉。
//
// 这一层不安装、不下载、不解压、不执行任何东西。它读取账本、判定、按顺序提交,再重新
// 读取以确认。
struct ExtensionLifecycleSnapshot {
    ExtensionInventorySnapshot inventory;
    ExtensionReviewLedgerStoreState reviewState =
        ExtensionReviewLedgerStoreState::Invalid;
    ExtensionEnablementLedgerStoreState grantState =
        ExtensionEnablementLedgerStoreState::Invalid;
    QList<ExtensionReviewPin> pins;
    QList<ExtensionEnablementGrant> grants;
    qint64 reviewGeneration = 0;
    qint64 grantGeneration = 0;
    QString reviewErrorCode;
    QString grantErrorCode;
};

enum class ExtensionLifecycleOutcome {
    // 判定拒绝,或前置账本不可用。两份账本都未被改动。
    Refused,
    // 更新校验通过。候选可被暂存,但仍然未复核、未授权,因此不可执行。
    StagedUnreviewed,
    // 移除完成:启用授权与复核记录都已收回,不可变身份被保留。
    Withdrawn,
    // 授权已收回,复核记录收回失败。这是安全的一侧,但必须可分辨。
    PartiallyWithdrawn,
};

struct ExtensionLifecycleResult {
    ExtensionLifecycleOutcome outcome = ExtensionLifecycleOutcome::Refused;
    // 恒为真:当前生效的版本不被这一层替换。替换是另一次显式操作。
    bool activePreserved = true;
    // 恒为假:候选在被独立复核并授权之前不可运行。
    bool candidateExecutable = false;
    // 恒为假:信任绑定确切内容,不随更新传递。
    bool inheritsTrust = false;
    // 恒为假:授权同样绑定确切内容。更新后必须重新授权。
    bool inheritsGrant = false;
    // 候选相对当前版本是降级。不被禁止,但必须可见。
    bool downgrade = false;
    // 移除时:启用授权是否确实已从账本收回。
    bool grantRevoked = false;
    // 移除时:复核记录是否确实已从账本收回。
    bool reviewRevoked = false;
    // 移除时:可执行内容是否被停用或删除。
    bool executableContentRemoved = false;
    // 移除后保留的不可变身份。抹掉它会让"这份内容曾被授权运行过"的历史消失。
    QString retainedIdentity;
    ExtensionLifecycleSnapshot snapshot;
    QString errorCode;
};

class ExtensionLifecycleController
{
public:
    static ExtensionLifecycleSnapshot inspect(
        const ExtensionInventoryInputs &inputs,
        ExtensionReviewLedgerStore *reviewStore,
        ExtensionEnablementLedgerStore *grantStore);

    // 暂存一次更新。校验失败时当前版本保持不变且候选不执行;校验通过时候选可被暂存,
    // 但这一层不为它写入任何复核记录或启用授权。
    static ExtensionLifecycleResult stageUpdate(
        const ExtensionInventoryInputs &inputs,
        const ExtensionUpdateCandidate &candidate,
        const ExtensionUpdateEvidence &evidence,
        ExtensionReviewLedgerStore *reviewStore,
        ExtensionEnablementLedgerStore *grantStore);

    // 移除一个扩展:先收回启用授权,再收回复核记录,并保留不可变身份。目标已经不存在
    // 时仍然必须能够收回授权并留下身份。
    static ExtensionLifecycleResult remove(
        const ExtensionInventoryInputs &inputs,
        ExtensionKind kind, const QString &id,
        ExtensionReviewLedgerStore *reviewStore,
        ExtensionEnablementLedgerStore *grantStore);
};

#endif // EXTENSION_LIFECYCLE_CONTROLLER_H
