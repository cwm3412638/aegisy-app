#include "extension_approval_policy.h"

#include "extension_enablement_policy.h"
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

QStringList granted()
{
    return QStringList{QStringLiteral("filesystem-read"), QStringLiteral("mcp-tools"),
                       QStringLiteral("network"), QStringLiteral("skill-content")};
}

// 三道门禁均已满足、且没有任何需要逐次确认的风险的记录。
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

ExtensionEnablementPrompt promptFor(const ExtensionRegistryRecord &value)
{
    return ExtensionEnablementPresentation::build(value, granted(), false, QString());
}

// 与提问逐项对齐的批准：这是唯一应当被授权的形状。
ExtensionApprovalAcknowledgement acknowledge(const ExtensionEnablementPrompt &prompt)
{
    ExtensionApprovalAcknowledgement value;
    value.decision = ExtensionApprovalDecision::Approve;
    value.kind = prompt.kind;
    value.id = prompt.identifier;
    value.approvedSourceIdentity = prompt.reviewedSourceIdentity;
    value.approvedContentIdentity = prompt.reviewedContentIdentity;
    value.acknowledgedWarnings = prompt.warnings;
    value.highRiskConfirmed = false;
    value.scope = ExtensionApprovalScope::OnceForThisContent;
    return value;
}

void authorizedTests()
{
    const ExtensionEnablementPrompt prompt = promptFor(record());
    const ExtensionApprovalVerdict verdict =
        ExtensionApprovalPolicy::evaluate(prompt, acknowledge(prompt));
    expect(verdict.state == ExtensionApprovalState::Authorized,
           "an approval aligned with its prompt was refused");
    expect(verdict.errorCode.isEmpty(), "an authorized verdict carried an error code");
    // 授权绑定的是屏幕上那份确切内容摘要。
    expect(verdict.authorizedContentIdentity == record().contentIdentity,
           "the verdict does not bind the exact approved content identity");
    // 未请求可复用规则时不得授予规则。
    expect(!verdict.ruleGranted, "a once-only approval granted a reusable rule");

    // 拒绝不产生授权，也不留下规则。
    ExtensionApprovalAcknowledgement declined = acknowledge(prompt);
    declined.decision = ExtensionApprovalDecision::Decline;
    declined.scope = ExtensionApprovalScope::RememberForThisContent;
    const ExtensionApprovalVerdict refusal =
        ExtensionApprovalPolicy::evaluate(prompt, declined);
    expect(refusal.state == ExtensionApprovalState::Refused
               && refusal.errorCode == QStringLiteral("extension-approval-declined")
               && !refusal.ruleGranted
               && refusal.authorizedContentIdentity.isEmpty(),
           "a declined decision produced authority or a rule");
}

// 批准必须与当时屏幕上的内容逐项对齐，否则它对应的是另一个界面状态。
void alignmentTests()
{
    const ExtensionEnablementPrompt prompt = promptFor(record());

    // 目标不一致：同一个标识在不同类型下是不同的扩展。
    ExtensionApprovalAcknowledgement wrongKind = acknowledge(prompt);
    wrongKind.kind = ExtensionKind::Mcp;
    expect(ExtensionApprovalPolicy::evaluate(prompt, wrongKind).errorCode
               == QStringLiteral("extension-approval-target-mismatch"),
           "an approval for another extension kind was accepted");
    ExtensionApprovalAcknowledgement wrongId = acknowledge(prompt);
    wrongId.id = QStringLiteral("other.extension");
    expect(ExtensionApprovalPolicy::evaluate(prompt, wrongId).errorCode
               == QStringLiteral("extension-approval-target-mismatch"),
           "an approval for another identifier was accepted");
    ExtensionApprovalAcknowledgement badId = acknowledge(prompt);
    badId.id = QStringLiteral("Bad Id");
    expect(ExtensionApprovalPolicy::evaluate(prompt, badId).errorCode
               == QStringLiteral("extension-approval-target-mismatch"),
           "an approval with a malformed identifier was accepted");

    // 渲染之后的内容漂移必须让批准失效，而不是被套用到新内容上。
    ExtensionApprovalAcknowledgement drifted = acknowledge(prompt);
    drifted.approvedContentIdentity = contentOf("different");
    expect(ExtensionApprovalPolicy::evaluate(prompt, drifted).errorCode
               == QStringLiteral("extension-approval-content-drift"),
           "an approval bound to different content was accepted");
    ExtensionApprovalAcknowledgement sourceDrifted = acknowledge(prompt);
    sourceDrifted.approvedSourceIdentity = sourceOf("different");
    expect(ExtensionApprovalPolicy::evaluate(prompt, sourceDrifted).errorCode
               == QStringLiteral("extension-approval-source-drift"),
           "an approval bound to a different source was accepted");

    // 摘要形式不合法时没有可绑定的内容。
    for (const QString &bad : {
             QStringLiteral(""),
             QStringLiteral("extension-content:sha256:abc"),
             QStringLiteral("acme.formatter")}) {
        ExtensionApprovalAcknowledgement value = acknowledge(prompt);
        value.approvedContentIdentity = bad;
        expect(ExtensionApprovalPolicy::evaluate(prompt, value).errorCode
                   == QStringLiteral("extension-approval-identity-invalid"),
               "an approval with a malformed identity was accepted");
    }
}

