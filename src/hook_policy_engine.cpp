#include "hook_policy_engine.h"

#include "extension_display_safety.h"

namespace {

using Safety = ExtensionDisplaySafety;

HookVerdict reject(const QString &code)
{
    HookVerdict verdict;
    verdict.state = HookVerdictState::DeclarationRejected;
    // 声明不可用时目标工具不执行。一个无法审查的钩子契约不应等于没有钩子:那会让删掉
    // 契约里的一行成为放行的办法。
    verdict.toolMayExecute = false;
    verdict.errorCode = code;
    return verdict;
}

} // namespace

QString HookPolicyEngine::eventLabel(HookLifecycleEvent event)
{
    switch (event) {
    case HookLifecycleEvent::PreToolUse:
        return QStringLiteral("工具执行前");
    case HookLifecycleEvent::PostToolUse:
        return QStringLiteral("工具执行后");
    case HookLifecycleEvent::SessionStart:
        return QStringLiteral("会话开始");
    case HookLifecycleEvent::UserPromptSubmit:
        return QStringLiteral("提交提示时");
    case HookLifecycleEvent::Stop:
        return QStringLiteral("停止时");
    }
    return QStringLiteral("未知事件");
}

bool HookPolicyEngine::failOpenPermitted(const HookDeclaration &declaration)
{
    // 受管安全钩子必须失败关闭。一个用来阻止危险操作的钩子如果在崩溃时放行,那么让它
    // 崩溃就成了绕过它的方法——于是这个钩子提供的保护等于零。
    if (declaration.provenance == HookProvenance::Managed
            && declaration.securityControl) {
        return false;
    }
    return declaration.failureBehavior == HookFailureBehavior::FailOpen;
}

bool HookPolicyEngine::declarationComplete(const HookDeclaration &declaration)
{
    // 契约的每一项都必须存在:缺一项意味着某个行为是隐含的,而隐含行为无法被审查。
    return Safety::validId(declaration.id)
        && !declaration.matcher.isEmpty()
        && !declaration.command.isEmpty()
        && !declaration.scope.isEmpty()
        && declaration.timeoutMs >= MinTimeoutMs
        && declaration.timeoutMs <= MaxTimeoutMs;
}

HookVerdict HookPolicyEngine::evaluate(const HookDeclaration &declaration,
                                       const HookExecutionResult &result)
{
    if (!Safety::validId(declaration.id)) {
        return reject(QStringLiteral("hook-id-invalid"));
    }
    if (declaration.matcher.isEmpty()
            || !Safety::safeDisplayText(declaration.matcher, 512)) {
        return reject(QStringLiteral("hook-matcher-missing"));
    }
    if (declaration.command.isEmpty()
            || !Safety::safeDisplayText(declaration.command, 4096)) {
        return reject(QStringLiteral("hook-command-missing"));
    }
    if (declaration.scope.isEmpty()
            || !Safety::safeDisplayText(declaration.scope, 256)) {
        return reject(QStringLiteral("hook-scope-missing"));
    }
    // 超时必须被声明且有界。未声明超时的钩子可以无限期挂住事件循环。
    if (declaration.timeoutMs < MinTimeoutMs
            || declaration.timeoutMs > MaxTimeoutMs) {
        return reject(QStringLiteral("hook-timeout-invalid"));
    }
    // 未经复核的钩子不运行,因此它的结论也不采纳。
    if (declaration.trust != HookTrustState::Verified) {
        return reject(QStringLiteral("hook-untrusted"));
    }
    // 受管安全钩子声明 fail-open 是一个自相矛盾的契约,必须在采纳它之前被拒绝,而不是
    // 在运行时被静默改写成 fail-closed:静默改写会让审查者看到一份与实际行为不符的声明。
    if (declaration.provenance == HookProvenance::Managed
            && declaration.securityControl
            && declaration.failureBehavior == HookFailureBehavior::FailOpen) {
        return reject(QStringLiteral("hook-managed-security-fail-open"));
    }

    HookVerdict verdict;
    // 每一条结论都署名到这个钩子。一次没有署名的拒绝会让人以为工具本身坏了。
    verdict.attributedHookId = declaration.id;
    verdict.failOpenPermitted = failOpenPermitted(declaration);

    switch (result.outcome) {
    case HookExecutionOutcome::Denied:
        // 受信任钩子在契约内返回拒绝时,目标工具必须真的不执行。
        verdict.state = HookVerdictState::ToolBlocked;
        verdict.toolMayExecute = false;
        verdict.errorCode = QStringLiteral("hook-denied");
        return verdict;
    case HookExecutionOutcome::Allowed:
        verdict.state = HookVerdictState::ToolAllowed;
        verdict.toolMayExecute = true;
        return verdict;
    case HookExecutionOutcome::TimedOut:
        verdict.fromFailureBehavior = true;
        verdict.toolMayExecute = verdict.failOpenPermitted;
        verdict.state = verdict.toolMayExecute
            ? HookVerdictState::ToolAllowed
            : HookVerdictState::ToolBlocked;
        verdict.errorCode = QStringLiteral("hook-timed-out");
        return verdict;
    case HookExecutionOutcome::Crashed:
        verdict.fromFailureBehavior = true;
        verdict.toolMayExecute = verdict.failOpenPermitted;
        verdict.state = verdict.toolMayExecute
            ? HookVerdictState::ToolAllowed
            : HookVerdictState::ToolBlocked;
        verdict.errorCode = QStringLiteral("hook-crashed");
        return verdict;
    case HookExecutionOutcome::ContractViolation:
        // 契约外的结果不可采纳,因此按失败处理而不是按允许处理:把一个无法解析的返回值
        // 当作允许,等于让钩子只要输出乱码就能放行任何东西。
        verdict.fromFailureBehavior = true;
        verdict.toolMayExecute = verdict.failOpenPermitted;
        verdict.state = verdict.toolMayExecute
            ? HookVerdictState::ToolAllowed
            : HookVerdictState::ToolBlocked;
        verdict.errorCode = QStringLiteral("hook-contract-violation");
        return verdict;
    }

    // 未分类结果按失败处理。新增的结果类型不得默认变成放行。
    verdict.fromFailureBehavior = true;
    verdict.toolMayExecute = false;
    verdict.state = HookVerdictState::ToolBlocked;
    verdict.errorCode = QStringLiteral("hook-outcome-unknown");
    return verdict;
}

HookBoundedOutput HookPolicyEngine::boundOutput(const QStringList &lines)
{
    HookBoundedOutput output;
    // 处理输出从不阻塞事件循环:一个话很多的钩子不应让整个 Agent 停下来。
    output.blockedEventLoop = false;
    const int start = lines.size() > MaxOutputLines
        ? lines.size() - MaxOutputLines
        : 0;
    output.droppedLines = start;
    output.truncated = start > 0;
    // 超出内联上限时转为工件,而不是把内容丢掉:钩子的输出常常是它拒绝的理由。
    output.storedAsArtifact = start > 0;
    for (int index = start; index < lines.size(); ++index) {
        QString text = lines.at(index);
        if (text.size() > MaxOutputLineLength) {
            text = text.left(MaxOutputLineLength);
            output.truncated = true;
            output.storedAsArtifact = true;
        }
        output.lines.append(text);
    }
    return output;
}
