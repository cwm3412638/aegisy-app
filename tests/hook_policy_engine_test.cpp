#include "hook_policy_engine.h"

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

// 一个契约完整、已复核的项目钩子。
HookDeclaration declaration()
{
    HookDeclaration value;
    value.id = QStringLiteral("acme.guard");
    value.event = HookLifecycleEvent::PreToolUse;
    value.matcher = QStringLiteral("Write|Edit");
    value.command = QStringLiteral("/repo/.hooks/guard.sh");
    value.timeoutMs = 5000;
    value.scope = QStringLiteral("project");
    value.requestedPermissions = QStringList{QStringLiteral("read-files")};
    value.failureBehavior = HookFailureBehavior::FailClosed;
    value.trust = HookTrustState::Verified;
    value.provenance = HookProvenance::Project;
    return value;
}

// 受管安全钩子:它必须失败关闭。
HookDeclaration managedSecurityHook()
{
    HookDeclaration value = declaration();
    value.id = QStringLiteral("org.security-guard");
    value.provenance = HookProvenance::Managed;
    value.securityControl = true;
    return value;
}

HookExecutionResult outcome(HookExecutionOutcome value)
{
    HookExecutionResult result;
    result.outcome = value;
    result.elapsedMs = 12;
    return result;
}

// 受信任钩子在契约内返回拒绝时,目标工具不得执行,且拒绝归因到该钩子。
void denialTests()
{
    const HookVerdict verdict = HookPolicyEngine::evaluate(
        declaration(), outcome(HookExecutionOutcome::Denied));
    expect(verdict.state == HookVerdictState::ToolBlocked,
           "a trusted hook's denial did not block the target tool");
    // 关键:目标工具真的不执行。
    expect(!verdict.toolMayExecute,
           "a denied tool was still permitted to execute");
    // 关键:时间线必须能把这次拒绝署名到那个钩子。没有署名的拒绝会让人去修工具本身。
    expect(verdict.attributedHookId == QStringLiteral("acme.guard"),
           "the timeline could not attribute the denial to the hook");
    expect(verdict.errorCode == QStringLiteral("hook-denied"),
           "a hook denial carried no diagnostic");
    // 这是钩子的显式判断,而不是失败关闭的结果。两者必须可分辨。
    expect(!verdict.fromFailureBehavior,
           "an explicit denial was presented as a failure fallback");

    // 允许路径同样署名。
    const HookVerdict allowed = HookPolicyEngine::evaluate(
        declaration(), outcome(HookExecutionOutcome::Allowed));
    expect(allowed.state == HookVerdictState::ToolAllowed
               && allowed.toolMayExecute,
           "a hook allowing the tool still blocked it");
    expect(allowed.attributedHookId == QStringLiteral("acme.guard"),
           "an allow decision was not attributed to the hook");
    expect(allowed.errorCode.isEmpty(),
           "an allow decision carried an error code");
}