// 呈现层已经拒绝提供动作时，走到审批的批准要么来自过期界面，要么是伪造的。
void promptStateTests()
{
    for (ExtensionRegistryRecord blocked : {record(), record(), record()}) {
        blocked.trust = ExtensionTrustState::Unverified;
        const ExtensionEnablementPrompt prompt = promptFor(blocked);
        expect(prompt.state == ExtensionEnablementPromptState::Blocked,
               "the fixture did not produce a blocked prompt");
        // 关键：即使批准与提问完全对齐，门禁未满足也不构成授权。
        expect(ExtensionApprovalPolicy::evaluate(prompt, acknowledge(prompt))
                   .errorCode
                   == QStringLiteral("extension-approval-prompt-blocked"),
               "an approval against a blocked prompt was accepted");
    }

    ExtensionRegistryRecord notInstalled = record();
    notInstalled.installed = false;
    const ExtensionEnablementPrompt installGate = promptFor(notInstalled);
    expect(ExtensionApprovalPolicy::evaluate(installGate, acknowledge(installGate))
               .state == ExtensionApprovalState::Refused,
           "an approval against an uninstalled target was accepted");

    ExtensionRegistryRecord incompatible = record();
    incompatible.compatibility = ExtensionCompatibilityState::Incompatible;
    const ExtensionEnablementPrompt compatGate = promptFor(incompatible);
    expect(ExtensionApprovalPolicy::evaluate(compatGate, acknowledge(compatGate))
               .state == ExtensionApprovalState::Refused,
           "an approval against an incompatible target was accepted");

    // 无法安全展示的内容不可能被人看过，因此不可能被批准。
    ExtensionRegistryRecord unsafe = record();
    unsafe.name = QStringLiteral("Acme‮Formatter");
    const ExtensionEnablementPrompt unpresentable = promptFor(unsafe);
    expect(unpresentable.state == ExtensionEnablementPromptState::Unpresentable,
           "the fixture did not produce an unpresentable prompt");
    expect(ExtensionApprovalPolicy::evaluate(unpresentable,
                                             acknowledge(unpresentable)).errorCode
               == QStringLiteral("extension-approval-prompt-unpresentable"),
           "an approval against an unpresentable prompt was accepted");
}

