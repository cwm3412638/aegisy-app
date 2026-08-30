#include "extension_lifecycle_controller.h"

#include "extension_display_safety.h"

namespace {

using Safety = ExtensionDisplaySafety;

bool ledgerUsable(ExtensionReviewLedgerStoreState state)
{
    return state == ExtensionReviewLedgerStoreState::Ready
        || state == ExtensionReviewLedgerStoreState::Empty;
}

bool ledgerUsable(ExtensionEnablementLedgerStoreState state)
{
    return state == ExtensionEnablementLedgerStoreState::Ready
        || state == ExtensionEnablementLedgerStoreState::Empty;
}

ExtensionLifecycleSnapshot collect(
    const ExtensionInventoryInputs &inputs,
    const ExtensionReviewLedgerStoreResult &review,
    const ExtensionEnablementLedgerStoreResult &grants)
{
    ExtensionLifecycleSnapshot snapshot;
    snapshot.reviewState = review.state;
    snapshot.grantState = grants.state;
    snapshot.reviewGeneration = review.generation;
    snapshot.grantGeneration = grants.generation;
    snapshot.reviewErrorCode = review.errorCode;
    snapshot.grantErrorCode = grants.errorCode;
    // 读不出的账本不返回内容。一份不完整的集合会让界面显示"没有被复核/授权过",而实际
    // 情况是当前状态未知。
    if (ledgerUsable(review.state)) snapshot.pins = review.pins;
    if (ledgerUsable(grants.state)) snapshot.grants = grants.grants;

    // 清单收集时带上复核记录,因为信任判定本身需要它们;授权**不**进入协调器输入,
    // 那道写 effectiveEnabled 的门在权限、审批、沙箱与恢复门禁完成之前保持关闭。
    ExtensionInventoryInputs bound = inputs;
    bound.reviewPins = snapshot.pins;
    snapshot.inventory = ExtensionInventoryCoordinator::collect(bound);
    return snapshot;
}

ExtensionLifecycleResult refuse(const ExtensionLifecycleSnapshot &snapshot,
                                const QString &code)
{
    ExtensionLifecycleResult result;
    result.outcome = ExtensionLifecycleOutcome::Refused;
    // 被拒绝时当前版本仍然不变,候选仍然不可执行,也仍然不继承任何权威。这些不变量在
    // 每一条返回路径上成立,而不是只在成功路径上被设置。
    result.activePreserved = true;
    result.candidateExecutable = false;
    result.inheritsTrust = false;
    result.inheritsGrant = false;
    result.snapshot = snapshot;
    result.errorCode = code;
    return result;
}

const ExtensionRegistryRecord *findRecord(
    const QList<ExtensionRegistryRecord> &records,
    ExtensionKind kind, const QString &id)
{
    for (const ExtensionRegistryRecord &record : records) {
        if (record.kind == kind && record.id == id) return &record;
    }
    return nullptr;
}

// 收回一个目标的全部记录。绑定按种类与 ID:被移除的内容摘要可能已经不可读,而移除
// 必须仍然能够收回它留下的授权。
QList<ExtensionEnablementGrant> withoutGrants(
    const QList<ExtensionEnablementGrant> &grants,
    ExtensionKind kind, const QString &id)
{
    QList<ExtensionEnablementGrant> remaining;
    for (const ExtensionEnablementGrant &grant : grants) {
        if (grant.kind == kind && grant.id == id) continue;
        remaining.append(grant);
    }
    return remaining;
}

QList<ExtensionReviewPin> withoutPins(const QList<ExtensionReviewPin> &pins,
                                      ExtensionKind kind, const QString &id)
{
    QList<ExtensionReviewPin> remaining;
    for (const ExtensionReviewPin &pin : pins) {
        if (pin.kind == kind && pin.id == id) continue;
        remaining.append(pin);
    }
    return remaining;
}

} // namespace

ExtensionLifecycleSnapshot ExtensionLifecycleController::inspect(
    const ExtensionInventoryInputs &inputs,
    ExtensionReviewLedgerStore *reviewStore,
    ExtensionEnablementLedgerStore *grantStore)
{
    ExtensionReviewLedgerStoreResult review;
    if (reviewStore) {
        review = reviewStore->load();
    } else {
        review.state = ExtensionReviewLedgerStoreState::Unavailable;
        review.errorCode = QStringLiteral("extension-review-store-unavailable");
    }
    ExtensionEnablementLedgerStoreResult grants;
    if (grantStore) {
        grants = grantStore->load();
    } else {
        grants.state = ExtensionEnablementLedgerStoreState::Unavailable;
        grants.errorCode =
            QStringLiteral("extension-enablement-store-unavailable");
    }
    return collect(inputs, review, grants);
}

