#include "extension_trust_policy.h"

#include <QRegularExpression>

namespace {

// 与 extension_registry 的校验保持一致；不合法的摘要不能作为复核证据。
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

ExtensionTrustDecision verdict(ExtensionTrustState state,
                               ExtensionTrustEvidence evidence,
                               const QString &reason)
{
    ExtensionTrustDecision decision;
    decision.state = state;
    decision.evidence = evidence;
    decision.reason = reason;
    return decision;
}

ExtensionTrustDecision unverified(ExtensionTrustEvidence evidence,
                                  const QString &reason)
{
    return verdict(ExtensionTrustState::Unverified, evidence, reason);
}

} // namespace

ExtensionTrustDecision ExtensionTrustPolicy::evaluate(
    const ExtensionRegistryRecord &record,
    const QList<ExtensionReviewPin> &pins)
{
    // 记录本身不可核查时不能进入匹配：一条摘要非法的记录可能匹配到任何东西。
    if (!validId(record.id)
            || !wellFormed(record.sourceIdentity, record.contentIdentity)) {
        return unverified(ExtensionTrustEvidence::ReviewMalformed,
                          QStringLiteral("extension-record-unverifiable"));
    }
    // 复核记录过多说明存储已经不可信，整体拒绝而不是截断后继续匹配。
    if (pins.size() > MaxReviewPins) {
        return unverified(ExtensionTrustEvidence::ReviewMalformed,
                          QStringLiteral("extension-review-store-oversized"));
    }

    // 同一 (kind, id) 只允许一条复核记录。存在多条时无法判断哪一条有效，
    // 因此判定冲突而不是任取一条匹配——任取会让攻击者只需追加一条记录就通过。
    const ExtensionReviewPin *match = nullptr;
    int candidates = 0;
    for (const ExtensionReviewPin &pin : pins) {
        if (!validId(pin.id)
                || !wellFormed(pin.sourceIdentity, pin.contentIdentity)) {
            return unverified(ExtensionTrustEvidence::ReviewMalformed,
                              QStringLiteral("extension-review-pin-malformed"));
        }
        if (pin.kind != record.kind || pin.id != record.id) continue;
        ++candidates;
        match = &pin;
    }
    if (candidates > 1) {
        return unverified(ExtensionTrustEvidence::ReviewConflict,
                          QStringLiteral("extension-review-conflict"));
    }
    if (candidates == 0) {
        return unverified(ExtensionTrustEvidence::Unreviewed,
                          QStringLiteral("extension-not-reviewed"));
    }

    // 内容漂移优先于来源漂移：被复核的是内容，内容变了就无需再区分来源。
    if (match->contentIdentity != record.contentIdentity) {
        return unverified(ExtensionTrustEvidence::ContentDrifted,
                          QStringLiteral("extension-review-content-drift"));
    }
    if (match->sourceIdentity != record.sourceIdentity) {
        return unverified(ExtensionTrustEvidence::SourceDrifted,
                          QStringLiteral("extension-review-source-drift"));
    }
    return verdict(ExtensionTrustState::Verified,
                   ExtensionTrustEvidence::ReviewMatched, QString());
}

void ExtensionTrustPolicy::apply(QList<ExtensionRegistryRecord> *records,
                                 const QList<ExtensionReviewPin> &pins)
{
    if (!records) return;
    for (ExtensionRegistryRecord &record : *records) {
        // 只写信任状态。兼容性与生效启用状态一概不动：Verified 只是启用门禁的
        // 一半，启用仍然需要一个独立动作。
        record.trust = evaluate(record, pins).state;
    }
}