// 超时或崩溃时按声明的失败策略处理,而受管安全钩子必须失败关闭。
void failureBehaviorTests()
{
    const QList<HookExecutionOutcome> failures{
        HookExecutionOutcome::TimedOut, HookExecutionOutcome::Crashed,
        HookExecutionOutcome::ContractViolation};

    // fail-closed 的普通钩子:拦下。
    for (const HookExecutionOutcome value : failures) {
        const HookVerdict verdict =
            HookPolicyEngine::evaluate(declaration(), outcome(value));
        expect(!verdict.toolMayExecute,
               "a fail-closed hook failure still permitted the tool");
        expect(verdict.state == HookVerdictState::ToolBlocked,
               "a fail-closed hook failure did not block the tool");
        // 失败关闭的结论必须与钩子的显式拒绝可分辨。
        expect(verdict.fromFailureBehavior,
               "a failure fallback was presented as an explicit hook decision");
        expect(!verdict.errorCode.isEmpty(),
               "a hook failure carried no diagnostic");
        expect(verdict.attributedHookId == QStringLiteral("acme.guard"),
               "a hook failure was not attributed to the hook");
    }

    // fail-open 的普通钩子:放行。这是允许的选择。
    HookDeclaration open = declaration();
    open.failureBehavior = HookFailureBehavior::FailOpen;
    for (const HookExecutionOutcome value : failures) {
        const HookVerdict verdict =
            HookPolicyEngine::evaluate(open, outcome(value));
        expect(verdict.toolMayExecute,
               "a fail-open hook failure did not permit the tool");
        expect(verdict.fromFailureBehavior,
               "a fail-open fallback was presented as an explicit decision");
    }

    // 关键:受管安全钩子必须失败关闭。如果它在崩溃时放行,那么让它崩溃就成了绕过它的
    // 办法,于是这个钩子提供的保护等于零。
    //
    // 这里的固定件刻意声明 fail-open。这一点是必要的:一个声明 fail-closed 的受管安全
    // 钩子会因为普通原因而拦下,于是"受管安全钩子不得 fail-open"这条不变量在它身上
    // 完全不可观察——测试会在守卫被移除后依然通过。声明 fail-open 才让这条规则成为
    // 结论的唯一来源。
    HookDeclaration managed = managedSecurityHook();
    managed.failureBehavior = HookFailureBehavior::FailOpen;
    expect(managed.failureBehavior == HookFailureBehavior::FailOpen,
           "the managed fixture did not request the behavior being forbidden");
    expect(!HookPolicyEngine::failOpenPermitted(managed),
           "fail-open was available to a managed security hook that asked for it");
    for (const HookExecutionOutcome value : failures) {
        const HookVerdict verdict =
            HookPolicyEngine::evaluate(managed, outcome(value));
        expect(!verdict.toolMayExecute,
               "a managed security hook failed open");
        expect(!verdict.failOpenPermitted,
               "a managed security hook was permitted to fail open");
    }
    // 声明 fail-closed 的受管安全钩子当然也失败关闭,但那不构成对这条规则的验证。
    expect(!HookPolicyEngine::failOpenPermitted(managedSecurityHook()),
           "fail-open was available to a managed security hook");

    // 受管安全钩子声明 fail-open 是自相矛盾的契约:必须被拒绝,而不是静默改写成
    // fail-closed。静默改写会让审查者看到一份与实际行为不符的声明。
    HookDeclaration contradictory = managedSecurityHook();
    contradictory.failureBehavior = HookFailureBehavior::FailOpen;
    const HookVerdict rejected =
        HookPolicyEngine::evaluate(contradictory, outcome(
            HookExecutionOutcome::Allowed));
    expect(rejected.state == HookVerdictState::DeclarationRejected,
           "a managed security hook declaring fail-open was accepted");
    expect(rejected.errorCode
               == QStringLiteral("hook-managed-security-fail-open"),
           "a contradictory managed contract did not report why");
    expect(!rejected.toolMayExecute,
           "a rejected hook declaration permitted the tool");
    expect(!HookPolicyEngine::failOpenPermitted(contradictory),
           "a contradictory declaration still reported fail-open as permitted");

    // 非受管的安全钩子仍然可以选择 fail-open:只有受管的那一类被禁止。
    HookDeclaration projectSecurity = declaration();
    projectSecurity.securityControl = true;
    projectSecurity.failureBehavior = HookFailureBehavior::FailOpen;
    expect(HookPolicyEngine::failOpenPermitted(projectSecurity),
           "a project security hook was denied a choice it may make");

    // 未分类结果按失败处理:新增结果类型不得默认变成放行。
    HookExecutionResult unknown;
    unknown.outcome = static_cast<HookExecutionOutcome>(9999);
    HookDeclaration openHook = declaration();
    openHook.failureBehavior = HookFailureBehavior::FailOpen;
    const HookVerdict unknownVerdict =
        HookPolicyEngine::evaluate(openHook, unknown);
    expect(!unknownVerdict.toolMayExecute,
           "an unclassified hook outcome defaulted to permitting the tool");
    expect(unknownVerdict.errorCode == QStringLiteral("hook-outcome-unknown"),
           "an unclassified hook outcome carried no diagnostic");
}

// 契约的每一项都必须存在:缺一项意味着某个行为是隐含的。
void declarationTests()
{
    expect(HookPolicyEngine::declarationComplete(declaration()),
           "a complete hook contract was rejected");

    struct Case {
        const char *label;
        HookDeclaration value;
        QString code;
    };
    QList<Case> cases;
    {
        HookDeclaration value = declaration();
        value.id = QStringLiteral("Bad Id");
        cases.append({"id", value, QStringLiteral("hook-id-invalid")});
    }
    {
        HookDeclaration value = declaration();
        value.matcher.clear();
        cases.append({"matcher", value, QStringLiteral("hook-matcher-missing")});
    }
    {
        HookDeclaration value = declaration();
        value.command.clear();
        cases.append({"command", value, QStringLiteral("hook-command-missing")});
    }
    {
        HookDeclaration value = declaration();
        value.scope.clear();
        cases.append({"scope", value, QStringLiteral("hook-scope-missing")});
    }
    {
        HookDeclaration value = declaration();
        value.timeoutMs = 0;
        cases.append({"timeout", value, QStringLiteral("hook-timeout-invalid")});
    }
    {
        HookDeclaration value = declaration();
        value.timeoutMs = HookPolicyEngine::MaxTimeoutMs + 1;
        cases.append({"timeout-max", value,
                      QStringLiteral("hook-timeout-invalid")});
    }
    for (const Case &item : cases) {
        const HookVerdict verdict =
            HookPolicyEngine::evaluate(item.value, outcome(
                HookExecutionOutcome::Allowed));
        expect(verdict.state == HookVerdictState::DeclarationRejected,
               "an incomplete hook contract was accepted");
        expect(verdict.errorCode == item.code,
               "an incomplete contract did not report which term is missing");
        // 关键:无法审查的契约不等于没有钩子。否则删掉契约里的一行就成了放行的办法。
        expect(!verdict.toolMayExecute,
               "an unreviewable hook contract permitted the tool");
        expect(!HookPolicyEngine::declarationComplete(item.value),
               "an incomplete contract was reported as complete");
    }

    // 未经复核的钩子不运行,因此它的结论也不采纳。
    HookDeclaration untrusted = declaration();
    untrusted.trust = HookTrustState::Unverified;
    const HookVerdict verdict = HookPolicyEngine::evaluate(
        untrusted, outcome(HookExecutionOutcome::Allowed));
    expect(verdict.errorCode == QStringLiteral("hook-untrusted"),
           "an unverified hook's conclusion was adopted");
    expect(!verdict.toolMayExecute,
           "an unverified hook permitted the tool");

    // 展示不安全的匹配器与命令同样被拒绝:它们会出现在授权与时间线界面上。
    HookDeclaration spoofed = declaration();
    spoofed.matcher = QStringLiteral("Write‮tidE");
    expect(HookPolicyEngine::evaluate(
               spoofed, outcome(HookExecutionOutcome::Allowed)).state
               == HookVerdictState::DeclarationRejected,
           "a bidirectional override in a matcher was accepted");
}

