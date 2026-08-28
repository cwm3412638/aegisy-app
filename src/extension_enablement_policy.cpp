#include "extension_enablement_policy.h"

#include <QRegularExpression>

namespace {

// 与注册表、信任与复核流程保持一致；形式不合法的摘要不能作为启用依据。
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

ExtensionEnablementDecision refused(ExtensionEnablementEvidence evidence,
                                    const QString &reason)
{
    ExtensionEnablementDecision decision;
    decision.enabled = false;
    decision.evidence = evidence;
    decision.reason = reason;
    return decision;
}

} // namespace

ExtensionEnablementDecision ExtensionEnablementPolicy::evaluate(
    const ExtensionRegistryRecord &record,
    const QList<ExtensionEnablementGrant> &grants)
{
    // 记录本身不可核查时不能进入匹配：一条摘要非法的记录可能匹配到任何东西。
    if (!validId(record.id)
            || !wellFormed(record.sourceIdentity, record.contentIdentity)) {
        return refused(ExtensionEnablementEvidence::GrantMalformed,
                       QStringLiteral("extension-record-unverifiable"));
    }
    // 授权过多说明存储已经不可信，整体拒绝而不是截断后继续匹配。
    if (grants.size() > MaxGrants) {
        return refused(ExtensionEnablementEvidence::GrantMalformed,
                       QStringLiteral("extension-enablement-store-oversized"));
    }

    // 同一 (kind, id) 只允许一条启用授权。存在多条时无法判断哪一条有效，因此判定
    // 冲突而不是任取一条匹配——任取会让攻击者只需追加一条授权就通过。
    const ExtensionEnablementGrant *match = nullptr;
    int candidates = 0;
    for (const ExtensionEnablementGrant &grant : grants) {
        if (!validId(grant.id)
                || !wellFormed(grant.sourceIdentity, grant.contentIdentity)) {
            return refused(ExtensionEnablementEvidence::GrantMalformed,
                           QStringLiteral("extension-enablement-grant-malformed"));
        }
        if (grant.kind != record.kind || grant.id != record.id) continue;
        ++candidates;
        match = &grant;
    }
    if (candidates > 1) {
        return refused(ExtensionEnablementEvidence::GrantConflict,
                       QStringLiteral("extension-enablement-conflict"));
    }
    if (candidates == 0) {
        return refused(ExtensionEnablementEvidence::NotGranted,
                       QStringLiteral("extension-not-enabled"));
    }

    // 内容漂移优先于来源漂移：被授权启用的是内容，内容变了就无需再区分来源。
    if (match->contentIdentity != record.contentIdentity) {
        return refused(ExtensionEnablementEvidence::ContentDrifted,
                       QStringLiteral("extension-enablement-content-drift"));
    }
    if (match->sourceIdentity != record.sourceIdentity) {
        return refused(ExtensionEnablementEvidence::SourceDrifted,
                       QStringLiteral("extension-enablement-source-drift"));
    }

    // 授权存在也不能绕过其余门禁。这三项在授权匹配之后判定，因此一条已经消失、
    // 未复核或不兼容的扩展带着有效授权依然得不到启用，而诊断代码仍能说明是哪一项
    // 缺失。注册表随后会独立地再要求一次 Verified + Compatible。
    if (!record.installed) {
        return refused(ExtensionEnablementEvidence::NotInstalled,
                       QStringLiteral("extension-enablement-target-not-installed"));
    }
    if (record.trust != ExtensionTrustState::Verified) {
        return refused(ExtensionEnablementEvidence::TrustMissing,
                       QStringLiteral("extension-enablement-trust-missing"));
    }
    if (record.compatibility != ExtensionCompatibilityState::Compatible) {
        return refused(ExtensionEnablementEvidence::CompatibilityMissing,
                       QStringLiteral("extension-enablement-compatibility-missing"));
    }

    ExtensionEnablementDecision decision;
    decision.enabled = true;
    decision.evidence = ExtensionEnablementEvidence::GrantMatched;
    return decision;
}

void ExtensionEnablementPolicy::apply(
    QList<ExtensionRegistryRecord> *records,
    const QList<ExtensionEnablementGrant> &grants)
{
    if (!records) return;
    for (ExtensionRegistryRecord &record : *records) {
        // 只写生效启用状态。信任与兼容性判定一概不动，两者仍然是注册表独立要求的
        // 门禁；这一层写入的值随后还要通过那道门禁才会被接受。
        record.effectiveEnabled = evaluate(record, grants).enabled;
    }
}
