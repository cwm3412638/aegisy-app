#include "extension_recovery_presentation.h"

#include <QCoreApplication>
#include <QSet>
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

ExtensionRecoveryView view(ExtensionRecoveryNeed need,
                           bool confirmationRequired,
                           ExtensionEnablementLedgerStoreState state,
                           qint64 generation, int visibleGrants,
                           const QString &code)
{
    ExtensionRecoveryView value;
    value.assessment.need = need;
    value.assessment.operatorConfirmationRequired = confirmationRequired;
    value.assessment.errorCode = code;
    value.generation = generation;
    value.grantState = state;
    value.visibleGrants = visibleGrants;
    return value;
}

} // namespace

// 四个结论是四句不同的话,不是一个状态的四种程度。折叠成"有问题/没问题"会让读不到与结果
// 未知这两种情况也长出一个清空按钮,而那两种情况下清空会销毁看不见的授权。
void testFourConclusionsAreFourSentences()
{
    const ExtensionRecoveryPrompt none = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::None, false,
             ExtensionEnablementLedgerStoreState::Ready, 4, 2,
             QStringLiteral("extension-recovery-not-required")));
    const ExtensionRecoveryPrompt blocked = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::Blocked, false,
             ExtensionEnablementLedgerStoreState::Unavailable, 0, 0,
             QStringLiteral("extension-recovery-store-unavailable")));
    const ExtensionRecoveryPrompt reconfirm = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::Reconfirm, false,
             ExtensionEnablementLedgerStoreState::OutcomeUnknown, 0, 0,
             QStringLiteral("extension-recovery-outcome-unknown")));
    const ExtensionRecoveryPrompt clear = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::ClearGrants, true,
             ExtensionEnablementLedgerStoreState::Invalid, 0, 0,
             QStringLiteral("extension-recovery-evidence-invalid")));

    // 每一句话都必须与其他三句不同。相同的措辞意味着两个不同的结论在屏幕上无法区分。
    QSet<QString> headlines{none.headline, blocked.headline,
                            reconfirm.headline, clear.headline};
    expect(headlines.size() == 4,
           "two different recovery conclusions render the same sentence");
    QSet<QString> guidance{none.guidance, blocked.guidance,
                           reconfirm.guidance, clear.guidance};
    expect(guidance.size() == 4,
           "two different recovery conclusions give the same instruction");
    // 每一条路径都必须真的说了些什么:留空会让人以为界面出错了,于是去找别的路径清理。
    for (const ExtensionRecoveryPrompt *prompt : {&none, &blocked, &reconfirm, &clear}) {
        expect(!prompt->headline.isEmpty() && !prompt->guidance.isEmpty(),
               "a recovery conclusion rendered nothing at all");
        // 判定层的诊断原样带出。
        expect(!prompt->errorCode.isEmpty(),
               "a recovery conclusion carries no diagnostic to look up");
    }
    expect(none.errorCode == QStringLiteral("extension-recovery-not-required")
               && blocked.errorCode
                   == QStringLiteral("extension-recovery-store-unavailable")
               && reconfirm.errorCode
                   == QStringLiteral("extension-recovery-outcome-unknown")
               && clear.errorCode
                   == QStringLiteral("extension-recovery-evidence-invalid"),
           "the presentation invented its own diagnostic instead of passing one through");
    // 只有自相矛盾的账本可以推进动作。其余三种都必须是纯说明。
    expect(clear.state == ExtensionRecoveryPromptState::Actionable,
           "a self-contradictory ledger offers the operator no way forward");
    expect(none.state == ExtensionRecoveryPromptState::Informational
               && blocked.state == ExtensionRecoveryPromptState::Informational
               && reconfirm.state == ExtensionRecoveryPromptState::Informational,
           "a recovery action was offered where clearing destroys invisible grants");
}

