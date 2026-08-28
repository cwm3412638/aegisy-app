#include "extension_recovery_gate.h"

#include <QCoreApplication>
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

ExtensionEnablementGrant grant()
{
    ExtensionEnablementGrant value;
    value.kind = ExtensionKind::Skill;
    value.id = QStringLiteral("acme.formatter");
    value.sourceIdentity = QStringLiteral("extension-source:sha256:")
        + QString(64, QLatin1Char('a'));
    value.contentIdentity = QStringLiteral("extension-content:sha256:")
        + QString(64, QLatin1Char('b'));
    return value;
}

ExtensionEnablementLedgerStoreResult ledger(
    ExtensionEnablementLedgerStoreState state, qint64 generation = 7)
{
    ExtensionEnablementLedgerStoreResult value;
    value.state = state;
    value.generation = generation;
    // 除了"从未授权过",每一种状态都携带一条授权。这一点是必要的:恢复"只收回授权,永不
    // 增加"的不变量只有在输入里确实存在可被复制的授权时才可被观察到。一份自相矛盾或读不到
    // 的账本同样可能带着部分解码出来的授权,而恢复绝不能把它们当作曾经有效的决定重建出来。
    if (state != ExtensionEnablementLedgerStoreState::Empty) {
        value.grants.append(grant());
    }
    return value;
}

ExtensionRecoveryRequest request(ExtensionRecoveryNeed need,
                                qint64 generation = 7)
{
    ExtensionRecoveryRequest value;
    value.acknowledgedNeed = need;
    value.expectedGeneration = generation;
    value.operatorConfirmed = true;
    return value;
}

// 四种存储状态是四个不同的结论,不能互相降级。
void assessmentTests()
{
    // 明确的账本没有需要恢复的损坏。对健康账本提供恢复等于提供一条批量撤销的后门。
    for (const ExtensionEnablementLedgerStoreState state : {
             ExtensionEnablementLedgerStoreState::Empty,
             ExtensionEnablementLedgerStoreState::Ready}) {
        const ExtensionRecoveryAssessment value =
            ExtensionRecoveryGate::assess(ledger(state));
        expect(value.need == ExtensionRecoveryNeed::None,
               "an authoritative ledger was offered a recovery action");
        expect(!value.operatorConfirmationRequired,
               "an authoritative ledger asked for recovery confirmation");
        expect(ExtensionRecoveryGate::authoritative(state),
               "an authoritative ledger state was classified as unreadable");
    }

    // 读不到内容时必须什么都不做:清空读不到的东西会销毁看不见的授权。
    const ExtensionRecoveryAssessment unavailable = ExtensionRecoveryGate::assess(
        ledger(ExtensionEnablementLedgerStoreState::Unavailable));
    expect(unavailable.need == ExtensionRecoveryNeed::Blocked,
           "an unreadable store was offered a destructive recovery action");
    expect(unavailable.errorCode
               == QStringLiteral("extension-recovery-store-unavailable"),
           "an unreadable store did not report why recovery is blocked");

    // 结果未知时必须重新读取,而不是在未知之上写入。
    const ExtensionRecoveryAssessment unknown = ExtensionRecoveryGate::assess(
        ledger(ExtensionEnablementLedgerStoreState::OutcomeUnknown));
    expect(unknown.need == ExtensionRecoveryNeed::Reconfirm,
           "an unknown write outcome was resolved by guessing");
    expect(unknown.errorCode
               == QStringLiteral("extension-recovery-outcome-unknown"),
           "an unknown write outcome was not reported as such");

    // 自相矛盾的证据允许在显式确认后收回全部授权。
    const ExtensionRecoveryAssessment invalid = ExtensionRecoveryGate::assess(
        ledger(ExtensionEnablementLedgerStoreState::Invalid));
    expect(invalid.need == ExtensionRecoveryNeed::ClearGrants,
           "corrupt evidence offered no operator path out");
    expect(invalid.operatorConfirmationRequired,
           "withdrawing every grant did not require explicit confirmation");
    expect(invalid.errorCode
               == QStringLiteral("extension-recovery-evidence-invalid"),
           "corrupt evidence was not reported as invalid");

    // 每一个结论都必须携带"只收回授权"的不变量。
    for (const ExtensionEnablementLedgerStoreState state : {
             ExtensionEnablementLedgerStoreState::Empty,
             ExtensionEnablementLedgerStoreState::Ready,
             ExtensionEnablementLedgerStoreState::Invalid,
             ExtensionEnablementLedgerStoreState::Unavailable,
             ExtensionEnablementLedgerStoreState::OutcomeUnknown}) {
        expect(ExtensionRecoveryGate::assess(ledger(state)).withdrawsAuthorityOnly,
               "a recovery assessment did not promise to withdraw authority only");
    }

    // 未知的存储状态必须按不可读处理,并且不提供任何恢复动作。
    const ExtensionEnablementLedgerStoreState bogus =
        static_cast<ExtensionEnablementLedgerStoreState>(9999);
    expect(!ExtensionRecoveryGate::authoritative(bogus),
           "an unclassified store state was treated as authoritative");
    expect(ExtensionRecoveryGate::assess(ledger(bogus)).need
               == ExtensionRecoveryNeed::Blocked,
           "an unclassified store state was offered a recovery action");
}