// 无界输出必须被截断或转为工件,且不得阻塞事件循环。
void boundedOutputTests()
{
    QStringList lines;
    for (int index = 0; index < HookPolicyEngine::MaxOutputLines + 40; ++index) {
        lines.append(QStringLiteral("line %1").arg(index));
    }
    const HookBoundedOutput output = HookPolicyEngine::boundOutput(lines);
    expect(output.lines.size() == HookPolicyEngine::MaxOutputLines,
           "unbounded hook output was not bounded");
    // 关键:不阻塞事件循环。
    expect(!output.blockedEventLoop,
           "bounding hook output blocked the Agent event loop");
    // 截断可见,且内容转为工件而不是被丢掉:钩子输出常常是它拒绝的理由。
    expect(output.truncated && output.droppedLines == 40,
           "truncated hook output did not disclose how much was dropped");
    expect(output.storedAsArtifact,
           "over-limit hook output was discarded instead of stored");
    expect(output.lines.last()
               == QStringLiteral("line %1")
                      .arg(HookPolicyEngine::MaxOutputLines + 39),
           "the most recent hook output lines were discarded");

    // 单行过长同样被裁剪并计为截断。
    const HookBoundedOutput longLine = HookPolicyEngine::boundOutput(
        {QString(HookPolicyEngine::MaxOutputLineLength + 5, QLatin1Char('y'))});
    expect(longLine.lines.at(0).size() == HookPolicyEngine::MaxOutputLineLength,
           "an unbounded hook output line was not clipped");
    expect(longLine.truncated && longLine.storedAsArtifact,
           "clipping a hook output line was not disclosed");

    // 未超限时不声称截断。
    const HookBoundedOutput small =
        HookPolicyEngine::boundOutput({QStringLiteral("ok")});
    expect(!small.truncated && small.droppedLines == 0
               && !small.storedAsArtifact,
           "hook output within limits was reported as truncated");
}

// 这一层不执行命令、不启动进程。
void authorityTests()
{
    // 默认声明(未复核、无契约)必须结论为不放行。
    const HookVerdict verdict = HookPolicyEngine::evaluate(
        HookDeclaration(), outcome(HookExecutionOutcome::Allowed));
    expect(!verdict.toolMayExecute,
           "a default hook declaration permitted the tool");
    expect(verdict.state == HookVerdictState::DeclarationRejected,
           "a default hook declaration was accepted");

    // 每一种已定义事件都有展示标签,否则时间线会出现一条没有事件名的记录。
    for (const HookLifecycleEvent event : {
             HookLifecycleEvent::PreToolUse, HookLifecycleEvent::PostToolUse,
             HookLifecycleEvent::SessionStart,
             HookLifecycleEvent::UserPromptSubmit, HookLifecycleEvent::Stop}) {
        expect(!HookPolicyEngine::eventLabel(event).isEmpty(),
               "a hook lifecycle event has no display label");
    }
    expect(!HookPolicyEngine::eventLabel(
               static_cast<HookLifecycleEvent>(9999)).isEmpty(),
           "an unclassified hook event has no display label");

    // 每一条结论都必须署名。没有署名的结论无法归因。
    for (const HookExecutionOutcome value : {
             HookExecutionOutcome::Allowed, HookExecutionOutcome::Denied,
             HookExecutionOutcome::TimedOut, HookExecutionOutcome::Crashed,
             HookExecutionOutcome::ContractViolation}) {
        expect(!HookPolicyEngine::evaluate(declaration(), outcome(value))
                    .attributedHookId.isEmpty(),
               "a hook verdict carried no attribution");
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    denialTests();
    failureBehaviorTests();
    declarationTests();
    boundedOutputTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "hook policy engine tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