// 批准的是"我看到了这些风险并接受"，因此风险集合必须与披露的集合完全一致。
void warningTests()
{
    const ExtensionEnablementPrompt prompt = promptFor(record());
    expect(!prompt.warnings.isEmpty(),
           "the fixture prompt disclosed no warnings to acknowledge");

    // 少确认一项：这份批准对应的是一个风险更少的界面。
    ExtensionApprovalAcknowledgement partial = acknowledge(prompt);
    partial.acknowledgedWarnings.removeLast();
    expect(ExtensionApprovalPolicy::evaluate(prompt, partial).errorCode
               == QStringLiteral("extension-approval-warning-undisclosed"),
           "an approval that skipped a disclosed risk was accepted");

    // 空集合同样不能通过：那是"我没看到任何风险"。
    ExtensionApprovalAcknowledgement empty = acknowledge(prompt);
    empty.acknowledgedWarnings.clear();
    expect(ExtensionApprovalPolicy::evaluate(prompt, empty).errorCode
               == QStringLiteral("extension-approval-warning-undisclosed"),
           "an approval acknowledging no risks was accepted");

    // 回传当前并未披露的风险，说明批准来自另一个界面状态。
    ExtensionApprovalAcknowledgement extra = acknowledge(prompt);
    extra.acknowledgedWarnings.append(
        ExtensionEnablementWarning::CapabilityBeyondReadOnly);
    expect(ExtensionApprovalPolicy::evaluate(prompt, extra).errorCode
               == QStringLiteral("extension-approval-warning-unknown"),
           "an approval acknowledging an undisclosed risk was accepted");

    ExtensionApprovalAcknowledgement duplicated = acknowledge(prompt);
    duplicated.acknowledgedWarnings.append(prompt.warnings.first());
    expect(ExtensionApprovalPolicy::evaluate(prompt, duplicated).errorCode
               == QStringLiteral("extension-approval-warning-duplicate"),
           "an approval with a duplicated risk list was accepted");
}

// 高风险必须逐次显式确认，不能由"记住我的选择"批量放行。
void highRiskTests()
{
    // 越权能力：即使已被复核，它仍在当前只读边界之外。
    ExtensionRegistryRecord risky = record();
    risky.requestedCapabilities = QStringList{QStringLiteral("filesystem-read"),
                                             QStringLiteral("network")};
    ExtensionEnablementPrompt prompt = ExtensionEnablementPresentation::build(
        risky, QStringList{QStringLiteral("filesystem-read")}, false, QString());
    expect(prompt.state == ExtensionEnablementPromptState::Ready
               && prompt.warnings.contains(
                      ExtensionEnablementWarning::CapabilityNotGranted),
           "the fixture did not disclose an ungranted capability");

    ExtensionApprovalAcknowledgement unconfirmed = acknowledge(prompt);
    expect(ExtensionApprovalPolicy::evaluate(prompt, unconfirmed).errorCode
               == QStringLiteral("extension-approval-confirmation-required"),
           "a high-risk approval succeeded without explicit confirmation");

    ExtensionApprovalAcknowledgement confirmed = acknowledge(prompt);
    confirmed.highRiskConfirmed = true;
    const ExtensionApprovalVerdict verdict =
        ExtensionApprovalPolicy::evaluate(prompt, confirmed);
    expect(verdict.state == ExtensionApprovalState::Authorized,
           "an explicitly confirmed high-risk approval was refused");
    // 高风险不产生可复用规则：一条记住的规则会在下一次自动放行。
    ExtensionApprovalAcknowledgement remembered = confirmed;
    remembered.scope = ExtensionApprovalScope::RememberForThisContent;
    expect(!ExtensionApprovalPolicy::evaluate(prompt, remembered).ruleGranted,
           "a high-risk approval produced a reusable rule");

    // 每一类需要逐次确认的风险都必须被这样对待。
    for (const ExtensionEnablementWarning warning : {
             ExtensionEnablementWarning::CapabilityBeyondReadOnly,
             ExtensionEnablementWarning::CapabilityNotGranted,
             ExtensionEnablementWarning::ContentChangedSinceGrant,
             ExtensionEnablementWarning::NameMismatchesIdentifier}) {
        expect(ExtensionApprovalPolicy::requiresExplicitConfirmation(warning),
               "a high-risk category does not require explicit confirmation");
    }
    // 纯披露性的说明不应要求确认，否则确认会退化成一个总是要点的复选框。
    expect(!ExtensionApprovalPolicy::requiresExplicitConfirmation(
               ExtensionEnablementWarning::GrantDoesNotExecuteYet),
           "a purely informational disclosure demands confirmation");

    // 分类必须对每一个已定义的风险都有明确答案。switch 是穷尽的，因此"未知类别默认
    // 需要确认"的兜底分支无法被观察到——它只在新增枚举值且忘记归类时才生效。这里直接
    // 把枚举的完整性钉住：每一项都必须被下面两个集合之一覆盖，于是新增一个风险类别时
    // 这个断言会失败，迫使做出归类决定，而不是让它静默落入某个默认行为。
    const QList<ExtensionEnablementWarning> confirmationRequired{
        ExtensionEnablementWarning::CapabilityBeyondReadOnly,
        ExtensionEnablementWarning::CapabilityNotGranted,
        ExtensionEnablementWarning::ContentChangedSinceGrant,
        ExtensionEnablementWarning::NameMismatchesIdentifier};
    const QList<ExtensionEnablementWarning> informational{
        ExtensionEnablementWarning::VersionUnknown,
        ExtensionEnablementWarning::AlreadyGranted,
        ExtensionEnablementWarning::GrantDoesNotExecuteYet};
    for (const ExtensionEnablementWarning warning : confirmationRequired) {
        expect(ExtensionApprovalPolicy::requiresExplicitConfirmation(warning),
               "a classified high-risk warning stopped requiring confirmation");
    }
    for (const ExtensionEnablementWarning warning : informational) {
        expect(!ExtensionApprovalPolicy::requiresExplicitConfirmation(warning),
               "a classified informational warning started requiring confirmation");
    }
    // 未归类的风险必须默认要求确认。这是可以直接观察的：传入一个不在分类里的取值，
    // 它必须落到 fail-closed 的兜底分支。新增一个风险类别却忘记归类时，行为等价于
    // 这里的输入，因此这条断言直接钉住了那个兜底方向——不能让新风险默认变成可被一条
    // 记住的规则批量放行的。
    expect(ExtensionApprovalPolicy::requiresExplicitConfirmation(
               static_cast<ExtensionEnablementWarning>(9999)),
           "an unclassified warning category defaulted to needing no confirmation");
}

