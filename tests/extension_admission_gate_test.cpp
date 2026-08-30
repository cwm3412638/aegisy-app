#include "extension_admission_gate.h"

#include "extension_enablement_policy.h"
#include "extension_recovery_gate.h"

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

ExtensionEnablementPrompt promptFor(const ExtensionRegistryRecord &value,
                                    const QStringList &grants = granted())
{
    return ExtensionEnablementPresentation::build(value, grants, false, QString());
}

ExtensionApprovalAcknowledgement acknowledge(
    const ExtensionEnablementPrompt &prompt, bool highRiskConfirmed = false)
{
    ExtensionApprovalAcknowledgement value;
    value.decision = ExtensionApprovalDecision::Approve;
    value.kind = prompt.kind;
    value.id = prompt.identifier;
    value.approvedSourceIdentity = prompt.reviewedSourceIdentity;
    value.approvedContentIdentity = prompt.reviewedContentIdentity;
    value.acknowledgedWarnings = prompt.warnings;
    value.highRiskConfirmed = highRiskConfirmed;
    value.scope = ExtensionApprovalScope::OnceForThisContent;
    return value;
}

// 可读的授权账本。
ExtensionEnablementLedgerStoreResult ledger(
    ExtensionEnablementLedgerStoreState state
        = ExtensionEnablementLedgerStoreState::Empty)
{
    ExtensionEnablementLedgerStoreResult value;
    value.state = state;
    value.generation = 3;
    return value;
}

// 三个维度齐备、门禁已签署的沙箱结论。当前产品里并不存在,因此必须由测试构造。
SandboxVerdict enforcedSandbox()
{
    SandboxEnforcementEvidence evidence;
    evidence.platform = SandboxPlatform::MacOS;
    evidence.filesystem = SandboxEnforcement::Enforced;
    evidence.process = SandboxEnforcement::Enforced;
    evidence.network = SandboxEnforcement::Enforced;
    evidence.releaseGateSigned = true;
    return ExecutionSandboxGate::evaluate(evidence);
}

// 当前产品真实的沙箱结论:没有任何平台交付过强制,因此是只读回退。
SandboxVerdict currentSandbox()
{
    return ExecutionSandboxGate::evaluate(ExecutionSandboxGate::currentEvidence());
}

// 请求越出只读能力的记录:它的授权需要真实的强制才能成立。
ExtensionRegistryRecord writeCapableRecord()
{
    ExtensionRegistryRecord value = record();
    value.requestedCapabilities = QStringList{QStringLiteral("filesystem-write")};
    return value;
}

// 只读内容在四道门齐备时被准入。
void admittedTests()
{
    const ExtensionEnablementPrompt prompt = promptFor(record());
    expect(prompt.state == ExtensionEnablementPromptState::Ready,
           "the fixture prompt was not ready");
    const ExtensionAdmissionVerdict verdict = ExtensionAdmissionGate::evaluate(
        ledger(), prompt, acknowledge(prompt), enforcedSandbox());
    expect(verdict.state == ExtensionAdmissionState::Admitted,
           "content satisfying every gate was refused admission");
    expect(verdict.authorizedContentIdentity == record().contentIdentity,
           "admission does not bind the exact approved content identity");
    expect(verdict.errorCode.isEmpty(), "an admitted verdict carried an error code");
    // 只读内容不要求越出只读的强制。
    expect(verdict.requiredAuthority == SandboxExecutionAuthority::ReadOnly,
           "read-only content demanded write enforcement");

    // 关键:只读内容在当前产品的只读沙箱结论下仍然可以被准入。否则这一层会把"尚未交付
    // 强制"变成"连读取都不能授权",那是把门禁用错了方向。
    expect(ExtensionAdmissionGate::evaluate(
               ledger(), prompt, acknowledge(prompt), currentSandbox()).state
               == ExtensionAdmissionState::Admitted,
           "read-only content was refused because writes are not yet enforced");
}

