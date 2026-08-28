#include "extension_enablement_workflow.h"

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
    return digest(QStringLiteral("extension-source:sha256:"), seed + "-source");
}

QString contentOf(const QByteArray &seed)
{
    return digest(QStringLiteral("extension-content:sha256:"), seed + "-content");
}

// 夹具默认是"已复核且兼容"，因为那才是授予可能成功的唯一状态；缺门的情形由各自的
// 用例显式撤下。
ExtensionRegistryRecord record(ExtensionKind kind, const QString &id,
                               const QByteArray &seed)
{
    ExtensionRegistryRecord value;
    value.kind = kind;
    value.id = id;
    value.name = QStringLiteral("Fixture");
    value.version = QStringLiteral("1.0.0");
    value.sourceIdentity = sourceOf(seed);
    value.contentIdentity = contentOf(seed);
    value.scope = QStringLiteral("user");
    value.installed = true;
    value.trust = ExtensionTrustState::Verified;
    value.compatibility = ExtensionCompatibilityState::Compatible;
    return value;
}

ExtensionEnablementRequest enable(const ExtensionRegistryRecord &target)
{
    ExtensionEnablementRequest request;
    request.action = ExtensionEnablementAction::Enable;
    request.kind = target.kind;
    request.id = target.id;
    request.reviewedSourceIdentity = target.sourceIdentity;
    request.reviewedContentIdentity = target.contentIdentity;
    return request;
}

ExtensionEnablementRequest disable(ExtensionKind kind, const QString &id)
{
    ExtensionEnablementRequest request;
    request.action = ExtensionEnablementAction::Disable;
    request.kind = kind;
    request.id = id;
    return request;
}

ExtensionEnablementGrant grantOf(const ExtensionRegistryRecord &target)
{
    ExtensionEnablementGrant grant;
    grant.kind = target.kind;
    grant.id = target.id;
    grant.sourceIdentity = target.sourceIdentity;
    grant.contentIdentity = target.contentIdentity;
    return grant;
}

ExtensionEnablementLedgerStoreResult emptyLedger()
{
    ExtensionEnablementLedgerStoreResult ledger;
    ledger.state = ExtensionEnablementLedgerStoreState::Empty;
    return ledger;
}

ExtensionEnablementLedgerStoreResult readyLedger(
    const QList<ExtensionEnablementGrant> &grants, qint64 generation)
{
    ExtensionEnablementLedgerStoreResult ledger;
    ledger.state = ExtensionEnablementLedgerStoreState::Ready;
    ledger.grants = grants;
    ledger.generation = generation;
    return ledger;
}

bool holds(const QList<ExtensionEnablementGrant> &grants,
           const ExtensionEnablementGrant &expected)
{
    for (const ExtensionEnablementGrant &grant : grants) {
        if (grant.kind == expected.kind && grant.id == expected.id
                && grant.sourceIdentity == expected.sourceIdentity
                && grant.contentIdentity == expected.contentIdentity) {
            return true;
        }
    }
    return false;
}

void grantTests()
{
    const ExtensionRegistryRecord target =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");

    const ExtensionEnablementPlan plan =
        ExtensionEnablementWorkflow::plan(enable(target), {target}, emptyLedger());
    expect(plan.state == ExtensionEnablementPlanState::Ready
               && plan.changed && plan.grants.size() == 1
               && plan.expectedGeneration == 0
               && holds(plan.grants, grantOf(target)),
           "enabling a reviewed, compatible, installed record was not planned");

    // 授权集合是完整集合，不是增量：既有授权必须被带上，否则提交会静默撤销它们。
    const ExtensionRegistryRecord other =
        record(ExtensionKind::Mcp, QStringLiteral("fixture.mcp"), "b");
    const ExtensionEnablementPlan preserved = ExtensionEnablementWorkflow::plan(
        enable(target), {target, other}, readyLedger({grantOf(other)}, 4));
    expect(preserved.state == ExtensionEnablementPlanState::Ready
               && preserved.grants.size() == 2
               && holds(preserved.grants, grantOf(other))
               && preserved.expectedGeneration == 4,
           "planning a grant discarded the existing grants");

    // 重复授予同一条内容不算变化：提交只会白白推进代号并改变身份摘要。
    const ExtensionEnablementPlan idempotent = ExtensionEnablementWorkflow::plan(
        enable(target), {target}, readyLedger({grantOf(target)}, 2));
    expect(idempotent.state == ExtensionEnablementPlanState::Ready
               && !idempotent.changed && idempotent.grants.size() == 1,
           "re-granting identical content was reported as a change");

    // 内容更新后重新授予是替换而不是追加：否则集合会变成启用判定必须拒绝的冲突状态。
    ExtensionRegistryRecord updated = target;
    updated.contentIdentity = contentOf("a2");
    const ExtensionEnablementPlan replaced = ExtensionEnablementWorkflow::plan(
        enable(updated), {updated}, readyLedger({grantOf(target)}, 3));
    expect(replaced.state == ExtensionEnablementPlanState::Ready
               && replaced.changed && replaced.grants.size() == 1
               && holds(replaced.grants, grantOf(updated)),
           "re-granting updated content appended a conflicting grant");
}

