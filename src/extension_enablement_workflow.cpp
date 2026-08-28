#include "extension_enablement_workflow.h"

#include <QRegularExpression>

namespace {

// 与注册表、信任判定和启用判定保持一致：形式不合法的摘要不能进入授权集合。
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

ExtensionEnablementPlan reject(const QString &code)
{
    ExtensionEnablementPlan plan;
    plan.state = ExtensionEnablementPlanState::Rejected;
    plan.errorCode = code;
    return plan;
}

} // namespace

ExtensionEnablementPlan ExtensionEnablementWorkflow::plan(
    const ExtensionEnablementRequest &request,
    const QList<ExtensionRegistryRecord> &records,
    const ExtensionEnablementLedgerStoreResult &ledger)
{
    // 当前授权集合读不出来时不能规划：那会把一份不完整的集合当成完整集合提交，从而
    // 静默撤销读不出来的那些授权。撤销方向虽然安全，但它把一次篡改表述成用户主动
    // 停用，而且下一次授予会以错误的代号提交。
    if (ledger.state != ExtensionEnablementLedgerStoreState::Ready
            && ledger.state != ExtensionEnablementLedgerStoreState::Empty) {
        return reject(QStringLiteral("extension-enablement-ledger-unusable"));
    }
    if (ledger.state == ExtensionEnablementLedgerStoreState::Empty
            && (!ledger.grants.isEmpty() || ledger.generation != 0)) {
        return reject(QStringLiteral("extension-enablement-ledger-inconsistent"));
    }
    if (ledger.generation < 0
            || ledger.grants.size() > ExtensionEnablementPolicy::MaxGrants) {
        return reject(QStringLiteral("extension-enablement-ledger-inconsistent"));
    }
    if (!validId(request.id)) {
        return reject(QStringLiteral("extension-enablement-request-id-invalid"));
    }

    // 已存集合里任何一条授权不合法都必须整体拒绝。带着它提交会让整个集合通过认证，
    // 从而把一条无效授权洗成"已认证"的启用授权。
    for (const ExtensionEnablementGrant &grant : ledger.grants) {
        if (!validId(grant.id)
                || !wellFormed(grant.sourceIdentity, grant.contentIdentity)) {
            return reject(QStringLiteral("extension-enablement-ledger-grant-invalid"));
        }
    }
    // 同一 (kind, id) 出现多条时，启用判定已经会拒绝；这里同样不允许在冲突集合上
    // 规划，因为"删掉哪一条"没有正确答案。
    for (int i = 0; i < ledger.grants.size(); ++i) {
        for (int j = i + 1; j < ledger.grants.size(); ++j) {
            if (ledger.grants.at(i).kind == ledger.grants.at(j).kind
                    && ledger.grants.at(i).id == ledger.grants.at(j).id) {
                return reject(QStringLiteral("extension-enablement-ledger-conflict"));
            }
        }
    }

    ExtensionEnablementPlan plan;
    plan.expectedGeneration = ledger.generation;
    plan.grants = ledger.grants;

    if (request.action == ExtensionEnablementAction::Disable) {
        // 停用只依据 (kind, id)：内容已经变化、记录已经消失、甚至清单整体读不出来的
        // 情况下依然必须能停用，否则一个被篡改过的扩展将永远无法撤销其启用授权。
        QList<ExtensionEnablementGrant> remaining;
        for (const ExtensionEnablementGrant &grant : plan.grants) {
            if (grant.kind == request.kind && grant.id == request.id) continue;
            remaining.append(grant);
        }
        plan.changed = remaining.size() != plan.grants.size();
        plan.grants = remaining;
        plan.state = ExtensionEnablementPlanState::Ready;
        return plan;
    }

    // 授予必须对应一条当前确实存在、已安装、已复核、兼容、且摘要与人工看到的完全
    // 一致的记录。缺少任何一项都不能授予：授予一条不存在或尚未复核的记录等于预先
    // 授权将来出现的内容——授权会一直躺在账本里，等到条件凑齐的那一刻自动生效。
    if (!wellFormed(request.reviewedSourceIdentity,
                    request.reviewedContentIdentity)) {
        return reject(QStringLiteral("extension-enablement-request-identity-invalid"));
    }
    const ExtensionRegistryRecord *target = nullptr;
    int matches = 0;
    for (const ExtensionRegistryRecord &record : records) {
        if (record.kind != request.kind || record.id != request.id) continue;
        ++matches;
        target = &record;
    }
    if (matches == 0) {
        return reject(QStringLiteral("extension-enablement-target-absent"));
    }
    // 清单里出现同一 (kind, id) 的多条记录说明来源已经不可信，不能任选一条授予。
    if (matches > 1) {
        return reject(QStringLiteral("extension-enablement-target-ambiguous"));
    }
    if (!target->installed) {
        return reject(QStringLiteral("extension-enablement-target-not-installed"));
    }
    if (!validId(target->id)
            || !wellFormed(target->sourceIdentity, target->contentIdentity)) {
        return reject(QStringLiteral("extension-enablement-target-unverifiable"));
    }
    // 渲染与授予之间内容发生变化：人工看到的不是现在要授权运行的东西。这必须失败，
    // 而不是把决定套用到当前内容上。
    if (target->contentIdentity != request.reviewedContentIdentity) {
        return reject(QStringLiteral("extension-enablement-content-drift"));
    }
    if (target->sourceIdentity != request.reviewedSourceIdentity) {
        return reject(QStringLiteral("extension-enablement-source-drift"));
    }
    // 复核与兼容在规划时就必须成立。缺少这两道门时授予仍然不会启用任何东西（判定层
    // 独立拒绝），但一条已认证的授权会一直留在账本里，等到复核出现的那一刻自动生效。
    if (target->trust != ExtensionTrustState::Verified) {
        return reject(QStringLiteral("extension-enablement-trust-missing"));
    }
    if (target->compatibility != ExtensionCompatibilityState::Compatible) {
        return reject(QStringLiteral("extension-enablement-compatibility-missing"));
    }

    ExtensionEnablementGrant granted;
    granted.kind = request.kind;
    granted.id = request.id;
    granted.sourceIdentity = target->sourceIdentity;
    granted.contentIdentity = target->contentIdentity;

    bool replaced = false;
    for (ExtensionEnablementGrant &grant : plan.grants) {
        if (grant.kind != granted.kind || grant.id != granted.id) continue;
        // 同一扩展只保留一条授权：重新授予是替换，不是追加，否则集合会变成启用判定
        // 必须拒绝的冲突状态。
        if (grant.sourceIdentity != granted.sourceIdentity
                || grant.contentIdentity != granted.contentIdentity) {
            plan.changed = true;
        }
        grant = granted;
        replaced = true;
        break;
    }
    if (!replaced) {
        if (plan.grants.size() >= ExtensionEnablementPolicy::MaxGrants) {
            return reject(QStringLiteral("extension-enablement-grant-limit"));
        }
        plan.grants.append(granted);
        plan.changed = true;
    }
    plan.state = ExtensionEnablementPlanState::Ready;
    return plan;
}