// 这一层存在的理由:四道门里漏掉任何一道都必须失败。
void everyGateRequiredTests()
{
    const ExtensionEnablementPrompt prompt = promptFor(record());

    // 一、账本不可读:无法知道这次授予会加到什么之上,也无法做比较并交换。
    for (const ExtensionEnablementLedgerStoreState state : {
             ExtensionEnablementLedgerStoreState::Invalid,
             ExtensionEnablementLedgerStoreState::Unavailable,
             ExtensionEnablementLedgerStoreState::OutcomeUnknown}) {
        expect(ExtensionAdmissionGate::evaluate(
                   ledger(state), prompt, acknowledge(prompt),
                   enforcedSandbox()).errorCode
                   == QStringLiteral("extension-admission-ledger-unreadable"),
               "admission proceeded over an unreadable grant ledger");
    }
    // 恢复门禁与准入必须对"可读"用同一个定义。
    for (const ExtensionEnablementLedgerStoreState state : {
             ExtensionEnablementLedgerStoreState::Empty,
             ExtensionEnablementLedgerStoreState::Ready,
             ExtensionEnablementLedgerStoreState::Invalid,
             ExtensionEnablementLedgerStoreState::Unavailable,
             ExtensionEnablementLedgerStoreState::OutcomeUnknown}) {
        const bool admitted = ExtensionAdmissionGate::evaluate(
            ledger(state), prompt, acknowledge(prompt), enforcedSandbox()).state
            == ExtensionAdmissionState::Admitted;
        expect(admitted == ExtensionRecoveryGate::authoritative(state),
               "admission and recovery disagree about which ledgers are readable");
    }

    // 二、审批不构成授权。诊断代码必须原样透出,而不是被折叠成一个笼统的准入失败。
    ExtensionApprovalAcknowledgement declined = acknowledge(prompt);
    declined.decision = ExtensionApprovalDecision::Decline;
    expect(ExtensionAdmissionGate::evaluate(
               ledger(), prompt, declined, enforcedSandbox()).errorCode
               == QStringLiteral("extension-approval-declined"),
           "a declined approval was not surfaced with its own diagnostic");
    ExtensionApprovalAcknowledgement drifted = acknowledge(prompt);
    drifted.approvedContentIdentity = contentOf("different");
    expect(ExtensionAdmissionGate::evaluate(
               ledger(), prompt, drifted, enforcedSandbox()).errorCode
               == QStringLiteral("extension-approval-content-drift"),
           "admission accepted an approval bound to different content");

    // 呈现层未开门时准入也必须拒绝。
    ExtensionRegistryRecord unreviewed = record();
    unreviewed.trust = ExtensionTrustState::Unverified;
    const ExtensionEnablementPrompt blocked = promptFor(unreviewed);
    expect(ExtensionAdmissionGate::evaluate(
               ledger(), blocked, acknowledge(blocked), enforcedSandbox()).errorCode
               == QStringLiteral("extension-approval-prompt-blocked"),
           "admission accepted an approval against a blocked prompt");

    // 三、沙箱未强制。这是最容易被漏掉的一道:前两道通过后"一切看起来都已批准"。
    const ExtensionEnablementPrompt writePrompt = promptFor(
        writeCapableRecord(), QStringList{QStringLiteral("filesystem-write")});
    expect(writePrompt.warnings.contains(
               ExtensionEnablementWarning::CapabilityBeyondReadOnly),
           "the fixture did not disclose a beyond-read-only capability");
    expect(ExtensionAdmissionGate::evaluate(
               ledger(), writePrompt, acknowledge(writePrompt, true),
               currentSandbox()).errorCode
               == QStringLiteral("extension-admission-sandbox-unenforced"),
           "write-capable content was admitted with no enforced sandbox");
    // 同一份内容在强制齐备时被准入,证明拒绝确实来自沙箱而不是别的门。
    const ExtensionAdmissionVerdict enforced = ExtensionAdmissionGate::evaluate(
        ledger(), writePrompt, acknowledge(writePrompt, true), enforcedSandbox());
    expect(enforced.state == ExtensionAdmissionState::Admitted,
           "write-capable content was refused under an enforced sandbox");
    expect(enforced.requiredAuthority == SandboxExecutionAuthority::WorkspaceWrite,
           "write-capable content did not demand write enforcement");
}

