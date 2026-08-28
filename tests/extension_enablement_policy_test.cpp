#include "extension_enablement_policy.h"

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

// 一条其余门禁全部齐备的记录：这样每个用例只改动它要检验的那一项。
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
    value.trust = ExtensionTrustState::Verified;
    value.compatibility = ExtensionCompatibilityState::Compatible;
    return value;
}

ExtensionEnablementGrant grantFor(const ExtensionRegistryRecord &value)
{
    ExtensionEnablementGrant grant;
    grant.kind = value.kind;
    grant.id = value.id;
    grant.sourceIdentity = value.sourceIdentity;
    grant.contentIdentity = value.contentIdentity;
    return grant;
}

bool enabled(const ExtensionEnablementDecision &decision)
{
    return decision.enabled
        && decision.evidence == ExtensionEnablementEvidence::GrantMatched
        && decision.reason.isEmpty();
}

bool refused(const ExtensionEnablementDecision &decision,
             ExtensionEnablementEvidence evidence, const QString &reason)
{
    return !decision.enabled && decision.evidence == evidence
        && decision.reason == reason;
}

void baselineTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("alpha"), "src-a", "content-a");

    // 没有任何授权时不启用：复核与兼容齐备也仍然需要一个独立的启用动作。
    expect(refused(ExtensionEnablementPolicy::evaluate(skill, {}),
                   ExtensionEnablementEvidence::NotGranted,
                   QStringLiteral("extension-not-enabled")),
           "an unrequested extension became enabled");

    expect(enabled(ExtensionEnablementPolicy::evaluate(skill, {grantFor(skill)})),
           "an exactly granted verified compatible extension was not enabled");

    // 授权绑定种类与 ID 两者：只匹配 ID 会让一个 Skill 的授权启用同名的 MCP 条目。
    ExtensionEnablementGrant otherKind = grantFor(skill);
    otherKind.kind = ExtensionKind::Mcp;
    expect(refused(ExtensionEnablementPolicy::evaluate(skill, {otherKind}),
                   ExtensionEnablementEvidence::NotGranted,
                   QStringLiteral("extension-not-enabled")),
           "an enablement grant crossed extension kinds");

    ExtensionEnablementGrant otherId = grantFor(skill);
    otherId.id = QStringLiteral("beta");
    expect(refused(ExtensionEnablementPolicy::evaluate(skill, {otherId}),
                   ExtensionEnablementEvidence::NotGranted,
                   QStringLiteral("extension-not-enabled")),
           "an enablement grant crossed extension identifiers");

    // 无关扩展的授权共存时不影响本条判定。
    const ExtensionRegistryRecord mcp =
        record(ExtensionKind::Mcp, QStringLiteral("beta"), "src-b", "content-b");
    expect(enabled(ExtensionEnablementPolicy::evaluate(
               skill, {grantFor(mcp), grantFor(skill)})),
           "an unrelated grant blocked a valid enablement");
}

void driftTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("alpha"), "src-a", "content-a");

    // 内容被替换后授权不能延续：继承前一份内容的启用授权正是这一层要防住的事情。
    ExtensionEnablementGrant staleContent = grantFor(skill);
    staleContent.contentIdentity = contentOf("content-old");
    expect(refused(ExtensionEnablementPolicy::evaluate(skill, {staleContent}),
                   ExtensionEnablementEvidence::ContentDrifted,
                   QStringLiteral("extension-enablement-content-drift")),
           "enablement survived extension content drift");

    // 同名同内容换了来源同样不能延续。
    ExtensionEnablementGrant staleSource = grantFor(skill);
    staleSource.sourceIdentity = sourceOf("src-old");
    expect(refused(ExtensionEnablementPolicy::evaluate(skill, {staleSource}),
                   ExtensionEnablementEvidence::SourceDrifted,
                   QStringLiteral("extension-enablement-source-drift")),
           "enablement survived extension source drift");

    // 两项同时漂移时内容优先报告：内容变了就无需再区分来源。
    ExtensionEnablementGrant bothStale = grantFor(skill);
    bothStale.contentIdentity = contentOf("content-old");
    bothStale.sourceIdentity = sourceOf("src-old");
    expect(refused(ExtensionEnablementPolicy::evaluate(skill, {bothStale}),
                   ExtensionEnablementEvidence::ContentDrifted,
                   QStringLiteral("extension-enablement-content-drift")),
           "content drift did not dominate source drift");
}

void conflictTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("alpha"), "src-a", "content-a");

    // 同一 (kind, id) 出现两条授权时判定冲突，而不是任取一条匹配的：任取会让
    // 追加一条授权就足以启用。两种顺序都必须拒绝。
    ExtensionEnablementGrant stale = grantFor(skill);
    stale.contentIdentity = contentOf("content-old");
    expect(refused(ExtensionEnablementPolicy::evaluate(
                       skill, {grantFor(skill), stale}),
                   ExtensionEnablementEvidence::GrantConflict,
                   QStringLiteral("extension-enablement-conflict")),
           "a matching grant was selected out of a conflicting set");
    expect(refused(ExtensionEnablementPolicy::evaluate(
                       skill, {stale, grantFor(skill)}),
                   ExtensionEnablementEvidence::GrantConflict,
                   QStringLiteral("extension-enablement-conflict")),
           "grant conflict depended on ordering");

    // 完全相同的两条授权同样是冲突：重复说明存储已经不再可信。
    expect(refused(ExtensionEnablementPolicy::evaluate(
                       skill, {grantFor(skill), grantFor(skill)}),
                   ExtensionEnablementEvidence::GrantConflict,
                   QStringLiteral("extension-enablement-conflict")),
           "duplicate identical grants were accepted");
}

void malformedTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("alpha"), "src-a", "content-a");

    // 记录本身不可核查时在匹配之前就拒绝：摘要非法的记录可能匹配到任何东西。
    ExtensionRegistryRecord badId = skill;
    badId.id = QStringLiteral("Alpha");
    expect(refused(ExtensionEnablementPolicy::evaluate(badId, {grantFor(badId)}),
                   ExtensionEnablementEvidence::GrantMalformed,
                   QStringLiteral("extension-record-unverifiable")),
           "a record with an invalid identifier was matched");

    ExtensionRegistryRecord badDigest = skill;
    badDigest.contentIdentity = QStringLiteral("extension-content:sha256:zz");
    expect(refused(ExtensionEnablementPolicy::evaluate(
                       badDigest, {grantFor(badDigest)}),
                   ExtensionEnablementEvidence::GrantMalformed,
                   QStringLiteral("extension-record-unverifiable")),
           "a record with a malformed digest was matched");

    // 一条不合法的授权让整体判定失败，而不是被跳过：跳过会让一个不合法的条目
    // 与一条有效条目共存时仍然启用，从而把不合法内容洗成可用授权。
    ExtensionEnablementGrant malformed = grantFor(skill);
    malformed.sourceIdentity = QStringLiteral("extension-source:sha256:nope");
    malformed.id = QStringLiteral("gamma");
    expect(refused(ExtensionEnablementPolicy::evaluate(
                       skill, {malformed, grantFor(skill)}),
                   ExtensionEnablementEvidence::GrantMalformed,
                   QStringLiteral("extension-enablement-grant-malformed")),
           "a malformed unrelated grant was skipped instead of failing");

    // 授权数量超过上限时整体拒绝而不是截断：截断后继续匹配等于接受一个已经
    // 不可信的存储。
    QList<ExtensionEnablementGrant> oversized;
    for (int i = 0; i <= ExtensionEnablementPolicy::MaxGrants; ++i) {
        ExtensionEnablementGrant grant;
        grant.kind = ExtensionKind::Skill;
        grant.id = QStringLiteral("id-%1").arg(i);
        grant.sourceIdentity = sourceOf(QByteArray::number(i));
        grant.contentIdentity = contentOf(QByteArray::number(i));
        oversized.append(grant);
    }
    oversized.append(grantFor(skill));
    expect(refused(ExtensionEnablementPolicy::evaluate(skill, oversized),
                   ExtensionEnablementEvidence::GrantMalformed,
                   QStringLiteral("extension-enablement-store-oversized")),
           "an oversized grant store was truncated instead of rejected");
}

void otherGateTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("alpha"), "src-a", "content-a");
    const ExtensionEnablementGrant grant = grantFor(skill);

    // 一条有效授权不能绕过其余门禁。扩展消失后授权不再生效：启用一个不存在的
    // 东西等于预先授权将来出现在这个位置的内容。
    ExtensionRegistryRecord uninstalled = skill;
    uninstalled.installed = false;
    expect(refused(ExtensionEnablementPolicy::evaluate(uninstalled, {grant}),
                   ExtensionEnablementEvidence::NotInstalled,
                   QStringLiteral("extension-enablement-target-not-installed")),
           "an uninstalled extension was enabled by a stale grant");

    // 未复核的内容不能被启用，即便用户曾经授权过。
    ExtensionRegistryRecord unverified = skill;
    unverified.trust = ExtensionTrustState::Unverified;
    expect(refused(ExtensionEnablementPolicy::evaluate(unverified, {grant}),
                   ExtensionEnablementEvidence::TrustMissing,
                   QStringLiteral("extension-enablement-trust-missing")),
           "an unreviewed extension was enabled by a grant");

    // 兼容性未确立与明确不兼容都不能启用；请求写入或执行能力的扩展在只读授权下
    // 落在后者，因此永远无法通过这一层。
    ExtensionRegistryRecord unknown = skill;
    unknown.compatibility = ExtensionCompatibilityState::Unknown;
    unknown.compatibilityReason = QStringLiteral("codex-plugin-host-version-unknown");
    expect(refused(ExtensionEnablementPolicy::evaluate(unknown, {grant}),
                   ExtensionEnablementEvidence::CompatibilityMissing,
                   QStringLiteral("extension-enablement-compatibility-missing")),
           "an extension with unknown compatibility was enabled");

    ExtensionRegistryRecord incompatible = skill;
    incompatible.compatibility = ExtensionCompatibilityState::Incompatible;
    incompatible.compatibilityReason =
        QStringLiteral("codex-plugin-capability-not-granted");
    expect(refused(ExtensionEnablementPolicy::evaluate(incompatible, {grant}),
                   ExtensionEnablementEvidence::CompatibilityMissing,
                   QStringLiteral("extension-enablement-compatibility-missing")),
           "an incompatible extension was enabled");

    // 授权匹配先于其余门禁判定：没有授权时报告的仍然是"未被要求启用"，因此
    // 诊断不会把缺失授权说成缺失复核。
    expect(refused(ExtensionEnablementPolicy::evaluate(unverified, {}),
                   ExtensionEnablementEvidence::NotGranted,
                   QStringLiteral("extension-not-enabled")),
           "a missing grant was reported as a missing gate");
}

void applyTests()
{
    QList<ExtensionRegistryRecord> records{
        record(ExtensionKind::Skill, QStringLiteral("alpha"), "src-a", "content-a"),
        record(ExtensionKind::Mcp, QStringLiteral("beta"), "src-b", "content-b")};
    records[1].trust = ExtensionTrustState::Unverified;

    const ExtensionTrustState trustBefore = records[0].trust;
    const ExtensionCompatibilityState compatibilityBefore = records[0].compatibility;

    ExtensionEnablementPolicy::apply(&records, {grantFor(records[0]),
                                               grantFor(records[1])});
    expect(records[0].effectiveEnabled,
           "apply did not enable an exactly granted eligible record");
    expect(!records[1].effectiveEnabled,
           "apply enabled an unreviewed record");

    // 只写生效启用状态：信任与兼容性判定一概不动，两者仍由各自的策略层负责。
    expect(records[0].trust == trustBefore
               && records[0].compatibility == compatibilityBefore,
           "apply overwrote a trust or compatibility verdict");

    // 撤销授权后必须回到未启用，而不是保留上一次的结论。
    ExtensionEnablementPolicy::apply(&records, {});
    expect(!records[0].effectiveEnabled && !records[1].effectiveEnabled,
           "apply retained enablement after the grant was withdrawn");

    // 空指针不得崩溃。
    ExtensionEnablementPolicy::apply(nullptr, {});
}

void gateAgreementTests()
{
    // 这一层写入的启用状态仍然要通过注册表独立的 Verified + Compatible 门禁，
    // 因此两道闸门互不依赖，任何一侧被绕过都会被另一侧发现。
    QList<ExtensionRegistryRecord> records{
        record(ExtensionKind::Skill, QStringLiteral("alpha"), "src-a", "content-a")};
    ExtensionEnablementPolicy::apply(&records, {grantFor(records[0])});

    ExtensionRegistryProjection projection;
    QString error;
    expect(ExtensionRegistry::build(records, &projection, &error),
           "a granted verified compatible record failed registry validation");
    expect(records[0].effectiveEnabled,
           "a granted verified compatible record was not enabled");

    // 撤销信任后同一条启用状态不再被注册表接受。
    records[0].trust = ExtensionTrustState::Unverified;
    expect(!ExtensionRegistry::build(records, &projection, &error),
           "registry accepted enablement after trust revocation");

    // 重新判定时这一层自己也会撤销启用。
    ExtensionEnablementPolicy::apply(&records, {grantFor(records[0])});
    expect(!records[0].effectiveEnabled,
           "enablement survived trust revocation");
    expect(ExtensionRegistry::build(records, &projection, &error),
           "a re-evaluated unverified record failed registry validation");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    baselineTests();
    driftTests();
    conflictTests();
    malformedTests();
    otherGateTests();
    applyTests();
    gateAgreementTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension enablement policy tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
