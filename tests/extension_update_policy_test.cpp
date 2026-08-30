#include "extension_update_policy.h"

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

QString identity(const QString &prefix, const QByteArray &seed)
{
    return prefix + QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex());
}

QString sourceOf(const QByteArray &seed)
{
    return identity(QStringLiteral("extension-source:sha256:"), seed + "-source");
}

QString contentOf(const QByteArray &seed)
{
    return identity(QStringLiteral("extension-content:sha256:"), seed + "-content");
}

// 当前生效的版本:已被复核、兼容、已安装。
ExtensionRegistryRecord active()
{
    ExtensionRegistryRecord value;
    value.kind = ExtensionKind::Skill;
    value.id = QStringLiteral("acme.formatter");
    value.name = QStringLiteral("Acme Formatter");
    value.version = QStringLiteral("2.1.0");
    value.sourceIdentity = sourceOf("acme-v2");
    value.contentIdentity = contentOf("acme-v2");
    value.trust = ExtensionTrustState::Verified;
    value.compatibility = ExtensionCompatibilityState::Compatible;
    value.scope = QStringLiteral("user");
    value.requestedCapabilities = QStringList{QStringLiteral("skill-content")};
    value.installed = true;
    return value;
}

ExtensionUpdateCandidate candidate()
{
    ExtensionUpdateCandidate value;
    value.kind = ExtensionKind::Skill;
    value.id = QStringLiteral("acme.formatter");
    value.version = QStringLiteral("2.2.0");
    value.sourceIdentity = sourceOf("acme-v3");
    value.contentIdentity = contentOf("acme-v3");
    value.requestedCapabilities = QStringList{QStringLiteral("skill-content")};
    return value;
}

ExtensionUpdateEvidence passing()
{
    ExtensionUpdateEvidence value;
    value.signatureValid = true;
    value.manifestValid = true;
    value.compatible = true;
    value.dependenciesSatisfied = true;
    value.healthy = true;
    return value;
}

// 校验通过的候选可以被暂存,但它按定义是另一份内容,因此从未复核、未授权。
void stagedTests()
{
    const ExtensionUpdateVerdict verdict =
        ExtensionUpdatePolicy::evaluate(active(), candidate(), passing());
    expect(verdict.state == ExtensionUpdateState::StagedUnreviewed,
           "a fully validated candidate was rejected");
    expect(verdict.errorCode.isEmpty(),
           "a staged candidate carried an error code");
    // 这一层存在的全部理由:候选不继承上一版的信任与授权。
    expect(!verdict.inheritsTrust,
           "the candidate inherited the reviewed trust of another content");
    expect(!verdict.inheritsGrant,
           "the candidate inherited the enablement grant of another content");
    expect(!verdict.candidateExecutable,
           "a merely validated candidate was already executable");
    expect(verdict.activePreserved,
           "staging a candidate did not preserve the active version");
    expect(!verdict.downgrade, "an upgrade was reported as a downgrade");
}

// 校验失败时当前版本保持不变,候选不执行。每一项都必须独立成立。
void validationTests()
{
    struct Case {
        const char *label;
        ExtensionUpdateEvidence evidence;
        QString code;
    };
    QList<Case> cases;
    {
        ExtensionUpdateEvidence value = passing();
        value.signatureValid = false;
        cases.append({"signature", value,
                      QStringLiteral("extension-update-signature-invalid")});
    }
    {
        ExtensionUpdateEvidence value = passing();
        value.manifestValid = false;
        cases.append({"manifest", value,
                      QStringLiteral("extension-update-manifest-invalid")});
    }
    {
        ExtensionUpdateEvidence value = passing();
        value.compatible = false;
        cases.append({"compatibility", value,
                      QStringLiteral("extension-update-incompatible")});
    }
    {
        ExtensionUpdateEvidence value = passing();
        value.dependenciesSatisfied = false;
        cases.append({"dependency", value,
                      QStringLiteral("extension-update-dependency-unsatisfied")});
    }
    {
        ExtensionUpdateEvidence value = passing();
        value.healthy = false;
        cases.append({"health", value,
                      QStringLiteral("extension-update-health-failed")});
    }
    for (const Case &item : cases) {
        const ExtensionUpdateVerdict verdict =
            ExtensionUpdatePolicy::evaluate(active(), candidate(), item.evidence);
        expect(verdict.state == ExtensionUpdateState::Rejected,
               "a candidate failing one validation was still staged");
        expect(verdict.errorCode == item.code,
               "a failed validation did not report its own diagnostic");
        // 关键:失败路径上当前版本仍然不变,候选仍然不可执行。
        expect(verdict.activePreserved,
               "a failed upgrade did not leave the active version unchanged");
        expect(!verdict.candidateExecutable,
               "a candidate that failed validation could execute");
        expect(!verdict.inheritsTrust && !verdict.inheritsGrant,
               "a rejected candidate still inherited authority");
    }
}

