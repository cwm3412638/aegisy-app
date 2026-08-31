#include "extension_lifecycle_presentation.h"

#include "extension_update_policy.h"

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

ExtensionRegistryRecord record()
{
    ExtensionRegistryRecord value;
    value.kind = ExtensionKind::Skill;
    value.id = QStringLiteral("acme.formatter");
    value.name = QStringLiteral("Acme Formatter");
    value.version = QStringLiteral("2.1.0");
    value.sourceIdentity = sourceOf("acme");
    value.contentIdentity = contentOf("acme");
    value.trust = ExtensionTrustState::Verified;
    value.compatibility = ExtensionCompatibilityState::Compatible;
    value.scope = QStringLiteral("user");
    value.requestedCapabilities = QStringList{QStringLiteral("skill-content")};
    value.installed = true;
    return value;
}

// 这一层最重要的一条不变量：收回记录从不删除磁盘上的内容。它必须在每一条返回路径上成立，
// 而不是只在成功路径上被设置——界面正是靠这个字段才不能把"授权已收回"说成"内容已删除"。
void testNeverRemovesSourceContent()
{
    const ExtensionRegistryRecord value = record();
    const ExtensionRemovalPlan ready = ExtensionLifecyclePresentation::buildRemoval(
        value.kind, value.id, &value, true, true);
    expect(ready.state == ExtensionRemovalPlanState::Ready,
           "a complete target must be removable");
    expect(!ready.removesSourceContent,
           "a removal plan must never claim it deletes the source content");
    expect(ready.retainsIdentity,
           "a removal plan must retain the immutable identity");
    expect(!ready.retainedIdentity.isEmpty(),
           "a removal plan must carry the retained identity forward");

    const ExtensionRemovalPlan rejected = ExtensionLifecyclePresentation::buildRemoval(
        value.kind, QStringLiteral("../escape"), &value, true, true);
    expect(rejected.state == ExtensionRemovalPlanState::Unpresentable,
           "an invalid identifier must not be presentable");
    expect(rejected.errorCode
               == QStringLiteral("extension-removal-plan-id-invalid"),
           "an invalid identifier must report its own diagnostic");
    expect(!rejected.removesSourceContent,
           "a rejected plan must not claim it deletes the source content");
    expect(rejected.retainsIdentity,
           "a rejected plan must still state the identity is retained");
}

// 收回没有门禁。内容漂移、复核被撤回、来源已消失的目标都必须仍然可以被收回，否则一个被
// 篡改或被删掉来源的扩展会永远留着一份已认证的授权。
void testRemovalHasNoGates()
{
    ExtensionRegistryRecord drifted = record();
    drifted.trust = ExtensionTrustState::Unverified;
    drifted.compatibility = ExtensionCompatibilityState::Incompatible;
    drifted.installed = false;
    const ExtensionRemovalPlan plan = ExtensionLifecyclePresentation::buildRemoval(
        drifted.kind, drifted.id, &drifted, false, true);
    expect(plan.state == ExtensionRemovalPlanState::Ready,
           "an unreviewed, incompatible, uninstalled target must stay removable");
    expect(!plan.targetAbsent, "a listed target must not be reported as absent");
    expect(plan.withdrawsGrant, "the plan must state the grant is withdrawn");
    expect(!plan.withdrawsReview,
           "the plan must not claim to withdraw a review pin that does not exist");

    const ExtensionRemovalPlan vanished = ExtensionLifecyclePresentation::buildRemoval(
        ExtensionKind::Mcp, QStringLiteral("missing.mcp"), nullptr, true, true);
    expect(vanished.state == ExtensionRemovalPlanState::Ready,
           "a vanished source must stay removable");
    expect(vanished.targetAbsent,
           "a vanished source must be reported as absent, not silently listed");
    expect(vanished.title == QStringLiteral("missing.mcp"),
           "a vanished source must fall back to its identifier as the title");
    expect(vanished.sourceIdentity.isEmpty() && vanished.contentIdentity.isEmpty(),
           "a vanished source has no digests to display");
    expect(vanished.kind == ExtensionKind::Mcp,
           "a vanished source must keep the kind it was asked about");
}

