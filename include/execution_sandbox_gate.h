#ifndef EXECUTION_SANDBOX_GATE_H
#define EXECUTION_SANDBOX_GATE_H

#include <QString>
#include <QStringList>

// 沙箱门禁。前面几层回答的是"这份内容被人看过吗"与"这个批准是否构成授权"，这一层回答
// 一个独立且不可跳过的问题："即使已经被批准，当前宿主能否在被强制的边界内运行它"。
//
// 这两件事必须分开：一份完全对齐的批准表达的是"人愿意让这份内容运行"，它并不证明操作
// 系统会拦住这份内容越出工作区去读凭据、改系统路径或连任意主机。批准是意图，沙箱是
// 强制；把意图当作强制，等于在没有围栏的地方宣布已经有围栏。
//
// 因此这一层是发布门禁而不是运行时开关：可写的原生执行在某个平台上不得随产品发布，
// 直到该平台的文件系统、进程与网络强制都被验证过。当前 ADR 0006 仍未选定 Windows 原生
// 沙箱组合,macOS 强制归属独立所有者且同样没有交付证据,因此本层对每个平台都得出只读
// 结论——这不是保守的默认值,而是当前唯一有证据支持的结论。
//
// 这一层不执行任何东西、不启动进程、不持久化、不修改策略。它只把"有哪些强制证据"变成
// 一个可判定的执行权限结论。
enum class SandboxPlatform {
    MacOS,
    Windows,
    // 既不是 macOS 也不是 Windows 的宿主。当前产品不为其提供任何强制证据。
    Unsupported,
};

// 单一强制维度的状态。三个维度必须各自独立成立：只挡住文件写入而放开子进程,等于让
// 越界写入换一条路径发生。
enum class SandboxEnforcement {
    // 该维度尚未在此平台上被验证过。
    Unverified,
    // 有已交付并被验证的强制。
    Enforced,
    // 该平台上确定无法强制该维度。
    Unavailable,
};

// 当前宿主声称具备的强制能力。这是被审查过的交付证据的投影,不是运行时探测结果:
// 运行时探测由被沙箱的进程自己回答"我被沙箱了吗",而那正是它无权回答的问题。
struct SandboxEnforcementEvidence {
    SandboxPlatform platform = SandboxPlatform::Unsupported;
    SandboxEnforcement filesystem = SandboxEnforcement::Unverified;
    SandboxEnforcement process = SandboxEnforcement::Unverified;
    SandboxEnforcement network = SandboxEnforcement::Unverified;
    // 该平台的逃逸回归测试结论。任何一次已证实的策略绕过都必须阻断可写通道,直到修复
    // 并重新验证——因此这一位为真时,其余证据一律不再构成执行权限。
    bool escapeRegressionOpen = false;
    // 该平台可写发布通道的门禁报告是否已经通过。编译成功、装得上、能启动都不是证据。
    bool releaseGateSigned = false;
};

// 允许的执行能力。只读始终是可用的下界:读取不需要强制边界就能保持无害。
enum class SandboxExecutionAuthority {
    // 只读:不写文件、不执行命令、不做 Git mutation。
    ReadOnly,
    // 在被强制的工作区边界内可写。
    WorkspaceWrite,
    // 明确标注的完全访问,必须逐会话显式确认,且只在发布策略允许时存在。
    FullAccess,
};

enum class SandboxVerdictState {
    // 强制齐备,可写执行被授权。
    Enforced,
    // 强制不齐备,工作保持只读。
    ReadOnlyFallback,
    // 已证实的逃逸回归未修复:可写通道被阻断。
    Blocked,
};

struct SandboxVerdict {
    SandboxVerdictState state = SandboxVerdictState::ReadOnlyFallback;
    SandboxExecutionAuthority authority = SandboxExecutionAuthority::ReadOnly;
    // 缺失的强制维度,按固定顺序输出,供界面说明为什么保持只读。
    QStringList missingEnforcement;
    // 是否允许把明确标注的完全访问模式提供给用户。逃逸回归未修复时永远为假:那时
    // 完全访问只是把已知可绕过的边界换成没有边界。
    bool fullAccessOfferable = false;
    QString errorCode;
};

// 沙箱拒绝一次工具动作时的结论。这是与"能不能发布"不同的问题,但共享同一条铁律:
// 被拒绝的动作不得在沙箱之外自动重试。
struct SandboxDenial {
    // 固定诊断代码,始终指明这是沙箱拒绝而不是工具失败或模型失败。
    QString errorCode;
    // 是否允许自动在沙箱之外重试。恒为假。
    bool retryOutsideSandbox = false;
    // 是否允许把这次拒绝报告成模型错误。恒为假:那会让人去改提示词而不是改边界。
    bool attributableToModel = false;
};

class ExecutionSandboxGate
{
public:
    // 当前构建所在平台的强制证据。没有任何平台交付过强制,因此三个维度都是
    // Unverified,门禁报告也未签署。
    static SandboxEnforcementEvidence currentEvidence();

    // 把强制证据变成执行权限结论。
    static SandboxVerdict evaluate(const SandboxEnforcementEvidence &evidence);

    // 沙箱拒绝一次工具动作。reason 只用于诊断分类,不参与权限判定。
    static SandboxDenial denial(const QString &reason);

    // 该权限是否越出当前只读边界。写文件、命令执行与 Git mutation 都在其外。
    static bool beyondReadOnly(SandboxExecutionAuthority authority);
};

#endif // EXECUTION_SANDBOX_GATE_H