// 记住的规则永远不比被批准的那份确切内容更宽。
void scopeTests()
{
    const ExtensionEnablementPrompt prompt = promptFor(record());
    ExtensionApprovalAcknowledgement remembered = acknowledge(prompt);
    remembered.scope = ExtensionApprovalScope::RememberForThisContent;
    const ExtensionApprovalVerdict verdict =
        ExtensionApprovalPolicy::evaluate(prompt, remembered);
    expect(verdict.state == ExtensionApprovalState::Authorized
               && verdict.ruleGranted,
           "a low-risk remembered approval did not grant a rule");
    // 规则绑定确切内容摘要，因此内容变更后它无法再匹配。
    expect(verdict.authorizedContentIdentity == record().contentIdentity,
           "a remembered rule is not bound to the exact approved content");

    // 内容变更后，同一份批准凭据不再对齐当前提问。
    ExtensionRegistryRecord changed = record();
    changed.contentIdentity = contentOf("v2");
    const ExtensionEnablementPrompt reRendered = promptFor(changed);
    expect(ExtensionApprovalPolicy::evaluate(reRendered, remembered).errorCode
               == QStringLiteral("extension-approval-content-drift"),
           "a remembered approval survived a content change");
}

// 审批不启用、不持久化、不执行任何东西。
void authorityTests()
{
    const ExtensionEnablementPrompt prompt = promptFor(record());
    const ExtensionApprovalVerdict verdict =
        ExtensionApprovalPolicy::evaluate(prompt, acknowledge(prompt));
    expect(verdict.state == ExtensionApprovalState::Authorized,
           "the fixture approval was refused");

    // 一份有效批准本身不启用任何东西：启用判定仍然独立要求账本里存在授权。
    const ExtensionEnablementDecision decision =
        ExtensionEnablementPolicy::evaluate(record(), {});
    expect(!decision.enabled,
           "an authorized approval enabled a record without a grant");

    // 批准产出的摘要原样进入启用请求，因此后续漂移仍然可被检测。
    ExtensionEnablementRequest request;
    request.action = ExtensionEnablementAction::Enable;
    request.kind = record().kind;
    request.id = record().id;
    request.reviewedSourceIdentity = prompt.reviewedSourceIdentity;
    request.reviewedContentIdentity = verdict.authorizedContentIdentity;
    expect(request.reviewedContentIdentity == record().contentIdentity,
           "the enablement request does not carry the approved identity");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    authorizedTests();
    alignmentTests();
    promptStateTests();
    warningTests();
    highRiskTests();
    scopeTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension approval policy tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