// 目标与身份必须成立,并且"内容未变"不是一次更新。
void targetTests()
{
    ExtensionUpdateCandidate wrongKind = candidate();
    wrongKind.kind = ExtensionKind::Mcp;
    expect(ExtensionUpdatePolicy::evaluate(active(), wrongKind, passing()).errorCode
               == QStringLiteral("extension-update-target-mismatch"),
           "an update for another extension kind was accepted");
    ExtensionUpdateCandidate wrongId = candidate();
    wrongId.id = QStringLiteral("other.extension");
    expect(ExtensionUpdatePolicy::evaluate(active(), wrongId, passing()).errorCode
               == QStringLiteral("extension-update-target-mismatch"),
           "an update for another identifier was accepted");
    ExtensionUpdateCandidate badId = candidate();
    badId.id = QStringLiteral("Bad Id");
    expect(ExtensionUpdatePolicy::evaluate(active(), badId, passing()).errorCode
               == QStringLiteral("extension-update-target-mismatch"),
           "an update with a malformed identifier was accepted");

    for (const QString &bad : {
             QStringLiteral(""),
             QStringLiteral("extension-content:sha256:abc"),
             QStringLiteral("acme.formatter")}) {
        ExtensionUpdateCandidate value = candidate();
        value.contentIdentity = bad;
        expect(ExtensionUpdatePolicy::evaluate(active(), value, passing()).errorCode
                   == QStringLiteral("extension-update-identity-invalid"),
               "an update with a malformed content identity was accepted");
    }

    // 内容摘要相同的"更新"不是更新:把它当作更新会推进状态,并可能刷掉一条尚未处理的
    // 漂移诊断。
    ExtensionUpdateCandidate unchanged = candidate();
    unchanged.contentIdentity = active().contentIdentity;
    expect(ExtensionUpdatePolicy::evaluate(active(), unchanged, passing()).errorCode
               == QStringLiteral("extension-update-content-unchanged"),
           "an update to identical content was accepted as an update");
}

// 复核绝不按标识或版本号传递。
void reviewTransferTests()
{
    ExtensionReviewPin pin;
    pin.kind = active().kind;
    pin.id = active().id;
    pin.sourceIdentity = active().sourceIdentity;
    pin.contentIdentity = active().contentIdentity;

    // 关键:针对当前内容的复核不适用于候选内容。
    expect(!ExtensionUpdatePolicy::reviewTransfers(pin, candidate()),
           "a review of the active content transferred to new content");

    // 只有内容与来源都完全一致时才仍然是同一份内容——而那不是一次更新。
    ExtensionUpdateCandidate same = candidate();
    same.sourceIdentity = pin.sourceIdentity;
    same.contentIdentity = pin.contentIdentity;
    expect(ExtensionUpdatePolicy::reviewTransfers(pin, same),
           "a review did not apply to byte-identical content");

    // 来源变化同样让复核失效:同名内容换了来源不能延续结论。
    ExtensionUpdateCandidate sourceChanged = same;
    sourceChanged.sourceIdentity = sourceOf("elsewhere");
    expect(!ExtensionUpdatePolicy::reviewTransfers(pin, sourceChanged),
           "a review survived a change of source");

    // 空摘要不构成证据。
    ExtensionReviewPin empty = pin;
    empty.contentIdentity.clear();
    expect(!ExtensionUpdatePolicy::reviewTransfers(empty, same),
           "an empty review identity was treated as matching evidence");
    ExtensionReviewPin emptySource = pin;
    emptySource.sourceIdentity.clear();
    expect(!ExtensionUpdatePolicy::reviewTransfers(emptySource, same),
           "an empty review source identity was treated as matching");

    // 类型必须一致:同一个标识在不同类型下是不同的扩展。
    ExtensionUpdateCandidate otherKind = same;
    otherKind.kind = ExtensionKind::Mcp;
    expect(!ExtensionUpdatePolicy::reviewTransfers(pin, otherKind),
           "a review transferred across extension kinds");
}

