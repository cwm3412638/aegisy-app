#include "extension_review_workflow.h"

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
    return digest(QStringLiteral("extension-source:sha256:"), seed + "-source");
}

QString contentOf(const QByteArray &seed)
{
    return digest(QStringLiteral("extension-content:sha256:"), seed + "-content");
}

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
    return value;
}

ExtensionReviewRequest approve(const ExtensionRegistryRecord &target)
{
    ExtensionReviewRequest request;
    request.action = ExtensionReviewAction::Approve;
    request.kind = target.kind;
    request.id = target.id;
    request.reviewedSourceIdentity = target.sourceIdentity;
    request.reviewedContentIdentity = target.contentIdentity;
    return request;
}

ExtensionReviewRequest revoke(ExtensionKind kind, const QString &id)
{
    ExtensionReviewRequest request;
    request.action = ExtensionReviewAction::Revoke;
    request.kind = kind;
    request.id = id;
    return request;
}

ExtensionReviewLedgerStoreResult empty()
{
    ExtensionReviewLedgerStoreResult result;
    result.state = ExtensionReviewLedgerStoreState::Empty;
    return result;
}

ExtensionReviewLedgerStoreResult ready(const QList<ExtensionReviewPin> &pins,
                                       qint64 generation)
{
    ExtensionReviewLedgerStoreResult result;
    result.state = ExtensionReviewLedgerStoreState::Ready;
    result.pins = pins;
    result.generation = generation;
    result.identity = QStringLiteral("extension-review-ledger:sha256:")
        + QString(64, QLatin1Char('a'));
    return result;
}

bool rejected(const ExtensionReviewPlan &plan, const QString &code)
{
    return plan.state == ExtensionReviewPlanState::Rejected
        && plan.errorCode == code && plan.pins.isEmpty() && !plan.changed
        && plan.expectedGeneration == 0;
}

void approvalTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");

    // 首次批准：集合从空变成恰好一条，代号照原样交给存储做比较并交换。
    const ExtensionReviewPlan first =
        ExtensionReviewWorkflow::plan(approve(skill), {skill}, empty());
    expect(first.state == ExtensionReviewPlanState::Ready && first.changed
               && first.pins.size() == 1
               && first.expectedGeneration == 0
               && first.pins.at(0).kind == skill.kind
               && first.pins.at(0).id == skill.id
               && first.pins.at(0).sourceIdentity == skill.sourceIdentity
               && first.pins.at(0).contentIdentity == skill.contentIdentity,
           "approving a reviewed record did not produce exactly one pin");

    // 规划出来的集合必须能被信任判定直接采纳。
    expect(ExtensionTrustPolicy::evaluate(skill, first.pins).state
               == ExtensionTrustState::Verified,
           "the planned review set did not verify its own target");

    // 批准只授予被批准的那一条，绝不能顺带覆盖别的扩展。
    const ExtensionRegistryRecord other =
        record(ExtensionKind::Mcp, QStringLiteral("fixture.mcp"), "b");
    expect(ExtensionTrustPolicy::evaluate(other, first.pins).state
               == ExtensionTrustState::Unverified,
           "approving one extension verified another");

    // 幂等：完全相同的批准不改变集合，因此不应提交。
    const ExtensionReviewPlan again = ExtensionReviewWorkflow::plan(
        approve(skill), {skill}, ready(first.pins, 4));
    expect(again.state == ExtensionReviewPlanState::Ready && !again.changed
               && again.pins.size() == 1 && again.expectedGeneration == 4,
           "re-approving identical content was not idempotent");

    // 追加第二个扩展保留第一个。
    const ExtensionReviewPlan second = ExtensionReviewWorkflow::plan(
        approve(other), {skill, other}, ready(first.pins, 4));
    expect(second.state == ExtensionReviewPlanState::Ready && second.changed
               && second.pins.size() == 2,
           "approving a second extension dropped the first");
    expect(ExtensionTrustPolicy::evaluate(skill, second.pins).state
               == ExtensionTrustState::Verified
               && ExtensionTrustPolicy::evaluate(other, second.pins).state
                   == ExtensionTrustState::Verified,
           "the two-pin set did not verify both targets");

    // 内容更新后重新批准是替换而不是追加：否则集合会变成信任判定必须拒绝的冲突。
    ExtensionRegistryRecord updated = skill;
    updated.contentIdentity = contentOf("a-v2");
    const ExtensionReviewPlan replaced = ExtensionReviewWorkflow::plan(
        approve(updated), {updated}, ready(first.pins, 7));
    expect(replaced.state == ExtensionReviewPlanState::Ready && replaced.changed
               && replaced.pins.size() == 1
               && replaced.pins.at(0).contentIdentity == updated.contentIdentity,
           "re-approving updated content appended a conflicting pin");
    expect(ExtensionTrustPolicy::evaluate(updated, replaced.pins).state
               == ExtensionTrustState::Verified
               && ExtensionTrustPolicy::evaluate(skill, replaced.pins).state
                   == ExtensionTrustState::Unverified,
           "replacing a pin did not move trust to the reviewed content");
}

void driftTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");

    // 人工在屏幕上看到的内容与现在要批准的内容不一致：必须失败，而不是把结论
    // 套用到当前内容上。这是整层最重要的性质。
    ExtensionRegistryRecord drifted = skill;
    drifted.contentIdentity = contentOf("a-tampered");
    expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {drifted}, empty()),
                    QStringLiteral("extension-review-content-drift")),
           "content that changed after review was approved anyway");

    ExtensionRegistryRecord resourced = skill;
    resourced.sourceIdentity = sourceOf("a-elsewhere");
    expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {resourced}, empty()),
                    QStringLiteral("extension-review-source-drift")),
           "the same content from a different source was approved anyway");

    // 批准一条不存在的记录等于预先授权将来出现的内容。
    expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {}, empty()),
                    QStringLiteral("extension-review-target-absent")),
           "an absent extension could be pre-approved");

    // 种类不同就是不同的扩展，即使 ID 相同。
    ExtensionRegistryRecord otherKind = skill;
    otherKind.kind = ExtensionKind::Mcp;
    expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {otherKind}, empty()),
                    QStringLiteral("extension-review-target-absent")),
           "a different kind with the same id satisfied the approval");

    // 清单里同一 (kind, id) 有多条说明来源不可信，不能任选一条批准。
    expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {skill, skill},
                                                 empty()),
                    QStringLiteral("extension-review-target-ambiguous")),
           "an ambiguous inventory allowed picking one record to approve");

    // 未安装的记录不能被批准。
    ExtensionRegistryRecord absent = skill;
    absent.installed = false;
    expect(rejected(ExtensionReviewWorkflow::plan(approve(absent), {absent}, empty()),
                    QStringLiteral("extension-review-target-not-installed")),
           "an uninstalled extension could be approved");

    // 复核请求自带的摘要不合法时，在触碰清单之前就必须拒绝：这样的摘要可能匹配
    // 到任何东西。
    ExtensionRegistryRecord unverifiable = skill;
    unverifiable.contentIdentity = QStringLiteral("extension-content:sha256:nope");
    ExtensionReviewRequest request = approve(skill);
    request.reviewedContentIdentity = unverifiable.contentIdentity;
    expect(rejected(ExtensionReviewWorkflow::plan(request, {unverifiable}, empty()),
                    QStringLiteral("extension-review-request-identity-invalid")),
           "an unverifiable identity was accepted as review evidence");

    // 记录自身的摘要不合法时同样不能批准，即使请求侧看起来一致：写进集合的是记录
    // 的摘要，一条不合法的摘要会让复核记录永远无法与任何内容对齐。
    ExtensionRegistryRecord malformed = skill;
    malformed.sourceIdentity = QStringLiteral("extension-source:sha256:short");
    ExtensionReviewRequest matching = approve(skill);
    expect(rejected(ExtensionReviewWorkflow::plan(matching, {malformed}, empty()),
                    QStringLiteral("extension-review-target-unverifiable")),
           "a record with a malformed identity could be approved");

    // 请求 ID 不合法时在触碰清单之前就必须拒绝。
    ExtensionReviewRequest badId = approve(skill);
    badId.id = QStringLiteral("Fixture/Skill");
    expect(rejected(ExtensionReviewWorkflow::plan(badId, {skill}, empty()),
                    QStringLiteral("extension-review-request-id-invalid")),
           "an invalid request id reached inventory matching");
}

void revocationTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");
    const ExtensionRegistryRecord other =
        record(ExtensionKind::Mcp, QStringLiteral("fixture.mcp"), "b");
    const ExtensionReviewPlan seeded = ExtensionReviewWorkflow::plan(
        approve(other), {skill, other},
        ready(ExtensionReviewWorkflow::plan(approve(skill), {skill}, empty()).pins, 2));
    if (!expect(seeded.pins.size() == 2, "the revocation fixture was not seeded")) {
        return;
    }

    // 撤销只移除目标，其余复核记录必须保留。
    const ExtensionReviewPlan removed = ExtensionReviewWorkflow::plan(
        revoke(skill.kind, skill.id), {skill, other}, ready(seeded.pins, 5));
    expect(removed.state == ExtensionReviewPlanState::Ready && removed.changed
               && removed.pins.size() == 1 && removed.expectedGeneration == 5
               && removed.pins.at(0).id == other.id,
           "revoking one extension did not preserve the others");
    expect(ExtensionTrustPolicy::evaluate(skill, removed.pins).state
               == ExtensionTrustState::Unverified
               && ExtensionTrustPolicy::evaluate(other, removed.pins).state
                   == ExtensionTrustState::Verified,
           "revocation did not remove trust from exactly its target");

    // 撤销不要求内容仍然一致：被篡改过的扩展必须仍然能被撤销，否则它将永远留在
    // 集合里。也不要求它还在清单里：已经删除的扩展同样必须能撤销。
    ExtensionRegistryRecord tampered = skill;
    tampered.contentIdentity = contentOf("a-tampered");
    expect(ExtensionReviewWorkflow::plan(revoke(skill.kind, skill.id),
                                         {tampered}, ready(seeded.pins, 5))
               .pins.size() == 1,
           "a tampered extension could not be revoked");
    const ExtensionReviewPlan gone = ExtensionReviewWorkflow::plan(
        revoke(skill.kind, skill.id), {}, ready(seeded.pins, 5));
    expect(gone.state == ExtensionReviewPlanState::Ready && gone.changed
               && gone.pins.size() == 1,
           "an uninstalled extension could not be revoked");

    // 撤销不存在的复核记录不改变集合，因此不应提交。
    const ExtensionReviewPlan noop = ExtensionReviewWorkflow::plan(
        revoke(ExtensionKind::CodexPlugin, QStringLiteral("fixture.plugin")),
        {skill, other}, ready(seeded.pins, 5));
    expect(noop.state == ExtensionReviewPlanState::Ready && !noop.changed
               && noop.pins.size() == 2,
           "revoking an unreviewed extension reported a change");

    // 清空最后一条复核记录是一次正常提交。
    const ExtensionReviewPlan emptied = ExtensionReviewWorkflow::plan(
        revoke(other.kind, other.id), {other},
        ready({removed.pins.at(0)}, 6));
    expect(emptied.state == ExtensionReviewPlanState::Ready && emptied.changed
               && emptied.pins.isEmpty(),
           "revoking the last pin was not a normal plan");

    // 请求 ID 不合法时撤销同样必须先拒绝。
    expect(rejected(ExtensionReviewWorkflow::plan(
                        revoke(skill.kind, QStringLiteral("BAD ID")),
                        {skill}, ready(seeded.pins, 5)),
                    QStringLiteral("extension-review-request-id-invalid")),
           "an invalid request id was accepted for revocation");
}