ExtensionLifecycleResult ExtensionLifecycleController::stageUpdate(
    const ExtensionInventoryInputs &inputs,
    const ExtensionUpdateCandidate &candidate,
    const ExtensionUpdateEvidence &evidence,
    ExtensionReviewLedgerStore *reviewStore,
    ExtensionEnablementLedgerStore *grantStore)
{
    const ExtensionLifecycleSnapshot snapshot =
        inspect(inputs, reviewStore, grantStore);
    if (!Safety::validId(candidate.id)) {
        return refuse(snapshot, QStringLiteral("extension-update-target-mismatch"));
    }
    const ExtensionRegistryRecord *active =
        findRecord(snapshot.inventory.records, candidate.kind, candidate.id);
    if (!active) {
        // 更新一个不存在的目标等于预先授权将来出现的内容。
        return refuse(snapshot, QStringLiteral("extension-update-target-absent"));
    }

    const ExtensionUpdateVerdict verdict =
        ExtensionUpdatePolicy::evaluate(*active, candidate, evidence);
    if (verdict.state != ExtensionUpdateState::StagedUnreviewed) {
        // 校验失败:当前版本保持不变,候选不执行,两份账本一个字节都不动。
        ExtensionLifecycleResult result = refuse(snapshot, verdict.errorCode.isEmpty()
            ? QStringLiteral("extension-update-rejected") : verdict.errorCode);
        result.downgrade = verdict.downgrade;
        return result;
    }

    ExtensionLifecycleResult result;
    result.outcome = ExtensionLifecycleOutcome::StagedUnreviewed;
    result.snapshot = snapshot;
    result.activePreserved = true;
    // 校验通过只让候选可以被暂存。这一层不为候选写入任何复核记录或启用授权:候选按定义
    // 是另一份内容,必须重新经过人工复核与一次独立授权。旧内容的记录留在账本里,但它
    // 绑定旧内容摘要,因此对候选而言是漂移而不是可继承的权威。
    result.candidateExecutable = false;
    result.inheritsTrust = false;
    result.inheritsGrant = false;
    result.downgrade = verdict.downgrade;
    return result;
}

