#include "extension_trust_policy.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QTextStream>

namespace {

int failures = 0;

bool expect(bool condition, const char *message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

QString digest(const QString &prefix, const QByteArray &seed)
{
    return prefix + QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex());
}

QString sourceOf(const QByteArray &seed)
{
    return digest(QStringLiteral("extension-source:sha256:"), seed);
}

QString contentOf(const QByteArray &seed)
{
    return digest(QStringLiteral("extension-content:sha256:"), seed);
}

ExtensionRegistryRecord record(ExtensionKind kind, const QString &id,
                               const QByteArray &sourceSeed,
                               const QByteArray &contentSeed)
{
    ExtensionRegistryRecord value;
    value.kind = kind;
    value.id = id;
    value.name = QStringLiteral("Sample");
    value.version = QStringLiteral("1.0.0");
    value.sourceIdentity = sourceOf(sourceSeed);
    value.contentIdentity = contentOf(contentSeed);
    value.scope = QStringLiteral("user");
    value.installed = true;
    return value;
}

ExtensionReviewPin pinFor(const ExtensionRegistryRecord &value)
{
    ExtensionReviewPin pin;
    pin.kind = value.kind;
    pin.id = value.id;
    pin.sourceIdentity = value.sourceIdentity;
    pin.contentIdentity = value.contentIdentity;
    return pin;
}

bool verified(const ExtensionTrustDecision &decision)
{
    return decision.state == ExtensionTrustState::Verified
        && decision.evidence == ExtensionTrustEvidence::ReviewMatched
        && decision.reason.isEmpty();
}

bool rejected(const ExtensionTrustDecision &decision,
              ExtensionTrustEvidence evidence, const QString &reason)
{
    return decision.state == ExtensionTrustState::Unverified
        && decision.evidence == evidence && decision.reason == reason;
}

void baselineTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"),
               "source-a", "content-a");

    // 没有任何复核记录时不能授予信任。
    expect(rejected(ExtensionTrustPolicy::evaluate(skill, {}),
                    ExtensionTrustEvidence::Unreviewed,
                    QStringLiteral("extension-not-reviewed")),
           "an unreviewed extension was trusted");

    // 针对确切内容的复核记录是唯一的信任来源。
    expect(verified(ExtensionTrustPolicy::evaluate(skill, {pinFor(skill)})),
           "an exactly reviewed extension was not trusted");

    // 无关记录不能顺带授予信任。
    const ExtensionRegistryRecord other =
        record(ExtensionKind::Skill, QStringLiteral("other.skill"),
               "source-b", "content-b");
    expect(rejected(ExtensionTrustPolicy::evaluate(skill, {pinFor(other)}),
                    ExtensionTrustEvidence::Unreviewed,
                    QStringLiteral("extension-not-reviewed")),
           "an unrelated review pin granted trust");

    // 同一 ID 的不同种类是不同的扩展，复核不能跨种类延续。
    ExtensionReviewPin crossKind = pinFor(skill);
    crossKind.kind = ExtensionKind::CodexPlugin;
    expect(rejected(ExtensionTrustPolicy::evaluate(skill, {crossKind}),
                    ExtensionTrustEvidence::Unreviewed,
                    QStringLiteral("extension-not-reviewed")),
           "a review pin crossed the extension kind boundary");

    // 复核记录在列表中的位置不影响结果。
    expect(verified(ExtensionTrustPolicy::evaluate(
               skill, {pinFor(other), pinFor(skill)})),
           "review matching depends on pin ordering");
}

void driftTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"),
               "source-a", "content-a");

    // 内容变化让复核结论失效：被复核的是内容，不是名字。
    ExtensionReviewPin staleContent = pinFor(skill);
    staleContent.contentIdentity = contentOf("content-old");
    expect(rejected(ExtensionTrustPolicy::evaluate(skill, {staleContent}),
                    ExtensionTrustEvidence::ContentDrifted,
                    QStringLiteral("extension-review-content-drift")),
           "content drift did not revoke trust");

    // 来源变化同样让复核结论失效，即使内容一致。
    ExtensionReviewPin staleSource = pinFor(skill);
    staleSource.sourceIdentity = sourceOf("source-old");
    expect(rejected(ExtensionTrustPolicy::evaluate(skill, {staleSource}),
                    ExtensionTrustEvidence::SourceDrifted,
                    QStringLiteral("extension-review-source-drift")),
           "source drift did not revoke trust");

    // 两者同时变化时报告内容漂移：内容已经不同，来源差异不再需要区分。
    ExtensionReviewPin bothStale = pinFor(skill);
    bothStale.sourceIdentity = sourceOf("source-old");
    bothStale.contentIdentity = contentOf("content-old");
    expect(rejected(ExtensionTrustPolicy::evaluate(skill, {bothStale}),
                    ExtensionTrustEvidence::ContentDrifted,
                    QStringLiteral("extension-review-content-drift")),
           "combined drift was not reported as content drift");
}

void conflictTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"),
               "source-a", "content-a");

    // 追加一条匹配记录不能覆盖已有的冲突：任取一条会让攻击者只需追加即可通过。
    ExtensionReviewPin stale = pinFor(skill);
    stale.contentIdentity = contentOf("content-old");
    expect(rejected(ExtensionTrustPolicy::evaluate(skill, {stale, pinFor(skill)}),
                    ExtensionTrustEvidence::ReviewConflict,
                    QStringLiteral("extension-review-conflict")),
           "an appended matching pin overrode a conflicting review");
    expect(rejected(ExtensionTrustPolicy::evaluate(skill, {pinFor(skill), stale}),
                    ExtensionTrustEvidence::ReviewConflict,
                    QStringLiteral("extension-review-conflict")),
           "review conflict detection depends on ordering");

    // 完全相同的重复记录同样是冲突：存储里不该出现两条，出现即不可信。
    expect(rejected(ExtensionTrustPolicy::evaluate(
                        skill, {pinFor(skill), pinFor(skill)}),
                    ExtensionTrustEvidence::ReviewConflict,
                    QStringLiteral("extension-review-conflict")),
           "duplicate review pins were accepted");
}

void malformedTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"),
               "source-a", "content-a");

    // 复核记录本身不合法时不能作为证据，且不能降级成"未复核"后继续匹配别的记录。
    const QStringList badIdentities{
        QString(), QStringLiteral("extension-content:sha256:abc"),
        QStringLiteral("extension-source:sha256:") + QString(64, QLatin1Char('z')),
        QStringLiteral("sha256:") + QString(64, QLatin1Char('a'))};
    for (const QString &bad : badIdentities) {
        ExtensionReviewPin pin = pinFor(skill);
        pin.contentIdentity = bad;
        expect(rejected(ExtensionTrustPolicy::evaluate(skill, {pin}),
                        ExtensionTrustEvidence::ReviewMalformed,
                        QStringLiteral("extension-review-pin-malformed")),
               "a malformed review pin was not rejected");
        // 即使同时存在一条合法匹配，非法记录也必须让整批证据失效。
        expect(rejected(ExtensionTrustPolicy::evaluate(skill, {pin, pinFor(skill)}),
                        ExtensionTrustEvidence::ReviewMalformed,
                        QStringLiteral("extension-review-pin-malformed")),
               "a malformed pin was skipped instead of failing closed");
    }
    ExtensionReviewPin badId = pinFor(skill);
    badId.id = QStringLiteral("Fixture.Skill");
    expect(rejected(ExtensionTrustPolicy::evaluate(skill, {badId}),
                    ExtensionTrustEvidence::ReviewMalformed,
                    QStringLiteral("extension-review-pin-malformed")),
           "a review pin with an invalid ID was accepted");

    // 记录自身不可核查时同样拒绝：摘要非法的记录可能匹配到任何东西。
    for (const QString &bad : badIdentities) {
        ExtensionRegistryRecord broken = skill;
        broken.sourceIdentity = bad;
        expect(rejected(ExtensionTrustPolicy::evaluate(broken, {pinFor(broken)}),
                        ExtensionTrustEvidence::ReviewMalformed,
                        QStringLiteral("extension-record-unverifiable")),
               "an unverifiable record was matched against a review pin");
    }
    ExtensionRegistryRecord brokenId = skill;
    brokenId.id = QStringLiteral("-leading-dash");
    expect(rejected(ExtensionTrustPolicy::evaluate(brokenId, {pinFor(brokenId)}),
                    ExtensionTrustEvidence::ReviewMalformed,
                    QStringLiteral("extension-record-unverifiable")),
           "a record with an invalid ID was trusted");

    // 复核存储超限说明存储已经不可信，整体拒绝而不是截断后继续匹配。
    QList<ExtensionReviewPin> oversized;
    for (int index = 0; index <= ExtensionTrustPolicy::MaxReviewPins; ++index) {
        ExtensionReviewPin pin = pinFor(skill);
        pin.id = QStringLiteral("filler.%1").arg(index);
        oversized.append(pin);
    }
    oversized.append(pinFor(skill));
    expect(rejected(ExtensionTrustPolicy::evaluate(skill, oversized),
                    ExtensionTrustEvidence::ReviewMalformed,
                    QStringLiteral("extension-review-store-oversized")),
           "an oversized review store was still consulted");

    // 恰好达到上限仍然可用，边界不能提前失效。
    QList<ExtensionReviewPin> atLimit;
    for (int index = 0; index < ExtensionTrustPolicy::MaxReviewPins - 1; ++index) {
        ExtensionReviewPin pin = pinFor(skill);
        pin.id = QStringLiteral("filler.%1").arg(index);
        atLimit.append(pin);
    }
    atLimit.append(pinFor(skill));
    expect(verified(ExtensionTrustPolicy::evaluate(skill, atLimit)),
           "a review store at the exact limit was rejected");
}