void revocationTests()
{
    const ExtensionRegistryRecord target =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");
    const ExtensionRegistryRecord other =
        record(ExtensionKind::Mcp, QStringLiteral("fixture.mcp"), "b");

    const ExtensionEnablementPlan plan = ExtensionEnablementWorkflow::plan(
        disable(target.kind, target.id), {target, other},
        readyLedger({grantOf(target), grantOf(other)}, 5));
    expect(plan.state == ExtensionEnablementPlanState::Ready && plan.changed
               && plan.grants.size() == 1 && holds(plan.grants, grantOf(other))
               && plan.expectedGeneration == 5,
           "disabling did not remove exactly the requested grant");

    // 停用只依据 (kind, id)。内容漂移、记录整体消失、甚至请求不带摘要，都必须仍然
    // 能撤销，否则一个被篡改过的扩展将永远无法撤销其启用授权。
    ExtensionRegistryRecord drifted = target;
    drifted.contentIdentity = contentOf("tampered");
    const ExtensionEnablementPlan afterDrift = ExtensionEnablementWorkflow::plan(
        disable(target.kind, target.id), {drifted},
        readyLedger({grantOf(target)}, 1));
    expect(afterDrift.state == ExtensionEnablementPlanState::Ready
               && afterDrift.changed && afterDrift.grants.isEmpty(),
           "content drift blocked revocation");

    const ExtensionEnablementPlan afterRemoval = ExtensionEnablementWorkflow::plan(
        disable(target.kind, target.id), {}, readyLedger({grantOf(target)}, 1));
    expect(afterRemoval.state == ExtensionEnablementPlanState::Ready
               && afterRemoval.changed && afterRemoval.grants.isEmpty(),
           "an absent record blocked revocation");

    // 复核或兼容已被撤销之后同样必须能停用。
    ExtensionRegistryRecord unverified = target;
    unverified.trust = ExtensionTrustState::Unverified;
    const ExtensionEnablementPlan afterRevoke = ExtensionEnablementWorkflow::plan(
        disable(target.kind, target.id), {unverified},
        readyLedger({grantOf(target)}, 1));
    expect(afterRevoke.state == ExtensionEnablementPlanState::Ready
               && afterRevoke.grants.isEmpty(),
           "a revoked review blocked revocation");

    // 本来就没有授权时停用是允许的，但不算变化。
    const ExtensionEnablementPlan absent = ExtensionEnablementWorkflow::plan(
        disable(target.kind, target.id), {target}, emptyLedger());
    expect(absent.state == ExtensionEnablementPlanState::Ready && !absent.changed
               && absent.grants.isEmpty(),
           "disabling an ungranted record was reported as a change");

    // 停用只匹配同一种类：不同 kind 的同名扩展不得被牵连。
    ExtensionEnablementGrant sameId = grantOf(target);
    sameId.kind = ExtensionKind::CodexPlugin;
    const ExtensionEnablementPlan scoped = ExtensionEnablementWorkflow::plan(
        disable(target.kind, target.id), {target},
        readyLedger({sameId}, 1));
    expect(scoped.state == ExtensionEnablementPlanState::Ready && !scoped.changed
               && scoped.grants.size() == 1,
           "disabling removed a grant of a different kind");
}

void driftTests()
{
    const ExtensionRegistryRecord target =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");

    // 渲染与授予之间内容变化：人工看到的不是现在要授权运行的东西。
    ExtensionRegistryRecord drifted = target;
    drifted.contentIdentity = contentOf("a2");
    const ExtensionEnablementPlan content = ExtensionEnablementWorkflow::plan(
        enable(target), {drifted}, emptyLedger());
    expect(content.state == ExtensionEnablementPlanState::Rejected
               && content.errorCode
                   == QStringLiteral("extension-enablement-content-drift")
               && content.grants.isEmpty(),
           "content drift was granted against the current content");

    ExtensionRegistryRecord moved = target;
    moved.sourceIdentity = sourceOf("a2");
    const ExtensionEnablementPlan source = ExtensionEnablementWorkflow::plan(
        enable(target), {moved}, emptyLedger());
    expect(source.state == ExtensionEnablementPlanState::Rejected
               && source.errorCode
                   == QStringLiteral("extension-enablement-source-drift"),
           "source drift was granted against the current source");
}