// 一个能被确认的动作就是一个会被执行的动作。因此可确认性只能来自判定层:这一层自己再判
// 一遍必然会与它漂移,而漂移的方向是界面对一份读不到的账本提供清空动作。
void testConfirmabilityComesOnlyFromTheGate()
{
    // 判定层说不需要确认时,即使结论是"清空",界面也不得要求确认。这是刻意构造的矛盾输入:
    // 它证明这一层是转发而不是自己推导。
    const ExtensionRecoveryPrompt forwarded = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::ClearGrants, false,
             ExtensionEnablementLedgerStoreState::Invalid, 0, 0,
             QStringLiteral("extension-recovery-evidence-invalid")));
    expect(!forwarded.confirmationRequired,
           "the presentation re-derived confirmability instead of forwarding it");
    // 反过来也必须转发:判定层要求确认时不得被这一层降级。
    const ExtensionRecoveryPrompt required = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::ClearGrants, true,
             ExtensionEnablementLedgerStoreState::Invalid, 0, 0,
             QStringLiteral("extension-recovery-evidence-invalid")));
    expect(required.confirmationRequired,
           "a required confirmation was dropped, turning a clear into a silent one");
    // 读不到时绝不要求确认。
    const ExtensionRecoveryPrompt blocked = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::Blocked, false,
             ExtensionEnablementLedgerStoreState::Unavailable, 0, 0,
             QStringLiteral("extension-recovery-store-unavailable")));
    expect(!blocked.confirmationRequired,
           "an unreadable ledger asked the operator to confirm clearing it");
}

// 无法读出的授权条数不显示。那个数字是编出来的,而屏幕上的一个具体数字会让人相信自己知道
// 这次清空的范围。可读时即使是 0 也是真的 0,因此"已知"与"非零"是两件事。
void testUnknowableGrantCountIsNotShown()
{
    // 自相矛盾的账本:即使 view 里带着一个条数,也不得展示。
    const ExtensionRecoveryPrompt invalid = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::ClearGrants, true,
             ExtensionEnablementLedgerStoreState::Invalid, 0, 5,
             QStringLiteral("extension-recovery-evidence-invalid")));
    expect(!invalid.grantCountKnown,
           "a self-contradictory ledger reported a grant count it cannot know");
    expect(invalid.visibleGrants == 0,
           "an unknowable grant count leaked a number onto the screen");
    // 读不到时同样不得展示。
    const ExtensionRecoveryPrompt unavailable = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::Blocked, false,
             ExtensionEnablementLedgerStoreState::Unavailable, 0, 3,
             QStringLiteral("extension-recovery-store-unavailable")));
    expect(!unavailable.grantCountKnown && unavailable.visibleGrants == 0,
           "an unreadable ledger reported a grant count it cannot know");
    // 结果未知时同样不得展示。
    const ExtensionRecoveryPrompt unknown = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::Reconfirm, false,
             ExtensionEnablementLedgerStoreState::OutcomeUnknown, 0, 9,
             QStringLiteral("extension-recovery-outcome-unknown")));
    expect(!unknown.grantCountKnown && unknown.visibleGrants == 0,
           "an unknown outcome reported a grant count it cannot know");
    // 可读时必须展示,而且必须是真实的条数。
    const ExtensionRecoveryPrompt ready = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::None, false,
             ExtensionEnablementLedgerStoreState::Ready, 7, 2,
             QStringLiteral("extension-recovery-not-required")));
    expect(ready.grantCountKnown && ready.visibleGrants == 2,
           "a readable ledger hid the grant count it could actually report");
    // 可读的空账本:条数是真的 0,而"已知"必须为真。把它报成未知会让一份确实没有授权的
    // 账本看起来像读不出来。
    const ExtensionRecoveryPrompt empty = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::None, false,
             ExtensionEnablementLedgerStoreState::Empty, 0, 0,
             QStringLiteral("extension-recovery-not-required")));
    expect(empty.grantCountKnown && empty.visibleGrants == 0,
           "a readable empty ledger was reported as an unknowable count");
}

// 代号必须原样回传,包括 0。一份自相矛盾的账本没有可信代号,读出来就是 0;界面若改用自己
// 记着的上一个代号,判定层会以为界面看的是另一份账本而拒绝这次恢复。
void testGenerationIsForwardedVerbatim()
{
    const ExtensionRecoveryPrompt zero = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::ClearGrants, true,
             ExtensionEnablementLedgerStoreState::Invalid, 0, 0,
             QStringLiteral("extension-recovery-evidence-invalid")));
    expect(zero.expectedGeneration == 0,
           "a corrupt ledger's absent generation was replaced with something else");
    const ExtensionRecoveryPrompt nonZero = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::None, false,
             ExtensionEnablementLedgerStoreState::Ready, 11, 1,
             QStringLiteral("extension-recovery-not-required")));
    expect(nonZero.expectedGeneration == 11,
           "the generation the operator read was not forwarded verbatim");
}