// 降级必须可见,但不参与权威判定。
void downgradeTests()
{
    ExtensionUpdateCandidate older = candidate();
    older.version = QStringLiteral("2.0.5");
    const ExtensionUpdateVerdict verdict =
        ExtensionUpdatePolicy::evaluate(active(), older, passing());
    expect(verdict.state == ExtensionUpdateState::StagedUnreviewed,
           "a downgrade was refused outright rather than disclosed");
    expect(verdict.downgrade, "a downgrade was not disclosed");
    // 降级同样不继承权威。
    expect(!verdict.inheritsTrust && !verdict.inheritsGrant,
           "a downgrade inherited authority from the newer content");

    // 无法比较的版本不声称是降级:那会把"不知道"表述成一个具体结论。
    ExtensionUpdateCandidate opaque = candidate();
    opaque.version = QStringLiteral("2026-build-x");
    expect(!ExtensionUpdatePolicy::evaluate(active(), opaque, passing()).downgrade,
           "an incomparable version was reported as a downgrade");

    ExtensionUpdateCandidate newer = candidate();
    newer.version = QStringLiteral("2.1.1");
    expect(!ExtensionUpdatePolicy::evaluate(active(), newer, passing()).downgrade,
           "an upgrade was reported as a downgrade");
}

// 移除停用可执行内容,但保留身份元数据,并且必须收回授权。
void removalTests()
{
    const ExtensionRegistryRecord record = active();
    const ExtensionRemovalVerdict verdict = ExtensionUpdatePolicy::evaluateRemoval(
        record.kind, record.id, &record);
    expect(verdict.state == ExtensionRemovalState::Ready,
           "removing an installed extension was refused");
    expect(verdict.removesExecutableContent,
           "removal left executable content in place");
    // 关键:身份元数据保留,否则一次移除会抹掉"这份内容曾被授权运行"的历史。
    expect(verdict.retainsIdentityMetadata,
           "removal discarded the immutable identity metadata");
    expect(verdict.retainedIdentity.contains(record.id)
               && verdict.retainedIdentity.contains(record.contentIdentity),
           "the retained identity does not identify the removed content");
    // 移除必须收回授权:留下授权会让同名内容重新出现时直接继承它。
    expect(!verdict.retainsGrant,
           "removal left an enablement grant behind");

    // 目标已经消失时仍然可以移除,并且仍然留下可辨识的身份。
    const ExtensionRemovalVerdict absent = ExtensionUpdatePolicy::evaluateRemoval(
        record.kind, record.id, nullptr);
    expect(absent.state == ExtensionRemovalState::Ready,
           "removing a vanished target was refused");
    expect(absent.retainsIdentityMetadata && !absent.retainsGrant,
           "removing a vanished target dropped history or kept its grant");
    expect(absent.retainedIdentity.contains(record.id),
           "the retained identity of a vanished target is unidentifiable");

    // 标识不合法或目标不一致时拒绝。
    expect(ExtensionUpdatePolicy::evaluateRemoval(
               record.kind, QStringLiteral("Bad Id"), nullptr).errorCode
               == QStringLiteral("extension-removal-id-invalid"),
           "removal accepted a malformed identifier");
    expect(ExtensionUpdatePolicy::evaluateRemoval(
               ExtensionKind::Mcp, record.id, &record).errorCode
               == QStringLiteral("extension-removal-target-mismatch"),
           "removal accepted a target of another kind");
}

// 这一层不安装、不启用、不执行任何东西。
void authorityTests()
{
    // 一个被暂存的候选不会让任何记录变成已启用:启用仍然独立要求账本里存在授权。
    const ExtensionUpdateVerdict verdict =
        ExtensionUpdatePolicy::evaluate(active(), candidate(), passing());
    expect(verdict.state == ExtensionUpdateState::StagedUnreviewed,
           "the fixture candidate was not staged");
    const ExtensionEnablementDecision decision =
        ExtensionEnablementPolicy::evaluate(active(), {});
    expect(!decision.enabled,
           "staging a candidate enabled a record without a grant");

    // 即使当前版本持有一份授权,候选内容也不满足它:授权绑定确切内容摘要。
    ExtensionEnablementGrant grant;
    grant.kind = active().kind;
    grant.id = active().id;
    grant.sourceIdentity = active().sourceIdentity;
    grant.contentIdentity = active().contentIdentity;
    ExtensionRegistryRecord updated = active();
    updated.sourceIdentity = candidate().sourceIdentity;
    updated.contentIdentity = candidate().contentIdentity;
    const ExtensionEnablementDecision afterUpdate =
        ExtensionEnablementPolicy::evaluate(updated, {grant});
    expect(!afterUpdate.enabled,
           "the previous grant enabled the updated content");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    stagedTests();
    validationTests();
    targetTests();
    reviewTransferTests();
    downgradeTests();
    removalTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension update policy tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