void applyTests()
{
    const ExtensionRegistryRecord reviewed =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"),
               "source-a", "content-a");
    QList<ExtensionRegistryRecord> records{
        reviewed,
        record(ExtensionKind::Mcp, QStringLiteral("fixture.mcp"),
               "source-b", "content-b"),
    };
    // 陈旧的 Verified 必须被重新判定推翻。
    records[1].trust = ExtensionTrustState::Verified;
    records[0].compatibility = ExtensionCompatibilityState::Compatible;
    records[1].compatibility = ExtensionCompatibilityState::Incompatible;
    records[1].compatibilityReason = QStringLiteral("extension-capability-not-granted");

    ExtensionTrustPolicy::apply(&records, {pinFor(reviewed)});

    expect(records[0].trust == ExtensionTrustState::Verified,
           "apply did not trust a reviewed record");
    expect(records[1].trust == ExtensionTrustState::Unverified,
           "apply did not revoke a stale verified record");

    // 授予信任不等于授权启用，也不改变兼容性判定。
    expect(!records[0].effectiveEnabled && !records[1].effectiveEnabled,
           "apply enabled an extension");
    expect(records[0].compatibility == ExtensionCompatibilityState::Compatible
               && records[1].compatibility == ExtensionCompatibilityState::Incompatible
               && records[1].compatibilityReason
                   == QStringLiteral("extension-capability-not-granted"),
           "apply changed a compatibility verdict");
    expect(!records[0].updateAvailable && !records[0].recoveryAvailable,
           "apply asserted update or recovery authority");
    expect(records[0].id == QStringLiteral("fixture.skill")
               && records[0].contentIdentity == reviewed.contentIdentity,
           "apply mutated source-reported facts");

    ExtensionTrustPolicy::apply(nullptr, {});
    QList<ExtensionRegistryRecord> empty;
    ExtensionTrustPolicy::apply(&empty, {pinFor(reviewed)});
    expect(empty.isEmpty(), "apply invented records");
}

void gateAgreementTests()
{
    // 双门禁：Verified 与 Compatible 同时具备才允许启用，任缺一项都不允许。
    const ExtensionRegistryRecord reviewed =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"),
               "source-a", "content-a");
    QList<ExtensionRegistryRecord> records{reviewed};
    ExtensionTrustPolicy::apply(&records, {pinFor(reviewed)});
    records[0].compatibility = ExtensionCompatibilityState::Compatible;
    records[0].compatibilityReason.clear();

    ExtensionRegistryProjection projection;
    QString error;
    expect(ExtensionRegistry::build(records, &projection, &error),
           "a verified compatible record failed registry validation");

    // 判定齐备也不会自动启用：启用仍然需要一个独立动作。
    expect(!records[0].effectiveEnabled,
           "a verified compatible record enabled itself");

    // 撤销信任后同一条记录不再可启用。
    records[0].trust = ExtensionTrustState::Unverified;
    records[0].effectiveEnabled = true;
    expect(!ExtensionRegistry::build(records, &projection, &error),
           "enablement survived trust revocation");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    baselineTests();
    driftTests();
    conflictTests();
    malformedTests();
    applyTests();
    gateAgreementTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension trust policy tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
