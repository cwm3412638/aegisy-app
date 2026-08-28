#include "extension_approval_policy.h"

#include "extension_display_safety.h"

#include <QSet>

namespace {

using Safety = ExtensionDisplaySafety;

ExtensionApprovalVerdict refuse(const QString &code)
{
    ExtensionApprovalVerdict verdict;
    verdict.state = ExtensionApprovalState::Refused;
    verdict.errorCode = code;
    return verdict;
}

} // namespace

bool ExtensionApprovalPolicy::requiresExplicitConfirmation(
    ExtensionEnablementWarning warning)
{
    switch (warning) {
    // 越界与越权能力是当前只读边界之外的授权，因此必须逐次确认，不能由一条记住的
    // 规则批量放行。
    case ExtensionEnablementWarning::CapabilityBeyondReadOnly:
    case ExtensionEnablementWarning::CapabilityNotGranted:
    // 内容自上次授权后发生变化时，这是一次全新的授权决定，不是对旧决定的沿用。
    case ExtensionEnablementWarning::ContentChangedSinceGrant:
    // 名称与标识不一致是冒充另一个已被授权扩展的手法，不能被记住的规则吸收。
    case ExtensionEnablementWarning::NameMismatchesIdentifier:
        return true;
    case ExtensionEnablementWarning::VersionUnknown:
    case ExtensionEnablementWarning::AlreadyGranted:
    case ExtensionEnablementWarning::GrantDoesNotExecuteYet:
        return false;
    }
    // 未知风险按需要确认处理：新增的风险类别不应默认变成可批量放行的。
    return true;
}

ExtensionApprovalVerdict ExtensionApprovalPolicy::evaluate(
    const ExtensionEnablementPrompt &prompt,
    const ExtensionApprovalAcknowledgement &acknowledgement)
{
    // 拒绝就是拒绝：它不产生任何授权，也不留下规则。先于其他检查处理，因为一个格式
    // 有问题的拒绝不应该被报告成"批准失败"。
    if (acknowledgement.decision == ExtensionApprovalDecision::Decline) {
        ExtensionApprovalVerdict verdict;
        verdict.state = ExtensionApprovalState::Refused;
        verdict.errorCode = QStringLiteral("extension-approval-declined");
        return verdict;
    }

    // 不可展示的提问不能被批准：人不可能看过一份无法呈现的内容。
    if (prompt.state == ExtensionEnablementPromptState::Unpresentable) {
        return refuse(QStringLiteral("extension-approval-prompt-unpresentable"));
    }
    // 门禁未满足时批准无效。呈现层已经拒绝提供动作，因此走到这里的批准要么来自过期的
    // 界面，要么是伪造的——两种情况都不能通过。
    if (prompt.state != ExtensionEnablementPromptState::Ready) {
        return refuse(QStringLiteral("extension-approval-prompt-blocked"));
    }

    if (!Safety::validId(acknowledgement.id)
            || acknowledgement.id != prompt.identifier
            || acknowledgement.kind != prompt.kind) {
        return refuse(QStringLiteral("extension-approval-target-mismatch"));
    }

    // 批准绑定的是屏幕上那份确切内容。渲染之后发生漂移时批准失效，而不是被套用到新
    // 内容上：那正是把"我看过这份内容"变成"我批准了任何后续内容"的路径。
    if (!Safety::hashIdentity(acknowledgement.approvedContentIdentity,
                              QStringLiteral("extension-content:sha256:"))
            || !Safety::hashIdentity(acknowledgement.approvedSourceIdentity,
                                     QStringLiteral("extension-source:sha256:"))) {
        return refuse(QStringLiteral("extension-approval-identity-invalid"));
    }
    if (acknowledgement.approvedContentIdentity != prompt.reviewedContentIdentity) {
        return refuse(QStringLiteral("extension-approval-content-drift"));
    }
    if (acknowledgement.approvedSourceIdentity != prompt.reviewedSourceIdentity) {
        return refuse(QStringLiteral("extension-approval-source-drift"));
    }

    // 批准的是"我看到了这些风险并接受"。当前提问披露的每一项风险都必须在回传的集合里
    // 出现，否则这份批准对应的是一个风险更少的界面。
    const QSet<ExtensionEnablementWarning> acknowledged(
        acknowledgement.acknowledgedWarnings.cbegin(),
        acknowledgement.acknowledgedWarnings.cend());
    if (acknowledged.size() != acknowledgement.acknowledgedWarnings.size()) {
        return refuse(QStringLiteral("extension-approval-warning-duplicate"));
    }
    bool requiresConfirmation = false;
    for (const ExtensionEnablementWarning warning : prompt.warnings) {
        if (!acknowledged.contains(warning)) {
            return refuse(QStringLiteral("extension-approval-warning-undisclosed"));
        }
        if (requiresExplicitConfirmation(warning)) requiresConfirmation = true;
    }
    // 回传了当前并未披露的风险，说明这份批准来自另一个界面状态。
    for (const ExtensionEnablementWarning warning
             : acknowledgement.acknowledgedWarnings) {
        if (!prompt.warnings.contains(warning)) {
            return refuse(QStringLiteral("extension-approval-warning-unknown"));
        }
    }

    // 高风险不接受概括性批准：必须逐次显式确认。
    if (requiresConfirmation && !acknowledgement.highRiskConfirmed) {
        return refuse(QStringLiteral("extension-approval-confirmation-required"));
    }

    ExtensionApprovalVerdict verdict;
    verdict.state = ExtensionApprovalState::Authorized;
    verdict.authorizedContentIdentity = prompt.reviewedContentIdentity;
    // 可复用规则永远不比被复核的那份内容更宽，并且高风险不产生规则：一条记住的规则会在
    // 下一次自动放行，而高风险必须每次都由人确认。
    verdict.ruleGranted =
        acknowledgement.scope == ExtensionApprovalScope::RememberForThisContent
        && !requiresConfirmation;
    return verdict;
}