// 恢复不推断过去:唯一诚实的重建是空集合,而空集合是收回授权。
void planTests()
{
    const ExtensionEnablementLedgerStoreResult broken =
        ledger(ExtensionEnablementLedgerStoreState::Invalid);
    // 前提:输入确实带着一条授权。否则"计划为空"这件事无法区分"恢复收回了授权"与"输入
    // 本来就是空的",而那正是这一节要钉住的性质。
    expect(!broken.grants.isEmpty(),
           "the fixture ledger carried no grant for recovery to discard");
    const ExtensionRecoveryPlan plan = ExtensionRecoveryGate::plan(
        broken, request(ExtensionRecoveryNeed::ClearGrants));
    expect(plan.state == ExtensionRecoveryPlanState::Ready,
           "a confirmed recovery of corrupt evidence was refused");
    // 关键:恢复只能减少授权。任何非空结果都是在伪造从未被做出的授权决定。
    expect(plan.grants.isEmpty(),
           "recovery reconstructed grants nobody ever authorized");
    expect(plan.expectedGeneration == broken.generation,
           "recovery does not commit the generation the operator read");
    // 事务不在计划阶段清除:部分完成的恢复绝不能被当成已完成的恢复。
    expect(!plan.clearsTransaction,
           "recovery cleared the transaction before verifying the result");
    expect(plan.errorCode.isEmpty(), "a ready recovery plan carried an error code");

    // 收回全部授权需要显式确认:方向是减少,但它仍然是一次真实的授权变更。
    ExtensionRecoveryRequest unconfirmed =
        request(ExtensionRecoveryNeed::ClearGrants);
    unconfirmed.operatorConfirmed = false;
    expect(ExtensionRecoveryGate::plan(broken, unconfirmed).errorCode
               == QStringLiteral("extension-recovery-confirmation-required"),
           "recovery withdrew every grant without explicit confirmation");

    // 并发的授予不允许被恢复静默覆盖。
    expect(ExtensionRecoveryGate::plan(
               broken, request(ExtensionRecoveryNeed::ClearGrants, 6)).errorCode
               == QStringLiteral("extension-recovery-generation-stale"),
           "recovery overwrote a concurrent grant with a stale generation");
}

// 恢复不得作用在可读的账本上:那会变成一条不经审批就撤销一切的路径。
void authoritativeLedgerTests()
{
    for (const ExtensionEnablementLedgerStoreState state : {
             ExtensionEnablementLedgerStoreState::Empty,
             ExtensionEnablementLedgerStoreState::Ready}) {
        for (const ExtensionRecoveryNeed need : {
                 ExtensionRecoveryNeed::ClearGrants,
                 ExtensionRecoveryNeed::None,
                 ExtensionRecoveryNeed::Reconfirm,
                 ExtensionRecoveryNeed::Blocked}) {
            const ExtensionRecoveryPlan plan =
                ExtensionRecoveryGate::plan(ledger(state), request(need));
            expect(plan.state == ExtensionRecoveryPlanState::Refused
                       && plan.grants.isEmpty(),
                   "recovery acted on a readable ledger");
            expect(plan.errorCode
                       == QStringLiteral("extension-recovery-not-required"),
                   "recovery on a readable ledger was not refused as unnecessary");
        }
    }
}

// 读不到内容与结果未知都不允许写入。
void blockedTests()
{
    const ExtensionEnablementLedgerStoreResult unavailable =
        ledger(ExtensionEnablementLedgerStoreState::Unavailable);
    expect(ExtensionRecoveryGate::plan(
               unavailable, request(ExtensionRecoveryNeed::Blocked)).errorCode
               == QStringLiteral("extension-recovery-blocked"),
           "recovery wrote over a store whose contents it could not read");

    const ExtensionEnablementLedgerStoreResult pending =
        ledger(ExtensionEnablementLedgerStoreState::OutcomeUnknown);
    expect(ExtensionRecoveryGate::plan(
               pending, request(ExtensionRecoveryNeed::Reconfirm)).errorCode
               == QStringLiteral("extension-recovery-reread-required"),
           "recovery wrote on top of an unknown write outcome");

    // 两条路径都不得产出授权。
    for (const ExtensionEnablementLedgerStoreResult &blocked : {unavailable, pending}) {
        const ExtensionRecoveryPlan plan = ExtensionRecoveryGate::plan(
            blocked, request(ExtensionRecoveryGate::assess(blocked).need));
        expect(plan.state == ExtensionRecoveryPlanState::Refused
                   && plan.grants.isEmpty() && !plan.clearsTransaction,
               "a blocked recovery produced a plan");
    }
}