// 名称来自不可信来源。不可展示时退回使用标识本身，而不是拒绝收回：一个把自己名字做成
// 不可展示文本的扩展否则就无法被收回授权。
void testUntrustedTextFallsBack()
{
    ExtensionRegistryRecord hostile = record();
    hostile.name = QStringLiteral("Acme‮Formatter");
    hostile.sourceIdentity = QStringLiteral("extension-source:sha256:short");
    hostile.contentIdentity = QStringLiteral("not-a-digest");
    const ExtensionRemovalPlan plan = ExtensionLifecyclePresentation::buildRemoval(
        hostile.kind, hostile.id, &hostile, true, false);
    expect(plan.state == ExtensionRemovalPlanState::Ready,
           "unpresentable metadata must not block removal");
    expect(plan.title == hostile.id,
           "an unpresentable name must fall back to the identifier");
    expect(plan.sourceIdentity.isEmpty(),
           "a malformed source digest must not be displayed");
    expect(plan.contentIdentity.isEmpty(),
           "a malformed content digest must not be displayed");
    expect(plan.withdrawsReview && !plan.withdrawsGrant,
           "the plan must name exactly the halves that exist");
}

// 这次收回是否成立只有一个来源：判定层。呈现层自己再判一遍必然会与它漂移，而漂移的方向
// 是界面提供一个判定层会拒绝的动作。
void testDelegatesTheVerdict()
{
    const ExtensionRegistryRecord value = record();
    const ExtensionRemovalPlan mismatched =
        ExtensionLifecyclePresentation::buildRemoval(
            ExtensionKind::Mcp, value.id, &value, true, true);
    const ExtensionRemovalVerdict verdict = ExtensionUpdatePolicy::evaluateRemoval(
        ExtensionKind::Mcp, value.id, &value);
    expect(verdict.state != ExtensionRemovalState::Ready,
           "the policy must reject a record whose kind disagrees with the request");
    expect(mismatched.state == ExtensionRemovalPlanState::Unpresentable,
           "the presentation must not offer what the policy rejects");
    expect(mismatched.errorCode == verdict.errorCode,
           "the presentation must report the policy's own diagnostic");

    const ExtensionRemovalPlan plan = ExtensionLifecyclePresentation::buildRemoval(
        value.kind, value.id, &value, true, true);
    const ExtensionRemovalVerdict ready = ExtensionUpdatePolicy::evaluateRemoval(
        value.kind, value.id, &value);
    expect(plan.retainedIdentity == ready.retainedIdentity,
           "the retained identity must be the policy's, not a second construction");
    expect(plan.retainsIdentity == ready.retainsIdentityMetadata,
           "identity retention must be restated from the policy");
}

// 两半都不存在时收回没有任何可收回的东西。界面据此不提供动作，因此这个区分必须可读。
void testNothingToWithdrawIsVisible()
{
    const ExtensionRegistryRecord value = record();
    const ExtensionRemovalPlan plan = ExtensionLifecyclePresentation::buildRemoval(
        value.kind, value.id, &value, false, false);
    expect(plan.state == ExtensionRemovalPlanState::Ready,
           "a target with no records is still a well-formed plan");
    expect(!plan.withdrawsGrant && !plan.withdrawsReview,
           "a target with no records must not claim to withdraw anything");
    expect(!plan.removesSourceContent,
           "a no-op plan must still not claim it deletes content");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testNeverRemovesSourceContent();
    testRemovalHasNoGates();
    testUntrustedTextFallsBack();
    testDelegatesTheVerdict();
    testNothingToWithdrawIsVisible();
    if (failures == 0) {
        QTextStream(stdout) << "extension lifecycle presentation guards passed\n";
    }
    return failures == 0 ? 0 : 1;
}