void gateTests()
{
    const ExtensionRegistryRecord target =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");

    // 未复核时授予必须被拒绝，而不是留下一条"等复核出现就生效"的已认证授权。
    ExtensionRegistryRecord unverified = target;
    unverified.trust = ExtensionTrustState::Unverified;
    const ExtensionEnablementPlan review = ExtensionEnablementWorkflow::plan(
        enable(target), {unverified}, emptyLedger());
    expect(review.state == ExtensionEnablementPlanState::Rejected
               && review.errorCode
                   == QStringLiteral("extension-enablement-trust-missing")
               && review.grants.isEmpty(),
           "an unreviewed record was granted enablement");

    ExtensionRegistryRecord incompatible = target;
    incompatible.compatibility = ExtensionCompatibilityState::Incompatible;
    const ExtensionEnablementPlan compatibility =
        ExtensionEnablementWorkflow::plan(
            enable(target), {incompatible}, emptyLedger());
    expect(compatibility.state == ExtensionEnablementPlanState::Rejected
               && compatibility.errorCode == QStringLiteral(
                   "extension-enablement-compatibility-missing"),
           "an incompatible record was granted enablement");

    ExtensionRegistryRecord unknown = target;
    unknown.compatibility = ExtensionCompatibilityState::Unknown;
    expect(ExtensionEnablementWorkflow::plan(
               enable(target), {unknown}, emptyLedger()).errorCode
               == QStringLiteral("extension-enablement-compatibility-missing"),
           "unknown compatibility was granted enablement");

    ExtensionRegistryRecord notInstalled = target;
    notInstalled.installed = false;
    expect(ExtensionEnablementWorkflow::plan(
               enable(target), {notInstalled}, emptyLedger()).errorCode
               == QStringLiteral("extension-enablement-target-not-installed"),
           "an uninstalled record was granted enablement");

    // 授予一条不存在的记录等于预先授权将来出现的内容。
    expect(ExtensionEnablementWorkflow::plan(
               enable(target), {}, emptyLedger()).errorCode
               == QStringLiteral("extension-enablement-target-absent"),
           "an absent record was granted enablement");

    // 清单里同一 (kind, id) 出现多条说明来源已不可信，不能任选一条授予。
    ExtensionRegistryRecord twin = target;
    twin.contentIdentity = contentOf("twin");
    expect(ExtensionEnablementWorkflow::plan(
               enable(target), {target, twin}, emptyLedger()).errorCode
               == QStringLiteral("extension-enablement-target-ambiguous"),
           "an ambiguous record was granted enablement");

    // 记录自身摘要不合法时不能授予。
    ExtensionRegistryRecord unverifiable = target;
    unverifiable.sourceIdentity = QStringLiteral("extension-source:sha256:zz");
    ExtensionEnablementRequest request = enable(target);
    request.reviewedSourceIdentity = unverifiable.sourceIdentity;
    expect(ExtensionEnablementWorkflow::plan(
               request, {unverifiable}, emptyLedger()).errorCode
               == QStringLiteral(
                   "extension-enablement-request-identity-invalid"),
           "a malformed reviewed identity was accepted");

    ExtensionEnablementRequest badId = enable(target);
    badId.id = QStringLiteral("Bad Id");
    expect(ExtensionEnablementWorkflow::plan(
               badId, {target}, emptyLedger()).errorCode
               == QStringLiteral("extension-enablement-request-id-invalid"),
           "a malformed request id was accepted");
    // 停用同样要求 ID 合法：否则一个无效 ID 会静默匹配不到任何东西并报告成功。
    ExtensionEnablementRequest badDisable = disable(target.kind, QStringLiteral(""));
    expect(ExtensionEnablementWorkflow::plan(
               badDisable, {target}, emptyLedger()).errorCode
               == QStringLiteral("extension-enablement-request-id-invalid"),
           "a malformed disable id was accepted");
}