ExtensionLifecycleResult ExtensionLifecycleController::remove(
    const ExtensionInventoryInputs &inputs,
    ExtensionKind kind, const QString &id,
    ExtensionReviewLedgerStore *reviewStore,
    ExtensionEnablementLedgerStore *grantStore)
{
    ExtensionLifecycleSnapshot snapshot =
        inspect(inputs, reviewStore, grantStore);
    if (!reviewStore || !grantStore) {
        return refuse(snapshot,
            QStringLiteral("extension-removal-store-unavailable"));
    }
    // 两份账本都必须可读才能移除。在授权未知的情况下声称已经收回授权,是这一层最不该
    // 做的一件事。
    if (!ledgerUsable(snapshot.grantState)) {
        return refuse(snapshot, snapshot.grantErrorCode.isEmpty()
            ? QStringLiteral("extension-removal-grant-ledger-unusable")
            : snapshot.grantErrorCode);
    }
    if (!ledgerUsable(snapshot.reviewState)) {
        return refuse(snapshot, snapshot.reviewErrorCode.isEmpty()
            ? QStringLiteral("extension-removal-review-ledger-unusable")
            : snapshot.reviewErrorCode);
    }

    const ExtensionRegistryRecord *record =
        findRecord(snapshot.inventory.records, kind, id);
    const ExtensionRemovalVerdict verdict =
        ExtensionUpdatePolicy::evaluateRemoval(kind, id, record);
    if (verdict.state != ExtensionRemovalState::Ready) {
        return refuse(snapshot, verdict.errorCode.isEmpty()
            ? QStringLiteral("extension-removal-rejected") : verdict.errorCode);
    }

    ExtensionLifecycleResult result;
    result.snapshot = snapshot;
    result.executableContentRemoved = verdict.removesExecutableContent;
    // 不可变身份在任何结局下都被保留,包括部分完成:抹掉它会让"这份内容曾被授权运行过"
    // 的历史一并消失,而移除恰好是最需要留下记录的操作之一。
    result.retainedIdentity = verdict.retainedIdentity;

    // 顺序是安全性的一部分。**先收回启用授权**:授权是真正运行内容的那一半,先收回它
    // 意味着任何中间失败都停在"没有授权、复核记录尚存"上,在注册表双重门禁下那是未启用。
    // 反过来先删复核记录会短暂留下"有授权、无复核"的更坏中间态,并且抹掉的正是审计证据。
    const QList<ExtensionEnablementGrant> remainingGrants =
        withoutGrants(snapshot.grants, kind, id);
    if (remainingGrants.size() != snapshot.grants.size()) {
        ExtensionEnablementLedgerStoreResult acknowledged;
        QString errorCode;
        if (!grantStore->replace(remainingGrants, snapshot.grantGeneration,
                                 &acknowledged, &errorCode)) {
            return refuse(snapshot, errorCode.isEmpty()
                ? QStringLiteral("extension-removal-grant-write-failed")
                : errorCode);
        }
        // 一次被确认的写入不是证据。结论只能来自重新读出来的字节:一个确认了写入却没有
        // 真的持久化的后端,会让"授权已收回"成为一句谎报,而那是这里最坏的一种错误。
        const ExtensionEnablementLedgerStoreResult reread = grantStore->load();
        snapshot.grantState = reread.state;
        snapshot.grantGeneration = reread.generation;
        snapshot.grantErrorCode = reread.errorCode;
        snapshot.grants = ledgerUsable(reread.state)
            ? reread.grants
            : QList<ExtensionEnablementGrant>();
        if (!ledgerUsable(reread.state)) {
            // 授权集合现在读不出来,因此不能声称它已经被收回。
            result.errorCode = reread.errorCode.isEmpty()
                ? QStringLiteral("extension-removal-grant-refresh-failed")
                : reread.errorCode;
        }
    }
    // 重新读到的字节才是依据:授权确实不在集合里,才算收回。读不出来的账本不算收回。
    result.grantRevoked = ledgerUsable(snapshot.grantState)
        && withoutGrants(snapshot.grants, kind, id).size()
            == snapshot.grants.size();
    if (!result.grantRevoked) {
        // 授权状态未知或授权仍在账本里。绝不能报成完成,也绝不继续去删复核记录:在授权
        // 未知的情况下往下走会留下更坏的中间态,而且抹掉的正是审计需要的证据。
        result.outcome = ExtensionLifecycleOutcome::PartiallyWithdrawn;
        result.reviewRevoked = false;
        result.snapshot = snapshot;
        if (result.errorCode.isEmpty()) {
            result.errorCode = QStringLiteral("extension-removal-grant-survived");
        }
        return result;
    }

    const QList<ExtensionReviewPin> remainingPins =
        withoutPins(snapshot.pins, kind, id);
    if (remainingPins.size() != snapshot.pins.size()) {
        ExtensionReviewLedgerStoreResult acknowledged;
        QString errorCode;
        if (!reviewStore->replace(remainingPins, snapshot.reviewGeneration,
                                  &acknowledged, &errorCode)) {
            // 授权已经收回但复核记录没有。这是安全的一侧,但必须可分辨,不能报成成功:
            // 人需要知道还有一条复核记录留着才会去清掉它。诊断记在这里,结论留给下面
            // 那一处判定,因此"两半都收回才算完成"只有一个来源。
            result.errorCode = errorCode.isEmpty()
                ? QStringLiteral("extension-removal-review-write-failed")
                : errorCode;
        } else {
            const ExtensionReviewLedgerStoreResult reread = reviewStore->load();
            snapshot.reviewState = reread.state;
            snapshot.reviewGeneration = reread.generation;
            snapshot.reviewErrorCode = reread.errorCode;
            snapshot.pins = ledgerUsable(reread.state)
                ? reread.pins
                : QList<ExtensionReviewPin>();
            if (!ledgerUsable(reread.state)) {
                result.errorCode = reread.errorCode.isEmpty()
                    ? QStringLiteral("extension-removal-review-refresh-failed")
                    : reread.errorCode;
            }
        }
    }
    // 读不出来的账本不算收回:一份状态未知的复核集合被当成空集合,会让界面显示"从未
    // 复核过",而实际情况是不知道。
    result.reviewRevoked = ledgerUsable(snapshot.reviewState)
        && withoutPins(snapshot.pins, kind, id).size() == snapshot.pins.size();

    // 两半都必须确实收回才报完成。任何一半没有收回都是部分完成。
    result.outcome = (result.grantRevoked && result.reviewRevoked)
        ? ExtensionLifecycleOutcome::Withdrawn
        : ExtensionLifecycleOutcome::PartiallyWithdrawn;
    if (result.outcome == ExtensionLifecycleOutcome::PartiallyWithdrawn
            && result.errorCode.isEmpty()) {
        result.errorCode = QStringLiteral("extension-removal-incomplete");
    }
    // 移除从不让任何东西变得可执行,也从不传递权威。
    result.candidateExecutable = false;
    result.inheritsTrust = false;
    result.inheritsGrant = false;
    result.snapshot = snapshot;
    return result;
}
