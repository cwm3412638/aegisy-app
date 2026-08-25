#include "extension_review_workflow.h"

#include "extension_trust_policy.h"

#include <QRegularExpression>

namespace {

// 与注册表和信任判定保持一致：形式不合法的摘要不能进入复核集合。
bool hashIdentity(const QString &value, const QString &prefix)
{
    return QRegularExpression(QStringLiteral("^%1[0-9a-f]{64}$")
        .arg(QRegularExpression::escape(prefix))).match(value).hasMatch();
}

bool wellFormed(const QString &sourceIdentity, const QString &contentIdentity)
{
    return hashIdentity(sourceIdentity, QStringLiteral("extension-source:sha256:"))
        && hashIdentity(contentIdentity, QStringLiteral("extension-content:sha256:"));
}

bool validId(const QString &value)
{
    return QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
        .match(value).hasMatch();
}

ExtensionReviewPlan reject(const QString &code)
{
    ExtensionReviewPlan plan;
    plan.state = ExtensionReviewPlanState::Rejected;
    plan.errorCode = code;
    return plan;
}

} // namespace

ExtensionReviewPlan ExtensionReviewWorkflow::plan(
    const ExtensionReviewRequest &request,
    const QList<ExtensionRegistryRecord> &records,
    const ExtensionReviewLedgerStoreResult &ledger)
{
    // 当前复核集合读不出来时不能规划：那会把一份不完整的集合当成完整集合提交，
    // 从而静默删除读不出来的那些复核记录。
    if (ledger.state != ExtensionReviewLedgerStoreState::Ready
            && ledger.state != ExtensionReviewLedgerStoreState::Empty) {
        return reject(QStringLiteral("extension-review-ledger-unusable"));
    }
    if (ledger.state == ExtensionReviewLedgerStoreState::Empty
            && (!ledger.pins.isEmpty() || ledger.generation != 0)) {
        return reject(QStringLiteral("extension-review-ledger-inconsistent"));
    }
    if (ledger.generation < 0 || ledger.pins.size() > ExtensionTrustPolicy::MaxReviewPins) {
        return reject(QStringLiteral("extension-review-ledger-inconsistent"));
    }
    if (!validId(request.id)) {
        return reject(QStringLiteral("extension-review-request-id-invalid"));
    }

    // 已存集合里任何一条记录不合法都必须整体拒绝。带着它提交会让整个集合通过认证，
    // 从而把一条无效记录洗成"已认证"的复核证据。
    for (const ExtensionReviewPin &pin : ledger.pins) {
        if (!validId(pin.id) || !wellFormed(pin.sourceIdentity, pin.contentIdentity)) {
            return reject(QStringLiteral("extension-review-ledger-pin-invalid"));
        }
    }
    // 同一 (kind, id) 出现多条时，信任判定已经会拒绝；这里同样不允许在冲突集合上
    // 规划，因为"删掉哪一条"没有正确答案。
    for (int i = 0; i < ledger.pins.size(); ++i) {
        for (int j = i + 1; j < ledger.pins.size(); ++j) {
            if (ledger.pins.at(i).kind == ledger.pins.at(j).kind
                    && ledger.pins.at(i).id == ledger.pins.at(j).id) {
                return reject(QStringLiteral("extension-review-ledger-conflict"));
            }
        }
    }

    ExtensionReviewPlan plan;
    plan.expectedGeneration = ledger.generation;
    plan.pins = ledger.pins;

    if (request.action == ExtensionReviewAction::Revoke) {
        // 撤销只依据 (kind, id)：内容已经变化的情况下依然必须能撤销，否则一个被
        // 篡改过的扩展将永远无法从集合里移除。
        QList<ExtensionReviewPin> remaining;
        for (const ExtensionReviewPin &pin : plan.pins) {
            if (pin.kind == request.kind && pin.id == request.id) continue;
            remaining.append(pin);
        }
        plan.changed = remaining.size() != plan.pins.size();
        plan.pins = remaining;
        plan.state = ExtensionReviewPlanState::Ready;
        return plan;
    }

    // 批准必须对应一条当前确实存在、已安装、且摘要与人工复核时完全一致的记录。
    // 缺少任何一项都不能批准：批准一条不存在的记录等于预先授权将来出现的内容。
    if (!wellFormed(request.reviewedSourceIdentity,
                    request.reviewedContentIdentity)) {
        return reject(QStringLiteral("extension-review-request-identity-invalid"));
    }
    const ExtensionRegistryRecord *target = nullptr;
    int matches = 0;
    for (const ExtensionRegistryRecord &record : records) {
        if (record.kind != request.kind || record.id != request.id) continue;
        ++matches;
        target = &record;
    }
    if (matches == 0) return reject(QStringLiteral("extension-review-target-absent"));
    // 清单里出现同一 (kind, id) 的多条记录说明来源已经不可信，不能任选一条批准。
    if (matches > 1) return reject(QStringLiteral("extension-review-target-ambiguous"));
    if (!target->installed) {
        return reject(QStringLiteral("extension-review-target-not-installed"));
    }
    if (!validId(target->id)
            || !wellFormed(target->sourceIdentity, target->contentIdentity)) {
        return reject(QStringLiteral("extension-review-target-unverifiable"));
    }
    // 渲染与批准之间内容发生变化：人工看到的不是现在要批准的东西。这必须失败，
    // 而不是把批准套用到当前内容上。
    if (target->contentIdentity != request.reviewedContentIdentity) {
        return reject(QStringLiteral("extension-review-content-drift"));
    }
    if (target->sourceIdentity != request.reviewedSourceIdentity) {
        return reject(QStringLiteral("extension-review-source-drift"));
    }

    ExtensionReviewPin approved;
    approved.kind = request.kind;
    approved.id = request.id;
    approved.sourceIdentity = target->sourceIdentity;
    approved.contentIdentity = target->contentIdentity;

    bool replaced = false;
    for (ExtensionReviewPin &pin : plan.pins) {
        if (pin.kind != approved.kind || pin.id != approved.id) continue;
        // 同一扩展只保留一条复核记录：重新批准是替换，不是追加，否则集合会变成
        // 信任判定必须拒绝的冲突状态。
        if (pin.sourceIdentity != approved.sourceIdentity
                || pin.contentIdentity != approved.contentIdentity) {
            plan.changed = true;
        }
        pin = approved;
        replaced = true;
        break;
    }
    if (!replaced) {
        if (plan.pins.size() >= ExtensionTrustPolicy::MaxReviewPins) {
            return reject(QStringLiteral("extension-review-pin-limit"));
        }
        plan.pins.append(approved);
        plan.changed = true;
    }
    plan.state = ExtensionReviewPlanState::Ready;
    return plan;
}