void ledgerStateTests()
{
    const ExtensionRegistryRecord target =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");

    // 读不出当前集合时不能规划：那会把不完整集合当成完整集合提交，静默撤销其余授权。
    for (const ExtensionEnablementLedgerStoreState state :
         {ExtensionEnablementLedgerStoreState::Invalid,
          ExtensionEnablementLedgerStoreState::Unavailable,
          ExtensionEnablementLedgerStoreState::OutcomeUnknown}) {
        ExtensionEnablementLedgerStoreResult ledger;
        ledger.state = state;
        const ExtensionEnablementPlan plan =
            ExtensionEnablementWorkflow::plan(enable(target), {target}, ledger);
        expect(plan.state == ExtensionEnablementPlanState::Rejected
                   && plan.errorCode == QStringLiteral(
                       "extension-enablement-ledger-unusable")
                   && plan.grants.isEmpty(),
               "an unreadable grant ledger was planned against");
        // 停用也必须拒绝：它同样会提交一份完整集合。
        expect(ExtensionEnablementWorkflow::plan(
                   disable(target.kind, target.id), {target}, ledger).state
                   == ExtensionEnablementPlanState::Rejected,
               "an unreadable grant ledger was revoked against");
    }

    // Empty 必须真的是空的：带着内容的 Empty 是自相矛盾的读取结果。
    ExtensionEnablementLedgerStoreResult contradictory = emptyLedger();
    contradictory.grants = {grantOf(target)};
    expect(ExtensionEnablementWorkflow::plan(
               enable(target), {target}, contradictory).errorCode
               == QStringLiteral("extension-enablement-ledger-inconsistent"),
           "a contradictory empty ledger was planned against");

    ExtensionEnablementLedgerStoreResult negative = readyLedger({}, -1);
    expect(ExtensionEnablementWorkflow::plan(
               enable(target), {target}, negative).errorCode
               == QStringLiteral("extension-enablement-ledger-inconsistent"),
           "a negative generation was planned against");

    // 已存集合里有一条无效授权时整体拒绝：带着它提交会把它洗成已认证的授权。
    ExtensionEnablementGrant malformed = grantOf(target);
    malformed.contentIdentity = QStringLiteral("extension-content:sha256:zz");
    expect(ExtensionEnablementWorkflow::plan(
               enable(target), {target}, readyLedger({malformed}, 1)).errorCode
               == QStringLiteral("extension-enablement-ledger-grant-invalid"),
           "a malformed existing grant was laundered into a new commit");

    ExtensionEnablementGrant duplicate = grantOf(target);
    expect(ExtensionEnablementWorkflow::plan(
               enable(target), {target},
               readyLedger({duplicate, duplicate}, 1)).errorCode
               == QStringLiteral("extension-enablement-ledger-conflict"),
           "a conflicting grant set was planned against");

    // 上限用满时新增被拒绝，而替换既有授权仍然允许。
    QList<ExtensionEnablementGrant> full;
    for (int i = 0; i < ExtensionEnablementPolicy::MaxGrants; ++i) {
        full.append(grantOf(record(ExtensionKind::Skill,
            QStringLiteral("filler.%1").arg(i), QByteArray::number(i))));
    }
    expect(ExtensionEnablementWorkflow::plan(
               enable(target), {target}, readyLedger(full, 1)).errorCode
               == QStringLiteral("extension-enablement-grant-limit"),
           "the grant limit was exceeded");
    QList<ExtensionEnablementGrant> fullWithTarget = full;
    fullWithTarget.removeLast();
    fullWithTarget.append(grantOf(target));
    expect(ExtensionEnablementWorkflow::plan(
               enable(target), {target}, readyLedger(fullWithTarget, 1)).state
               == ExtensionEnablementPlanState::Ready,
           "replacing an existing grant at the limit was rejected");
}

void authorityTests()
{
    // 规划层不判定启用：它产出的授权仍然要经过 ExtensionEnablementPolicy，而判定
    // 独立要求已复核、兼容且已安装。
    const ExtensionRegistryRecord target =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");
    const ExtensionEnablementPlan plan =
        ExtensionEnablementWorkflow::plan(enable(target), {target}, emptyLedger());
    expect(plan.state == ExtensionEnablementPlanState::Ready,
           "the authority fixture was not planned");

    expect(ExtensionEnablementPolicy::evaluate(target, plan.grants).enabled,
           "a planned grant did not enable an eligible record");

    // 复核在提交之后被撤销：同一份授权不再启用任何东西。
    ExtensionRegistryRecord revoked = target;
    revoked.trust = ExtensionTrustState::Unverified;
    expect(!ExtensionEnablementPolicy::evaluate(revoked, plan.grants).enabled,
           "enablement survived review revocation");

    // 内容在提交之后变化：授权绑定的是内容摘要，因此不再适用。
    ExtensionRegistryRecord drifted = target;
    drifted.contentIdentity = contentOf("a2");
    expect(!ExtensionEnablementPolicy::evaluate(drifted, plan.grants).enabled,
           "enablement survived content drift");

    // 规划层本身不写 effectiveEnabled：注册表仍然是最后一道门。
    QList<ExtensionRegistryRecord> records = {target};
    expect(!records.first().effectiveEnabled,
           "planning wrote effective enablement onto the record");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    grantTests();
    revocationTests();
    driftTests();
    gateTests();
    ledgerStateTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension enablement workflow tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