void ledgerStateTests()
{
    const ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");

    // 当前集合读不出来时不能规划：提交一份不完整的集合会静默删除读不出来的复核。
    for (const ExtensionReviewLedgerStoreState state : {
             ExtensionReviewLedgerStoreState::Invalid,
             ExtensionReviewLedgerStoreState::Unavailable,
             ExtensionReviewLedgerStoreState::OutcomeUnknown}) {
        ExtensionReviewLedgerStoreResult ledger;
        ledger.state = state;
        expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {skill}, ledger),
                        QStringLiteral("extension-review-ledger-unusable")),
               "an unusable ledger state was planned against");
        expect(rejected(ExtensionReviewWorkflow::plan(revoke(skill.kind, skill.id),
                                                      {skill}, ledger),
                        QStringLiteral("extension-review-ledger-unusable")),
               "an unusable ledger state allowed a revocation plan");
    }

    // 声称为空却带着内容的结果自相矛盾，不能作为规划依据。
    ExtensionReviewLedgerStoreResult lying = empty();
    lying.pins = ExtensionReviewWorkflow::plan(approve(skill), {skill}, empty()).pins;
    expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {skill}, lying),
                    QStringLiteral("extension-review-ledger-inconsistent")),
           "an empty ledger carrying pins was planned against");
    ExtensionReviewLedgerStoreResult negative = ready({}, -1);
    expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {skill}, negative),
                    QStringLiteral("extension-review-ledger-inconsistent")),
           "a negative generation was planned against");

    // 已存集合里有不合法记录时必须整体拒绝：带着它提交会把它洗成已认证的证据。
    ExtensionReviewPin broken;
    broken.kind = ExtensionKind::Skill;
    broken.id = QStringLiteral("fixture.other");
    broken.sourceIdentity = sourceOf("x");
    broken.contentIdentity = QStringLiteral("extension-content:sha256:nope");
    expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {skill},
                                                  ready({broken}, 3)),
                    QStringLiteral("extension-review-ledger-pin-invalid")),
           "a malformed existing pin was laundered into a new commit");
    expect(rejected(ExtensionReviewWorkflow::plan(revoke(skill.kind, skill.id),
                                                  {skill}, ready({broken}, 3)),
                    QStringLiteral("extension-review-ledger-pin-invalid")),
           "a malformed existing pin survived a revocation plan");

    // 已存集合本身冲突时"删掉哪一条"没有正确答案，必须拒绝。
    ExtensionReviewPin conflicting;
    conflicting.kind = ExtensionKind::Skill;
    conflicting.id = QStringLiteral("fixture.skill");
    conflicting.sourceIdentity = sourceOf("a");
    conflicting.contentIdentity = contentOf("a-other");
    const ExtensionReviewPin valid = ExtensionReviewWorkflow::plan(
        approve(skill), {skill}, empty()).pins.at(0);
    expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {skill},
                                                  ready({valid, conflicting}, 3)),
                    QStringLiteral("extension-review-ledger-conflict")),
           "a conflicting existing set was planned against");

    // 集合已满时新增必须失败，而不是丢弃一条已有复核记录。
    QList<ExtensionReviewPin> full;
    for (int i = 0; i < ExtensionTrustPolicy::MaxReviewPins; ++i) {
        ExtensionReviewPin pin;
        pin.kind = ExtensionKind::Mcp;
        pin.id = QStringLiteral("fixture.filler%1").arg(i);
        pin.sourceIdentity = sourceOf(QByteArray::number(i));
        pin.contentIdentity = contentOf(QByteArray::number(i));
        full.append(pin);
    }
    expect(rejected(ExtensionReviewWorkflow::plan(approve(skill), {skill},
                                                  ready(full, 9)),
                    QStringLiteral("extension-review-pin-limit")),
           "a full review set silently dropped a pin to make room");
    // 但在已满的集合上撤销必须仍然可用，否则集合将无法收缩。
    const ExtensionReviewPlan shrink = ExtensionReviewWorkflow::plan(
        revoke(ExtensionKind::Mcp, QStringLiteral("fixture.filler0")), {},
        ready(full, 9));
    expect(shrink.state == ExtensionReviewPlanState::Ready && shrink.changed
               && shrink.pins.size() == full.size() - 1,
           "a full review set could not be shrunk by revocation");
}

void authorityTests()
{
    // 规划不授予启用权：即使 Verified 且 Compatible，生效启用仍然需要独立动作。
    ExtensionRegistryRecord skill =
        record(ExtensionKind::Skill, QStringLiteral("fixture.skill"), "a");
    skill.compatibility = ExtensionCompatibilityState::Compatible;
    const ExtensionReviewPlan plan =
        ExtensionReviewWorkflow::plan(approve(skill), {skill}, empty());
    if (!expect(plan.state == ExtensionReviewPlanState::Ready,
                "the authority fixture could not be planned")) {
        return;
    }
    QList<ExtensionRegistryRecord> records{skill};
    ExtensionTrustPolicy::apply(&records, plan.pins);
    expect(records.at(0).trust == ExtensionTrustState::Verified
               && records.at(0).compatibility == ExtensionCompatibilityState::Compatible
               && !records.at(0).effectiveEnabled
               && !records.at(0).updateAvailable
               && !records.at(0).recoveryAvailable,
           "a completed review granted effective enablement");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    approvalTests();
    driftTests();
    revocationTests();
    ledgerStateTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension review workflow tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