// 恢复必须说出它不做什么。收回授权不删除磁盘上的内容,也不清除复核记录。人如果以为"恢复"
// 把扩展清理干净了就会停止清理,而内容还在原处;人如果以为复核记录也被清掉了,就会以为
// 失去了审计线索,而那份线索恰恰被完整保留着。
void testRecoveryStatesWhatItDoesNotDo()
{
    const ExtensionRecoveryNeed needs[] = {
        ExtensionRecoveryNeed::None, ExtensionRecoveryNeed::Blocked,
        ExtensionRecoveryNeed::Reconfirm, ExtensionRecoveryNeed::ClearGrants};
    // 每一条返回路径上都必须成立,而不是只在可动作那一条上。
    for (const ExtensionRecoveryNeed need : needs) {
        const ExtensionRecoveryPrompt prompt = ExtensionRecoveryPresentation::build(
            view(need, need == ExtensionRecoveryNeed::ClearGrants,
                 ExtensionEnablementLedgerStoreState::Invalid, 0, 0,
                 QStringLiteral("code")));
        expect(prompt.withdrawsAuthorityOnly,
               "a recovery prompt does not declare that it only withdraws authority");
        expect(!prompt.removesSourceContent,
               "a recovery prompt claims it deletes extension content from disk");
        expect(!prompt.clearsReviewRecords,
               "a recovery prompt claims it destroys the review audit evidence");
    }
    // 可动作的那一条还必须把这两件事写进给人看的文字里:一个只在结构体字段上为假、却没有
    // 在屏幕上说出口的不变量,对做决定的人不起任何作用。
    const ExtensionRecoveryPrompt clear = ExtensionRecoveryPresentation::build(
        view(ExtensionRecoveryNeed::ClearGrants, true,
             ExtensionEnablementLedgerStoreState::Invalid, 0, 0,
             QStringLiteral("extension-recovery-evidence-invalid")));
    expect(clear.guidance.contains(QStringLiteral("不会删除")),
           "the actionable prompt never says that no content is deleted");
    expect(clear.guidance.contains(QStringLiteral("复核记录")),
           "the actionable prompt never says the review records are kept");
}

// 未知结论按不可读处理,并且不提供任何动作:新增的判定结果不应默认长出一个清空按钮。
void testUnknownConclusionOffersNoAction()
{
    ExtensionRecoveryView unknown =
        view(ExtensionRecoveryNeed::ClearGrants, true,
             ExtensionEnablementLedgerStoreState::Ready, 3, 1, QString());
    // 构造一个不属于任何已知结论的值。
    unknown.assessment.need = static_cast<ExtensionRecoveryNeed>(99);
    const ExtensionRecoveryPrompt prompt =
        ExtensionRecoveryPresentation::build(unknown);
    expect(prompt.state == ExtensionRecoveryPromptState::Informational,
           "an unrecognized conclusion grew a clearing action by default");
    expect(prompt.need == ExtensionRecoveryNeed::Blocked,
           "an unrecognized conclusion was not treated as unreadable");
    expect(!prompt.confirmationRequired,
           "an unrecognized conclusion asked the operator to confirm something");
    expect(!prompt.grantCountKnown && prompt.visibleGrants == 0,
           "an unrecognized conclusion reported a grant count anyway");
    expect(!prompt.errorCode.isEmpty(),
           "an unrecognized conclusion carries no diagnostic to report");
    expect(prompt.withdrawsAuthorityOnly && !prompt.removesSourceContent
               && !prompt.clearsReviewRecords,
           "an unrecognized conclusion does not declare the invariants");
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    testFourConclusionsAreFourSentences();
    testConfirmabilityComesOnlyFromTheGate();
    testUnknowableGrantCountIsNotShown();
    testGenerationIsForwardedVerbatim();
    testRecoveryStatesWhatItDoesNotDo();
    testUnknownConclusionOffersNoAction();
    if (failures != 0) {
        QTextStream(stderr) << failures
                            << " extension recovery presentation guard(s) failed\n";
        return 1;
    }
    QTextStream(stdout) << "extension recovery presentation guards passed\n";
    return 0;
}