// 越界内容不得被一个只允许只读的强制结论承载。
void authorityCoverageTests()
{
    const ExtensionEnablementPrompt writePrompt = promptFor(
        writeCapableRecord(), QStringList{QStringLiteral("filesystem-write")});

    // 一个自称已强制、但权限仍是只读的结论:强制存在不等于覆盖了所需级别。
    SandboxVerdict readOnlyEnforced = enforcedSandbox();
    readOnlyEnforced.authority = SandboxExecutionAuthority::ReadOnly;
    expect(ExtensionAdmissionGate::evaluate(
               ledger(), writePrompt, acknowledge(writePrompt, true),
               readOnlyEnforced).errorCode
               == QStringLiteral("extension-admission-authority-insufficient"),
           "write-capable content was carried by read-only enforcement");

    // 已证实的逃逸未修复时,可写内容不得被准入。
    SandboxEnforcementEvidence escaped;
    escaped.platform = SandboxPlatform::MacOS;
    escaped.filesystem = SandboxEnforcement::Enforced;
    escaped.process = SandboxEnforcement::Enforced;
    escaped.network = SandboxEnforcement::Enforced;
    escaped.releaseGateSigned = true;
    escaped.escapeRegressionOpen = true;
    expect(ExtensionAdmissionGate::evaluate(
               ledger(), writePrompt, acknowledge(writePrompt, true),
               ExecutionSandboxGate::evaluate(escaped)).state
               == ExtensionAdmissionState::Refused,
           "write-capable content was admitted despite a demonstrated escape");
}

// 所需强制级别读的是呈现给人的披露,而不是重新读取记录。
void requiredAuthorityTests()
{
    expect(ExtensionAdmissionGate::requiredAuthority(promptFor(record()))
               == SandboxExecutionAuthority::ReadOnly,
           "read-only disclosure demanded write enforcement");
    const ExtensionEnablementPrompt writePrompt = promptFor(
        writeCapableRecord(), QStringList{QStringLiteral("filesystem-write")});
    expect(ExtensionAdmissionGate::requiredAuthority(writePrompt)
               == SandboxExecutionAuthority::WorkspaceWrite,
           "a disclosed beyond-read-only capability demanded no enforcement");

    // 关键:渲染之后被改写的记录不得降低自己的强制要求。所需级别只看那份披露。
    ExtensionEnablementPrompt laundered = writePrompt;
    laundered.capabilities = QStringList{QStringLiteral("skill-content")};
    expect(ExtensionAdmissionGate::requiredAuthority(laundered)
               == SandboxExecutionAuthority::WorkspaceWrite,
           "rewriting the capability list lowered the enforcement requirement");
}

// 准入不放宽审批的规则判定,也不授予任何执行权限。
void authorityTests()
{
    const ExtensionEnablementPrompt prompt = promptFor(record());
    // 高风险在审批层已经被拒绝授予可复用规则,准入必须原样保留这个结论。
    const ExtensionEnablementPrompt writePrompt = promptFor(
        writeCapableRecord(), QStringList{QStringLiteral("filesystem-write")});
    ExtensionApprovalAcknowledgement remembered =
        acknowledge(writePrompt, true);
    remembered.scope = ExtensionApprovalScope::RememberForThisContent;
    expect(!ExtensionAdmissionGate::evaluate(
               ledger(), writePrompt, remembered, enforcedSandbox()).ruleGranted,
           "admission granted a reusable rule for high-risk content");

    // 低风险的可复用规则来自审批层,准入不额外授予也不撤销。
    ExtensionApprovalAcknowledgement lowRisk = acknowledge(prompt);
    lowRisk.scope = ExtensionApprovalScope::RememberForThisContent;
    expect(ExtensionAdmissionGate::evaluate(
               ledger(), prompt, lowRisk, enforcedSandbox()).ruleGranted,
           "admission dropped a rule the approval layer granted");

    // 一份被准入的判定本身不启用任何东西:启用仍然独立要求账本里存在授权。
    const ExtensionEnablementDecision decision =
        ExtensionEnablementPolicy::evaluate(record(), {});
    expect(!decision.enabled,
           "an admitted verdict enabled a record without a grant");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    admittedTests();
    everyGateRequiredTests();
    authorityCoverageTests();
    requiredAuthorityTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension admission gate tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
