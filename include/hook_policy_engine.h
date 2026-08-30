#ifndef HOOK_POLICY_ENGINE_H
#define HOOK_POLICY_ENGINE_H

#include <QList>
#include <QString>
#include <QStringList>

// 钩子策略引擎。钩子与这里其他每一层都不同:它是**在工具执行之前运行并可以否决执行的
// 外部命令**。这让它同时是最强的安全控制点和最危险的失败点,而两者的答案方向相反。
//
// 作为安全控制点:一个受信任钩子在其契约内返回拒绝时,目标工具必须真的不执行,并且时间线
// 必须把这次拒绝归因到那个钩子。归因不是记账细节——一次没有署名的拒绝会让人以为工具本身
// 坏了,于是去修工具,而实际决定来自某个钩子的匹配规则。
//
// 作为失败点:钩子会超时、会崩溃、会输出无限日志。超时后是放行还是拦下取决于声明的策略,
// 但**受管安全钩子必须失败关闭**:一个用来阻止危险操作的钩子如果在崩溃时放行,那么让它
// 崩溃就成了绕过它的方法。因此 fail-open 对受管安全钩子不可用,而不是一个默认值。
//
// 无界输出不得阻塞事件循环。一个话很多的钩子不应让整个 Agent 停下来,因此输出被截断或
// 转为工件,而截断本身必须可见。
//
// 这一层不执行命令、不启动进程。它把一次已观测到的钩子结果变成可判定的结论。
enum class HookLifecycleEvent {
    PreToolUse,
    PostToolUse,
    SessionStart,
    UserPromptSubmit,
    Stop,
};

enum class HookTrustState {
    // 未经复核的钩子不运行。
    Unverified,
    Verified,
};

// 钩子的来源层级。受管钩子由组织策略下发。
enum class HookProvenance {
    Managed,
    User,
    Project,
};

enum class HookFailureBehavior {
    // 超时或崩溃时放行目标工具。
    FailOpen,
    // 超时或崩溃时拦下目标工具。
    FailClosed,
};

// 一个钩子的声明。契约的每一项都必须存在:缺一项就意味着某个行为是隐含的,而隐含行为
// 无法被审查。
struct HookDeclaration {
    QString id;
    HookLifecycleEvent event = HookLifecycleEvent::PreToolUse;
    // 匹配器:决定这个钩子对哪些工具调用生效。
    QString matcher;
    QString command;
    int timeoutMs = 0;
    // 生效范围。
    QString scope;
    QStringList requestedPermissions;
    HookFailureBehavior failureBehavior = HookFailureBehavior::FailClosed;
    HookTrustState trust = HookTrustState::Unverified;
    HookProvenance provenance = HookProvenance::Project;
    // 该钩子是否被声明为安全控制。受管安全钩子不允许 fail-open。
    bool securityControl = false;
};

enum class HookExecutionOutcome {
    // 钩子在契约内返回了允许。
    Allowed,
    // 钩子在契约内返回了拒绝。
    Denied,
    TimedOut,
    Crashed,
    // 钩子返回了契约外的结果,因此它的结论不可采纳。
    ContractViolation,
};

// 一次已观测到的钩子运行结果。
struct HookExecutionResult {
    HookExecutionOutcome outcome = HookExecutionOutcome::ContractViolation;
    // 钩子给出的理由,原样保留供人检视。
    QString reportedReason;
    QStringList outputLines;
    int elapsedMs = 0;
};

enum class HookVerdictState {
    // 目标工具可以执行。
    ToolAllowed,
    // 目标工具不得执行。
    ToolBlocked,
    // 钩子声明本身无法作为依据。
    DeclarationRejected,
};

struct HookVerdict {
    HookVerdictState state = HookVerdictState::DeclarationRejected;
    bool toolMayExecute = false;
    // 这次结论归因到哪个钩子。时间线用它署名。
    QString attributedHookId;
    // 该结论是否是失败关闭的结果,而不是钩子的显式判断。
    bool fromFailureBehavior = false;
    // 该钩子是否被允许 fail-open。受管安全钩子恒为假。
    bool failOpenPermitted = false;
    QString errorCode;
};

// 有界输出。它可以被截断或转为工件,但不得阻塞事件循环。
struct HookBoundedOutput {
    QStringList lines;
    int droppedLines = 0;
    bool truncated = false;
    // 超出内联上限时转为工件而不是丢弃。
    bool storedAsArtifact = false;
    // 恒为假:处理输出不阻塞 Agent 事件循环。
    bool blockedEventLoop = false;
};

class HookPolicyEngine
{
public:
    static constexpr int MaxOutputLines = 256;
    static constexpr int MaxOutputLineLength = 4096;
    static constexpr int MaxTimeoutMs = 120000;
    static constexpr int MinTimeoutMs = 1;

    static HookVerdict evaluate(const HookDeclaration &declaration,
                                const HookExecutionResult &result);

    // 该钩子是否允许 fail-open。受管安全钩子不允许。
    static bool failOpenPermitted(const HookDeclaration &declaration);

    // 声明契约是否完整。
    static bool declarationComplete(const HookDeclaration &declaration);

    static HookBoundedOutput boundOutput(const QStringList &lines);

    static QString eventLabel(HookLifecycleEvent event);
};

#endif // HOOK_POLICY_ENGINE_H