// 操作者确认的必须是当下真实的损坏,而不是界面上过期的结论。
void staleAssessmentTests()
{
    const ExtensionEnablementLedgerStoreResult unavailable =
        ledger(ExtensionEnablementLedgerStoreState::Unavailable);
    // 关键:一份针对"证据损坏"的确认不得被套用到"读不到内容"上。前者允许清空,后者绝不
    // 允许——把过期结论当成当下结论会让恢复销毁看不见的授权。
    expect(ExtensionRecoveryGate::plan(
               unavailable, request(ExtensionRecoveryNeed::ClearGrants)).errorCode
               == QStringLiteral("extension-recovery-assessment-stale"),
           "a clear-grants confirmation was applied to an unreadable store");

    const ExtensionEnablementLedgerStoreResult broken =
        ledger(ExtensionEnablementLedgerStoreState::Invalid);
    expect(ExtensionRecoveryGate::plan(
               broken, request(ExtensionRecoveryNeed::Reconfirm)).errorCode
               == QStringLiteral("extension-recovery-assessment-stale"),
           "a confirmation for another conclusion was accepted");
}

// 恢复只有在重新读取确实得到"从未授权过"时才算完成。
void completionTests()
{
    expect(ExtensionRecoveryGate::completed(
               ledger(ExtensionEnablementLedgerStoreState::Empty)),
           "a verified empty reread was not accepted as completed recovery");

    // 残留的授权、依旧的损坏、读不到与结果未知都必须让事务保持打开。
    for (const ExtensionEnablementLedgerStoreState state : {
             ExtensionEnablementLedgerStoreState::Ready,
             ExtensionEnablementLedgerStoreState::Invalid,
             ExtensionEnablementLedgerStoreState::Unavailable,
             ExtensionEnablementLedgerStoreState::OutcomeUnknown}) {
        expect(!ExtensionRecoveryGate::completed(ledger(state)),
               "a partial recovery was mistaken for a completed one");
    }

    // 状态说"从未授权过"但仍带着授权,是自相矛盾的读取,不能算完成。
    ExtensionEnablementLedgerStoreResult contradictory =
        ledger(ExtensionEnablementLedgerStoreState::Empty);
    contradictory.grants.append(grant());
    expect(!ExtensionRecoveryGate::completed(contradictory),
           "an empty state carrying grants was accepted as completed recovery");

    // 未知状态不算完成。
    expect(!ExtensionRecoveryGate::completed(
               ledger(static_cast<ExtensionEnablementLedgerStoreState>(9999))),
           "an unclassified reread state was accepted as completed recovery");
}

// 恢复门禁不读盘、不写盘、不执行任何东西:每一条可能的计划都只收回授权。
void authorityTests()
{
    for (const ExtensionEnablementLedgerStoreState state : {
             ExtensionEnablementLedgerStoreState::Empty,
             ExtensionEnablementLedgerStoreState::Ready,
             ExtensionEnablementLedgerStoreState::Invalid,
             ExtensionEnablementLedgerStoreState::Unavailable,
             ExtensionEnablementLedgerStoreState::OutcomeUnknown}) {
        for (const ExtensionRecoveryNeed need : {
                 ExtensionRecoveryNeed::None,
                 ExtensionRecoveryNeed::Blocked,
                 ExtensionRecoveryNeed::ClearGrants,
                 ExtensionRecoveryNeed::Reconfirm}) {
            for (const bool confirmed : {false, true}) {
                ExtensionRecoveryRequest value = request(need);
                value.operatorConfirmed = confirmed;
                const ExtensionEnablementLedgerStoreResult input = ledger(state);
                const ExtensionRecoveryPlan plan =
                    ExtensionRecoveryGate::plan(input, value);
                expect(input.grants.isEmpty()
                           == (state == ExtensionEnablementLedgerStoreState::Empty),
                       "the fixture did not offer a grant to be copied");
                // 唯一的不变量:没有任何输入组合能让恢复产出授权,或提前关闭事务。
                expect(plan.grants.isEmpty(),
                       "some recovery input produced a grant");
                expect(!plan.clearsTransaction,
                       "some recovery input closed the transaction early");
            }
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    assessmentTests();
    planTests();
    authoritativeLedgerTests();
    blockedTests();
    staleAssessmentTests();
    completionTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension recovery gate tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
