#include "extension_admission_gate.h"

#include "extension_recovery_gate.h"

namespace {

ExtensionAdmissionVerdict refuse(const QString &code)
{
    ExtensionAdmissionVerdict verdict;
    verdict.state = ExtensionAdmissionState::Refused;
    verdict.errorCode = code;
    return verdict;
}

} // namespace

SandboxExecutionAuthority ExtensionAdmissionGate::requiredAuthority(
    const ExtensionEnablementPrompt &prompt)
{
    // 读的是呈现给人的披露,而不是重新读取记录。一条在渲染之后被改写的记录不得借此降低
    // 自己的强制要求:人看到的那份披露才是这次授权的范围。
    for (const ExtensionEnablementWarning warning : prompt.warnings) {
        if (warning == ExtensionEnablementWarning::CapabilityBeyondReadOnly) {
            return SandboxExecutionAuthority::WorkspaceWrite;
        }
    }
    return SandboxExecutionAuthority::ReadOnly;
}

ExtensionAdmissionVerdict ExtensionAdmissionGate::evaluate(
    const ExtensionEnablementLedgerStoreResult &ledger,
    const ExtensionEnablementPrompt &prompt,
    const ExtensionApprovalAcknowledgement &acknowledgement,
    const SandboxVerdict &sandbox)
{
    // 一、账本必须当下可读。授权集合读不到时无法知道这次授予会加到什么之上,也无法在
    // 提交时做比较并交换。恢复门禁的结论在这里被直接采用,而不是重新判断一次:两处各自
    // 判断会让两个地方对"可读"的定义漂移。
    if (!ExtensionRecoveryGate::authoritative(ledger.state)) {
        return refuse(QStringLiteral("extension-admission-ledger-unreadable"));
    }

    // 二、审批必须构成有效授权。这里不重新实现对齐检查:审批层已经逐项判定过,重复实现
    // 会产生两套可能漂移的规则。
    const ExtensionApprovalVerdict approval =
        ExtensionApprovalPolicy::evaluate(prompt, acknowledgement);
    if (approval.state != ExtensionApprovalState::Authorized) {
        // 审批的诊断代码原样透出:准入不掩盖具体哪一项没有对齐。
        return refuse(approval.errorCode);
    }

    // 三、沙箱必须能强制这份内容所需要的边界。这是最容易被漏掉的一道门,因为前两道门
    // 通过之后"一切看起来都已批准"——但被批准的意图在没有强制的机器上依然是没有边界的。
    const SandboxExecutionAuthority required = requiredAuthority(prompt);
    if (ExecutionSandboxGate::beyondReadOnly(required)) {
        if (sandbox.state != SandboxVerdictState::Enforced) {
            return refuse(QStringLiteral("extension-admission-sandbox-unenforced"));
        }
        // 强制存在还不够:它必须至少覆盖这份内容需要的级别。一个只允许只读的强制结论
        // 不能承载一份请求写入的授权。
        if (!ExecutionSandboxGate::beyondReadOnly(sandbox.authority)) {
            return refuse(QStringLiteral("extension-admission-authority-insufficient"));
        }
    }

    ExtensionAdmissionVerdict verdict;
    verdict.state = ExtensionAdmissionState::Admitted;
    verdict.authorizedContentIdentity = approval.authorizedContentIdentity;
    // 可复用规则的判定完全来自审批层。准入不放宽它:高风险在那里已经被拒绝授予规则。
    verdict.ruleGranted = approval.ruleGranted;
    verdict.requiredAuthority = required;
    return verdict;
}
