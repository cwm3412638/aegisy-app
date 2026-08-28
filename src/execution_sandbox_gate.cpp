#include "execution_sandbox_gate.h"

#include <QPair>
#include <QtGlobal>

namespace {

// 缺失维度的固定名称。诊断必须区分"没验证过"与"确定做不到":前者是待完成的工作,
// 后者是这个平台上永远不该开放可写执行的结论。
QString dimensionCode(const QString &dimension, SandboxEnforcement state)
{
    switch (state) {
    case SandboxEnforcement::Enforced:
        return QString();
    case SandboxEnforcement::Unavailable:
        return QStringLiteral("sandbox-") + dimension + QStringLiteral("-unavailable");
    case SandboxEnforcement::Unverified:
        break;
    }
    // 未知取值按未验证处理:新增的强制状态不应默认被当成已强制。
    return QStringLiteral("sandbox-") + dimension + QStringLiteral("-unverified");
}

SandboxVerdict readOnly(const QStringList &missing, const QString &errorCode,
                        bool fullAccessOfferable)
{
    SandboxVerdict verdict;
    verdict.state = SandboxVerdictState::ReadOnlyFallback;
    verdict.authority = SandboxExecutionAuthority::ReadOnly;
    verdict.missingEnforcement = missing;
    verdict.fullAccessOfferable = fullAccessOfferable;
    verdict.errorCode = errorCode;
    return verdict;
}

} // namespace

SandboxEnforcementEvidence ExecutionSandboxGate::currentEvidence()
{
    SandboxEnforcementEvidence evidence;
#if defined(Q_OS_MAC)
    evidence.platform = SandboxPlatform::MacOS;
#elif defined(Q_OS_WIN)
    evidence.platform = SandboxPlatform::Windows;
#else
    evidence.platform = SandboxPlatform::Unsupported;
#endif
    // 三个维度都保持 Unverified,门禁报告未签署:当前没有任何平台交付过被验证的文件
    // 系统、进程与网络强制。ADR 0006 未选定 Windows 原生沙箱组合,macOS 强制归属
    // 独立所有者且同样没有交付证据。这里不写入乐观值——一个自称"已强制"的常量会让
    // 上层在没有围栏的地方放开写入。
    return evidence;
}

SandboxVerdict ExecutionSandboxGate::evaluate(
    const SandboxEnforcementEvidence &evidence)
{
    // 已证实的策略绕过先于其他判断:此时可写通道必须被阻断,而不是退回到"缺哪几项"
    // 的讨论。一个已知可绕过的边界比没有边界更危险,因为它看起来是有边界的。
    if (evidence.escapeRegressionOpen) {
        SandboxVerdict verdict;
        verdict.state = SandboxVerdictState::Blocked;
        verdict.authority = SandboxExecutionAuthority::ReadOnly;
        verdict.fullAccessOfferable = false;
        verdict.errorCode = QStringLiteral("sandbox-escape-regression-open");
        return verdict;
    }

    // 不支持的平台没有任何强制证据可谈。这里不落入"缺失维度"的路径:那会暗示补上
    // 三项验证就能开放,而实际上需要先为该平台选定一套强制机制。
    if (evidence.platform == SandboxPlatform::Unsupported) {
        return readOnly(QStringList{QStringLiteral("sandbox-platform-unsupported")},
                        QStringLiteral("sandbox-platform-unsupported"), false);
    }

    // 三个维度按固定顺序逐项检查,缺哪几项就报哪几项:只报第一项会让人以为补上它就够了。
    QStringList missing;
    for (const auto &dimension : {
             qMakePair(QStringLiteral("filesystem"), evidence.filesystem),
             qMakePair(QStringLiteral("process"), evidence.process),
             qMakePair(QStringLiteral("network"), evidence.network)}) {
        const QString code = dimensionCode(dimension.first, dimension.second);
        if (!code.isEmpty()) missing.append(code);
    }
    if (!missing.isEmpty()) {
        // 强制不齐备时工作保持只读。发布策略可以改为提供一个明确标注的完全访问模式,
        // 但那要求逐会话显式确认,并且只在该平台的门禁报告已签署时才可提供——否则
        // "完全访问"会变成绕过未完成门禁的常规路径。
        return readOnly(missing, QStringLiteral("sandbox-enforcement-incomplete"),
                        evidence.releaseGateSigned);
    }

    // 强制齐备仍然不够:该平台可写发布通道的门禁报告必须已经通过。编译成功、装得上、
    // 能启动都不是强制证据。
    if (!evidence.releaseGateSigned) {
        return readOnly(QStringList{QStringLiteral("sandbox-release-gate-unsigned")},
                        QStringLiteral("sandbox-release-gate-unsigned"), false);
    }

    SandboxVerdict verdict;
    verdict.state = SandboxVerdictState::Enforced;
    verdict.authority = SandboxExecutionAuthority::WorkspaceWrite;
    verdict.fullAccessOfferable = true;
    return verdict;
}

SandboxDenial ExecutionSandboxGate::denial(const QString &reason)
{
    SandboxDenial value;
    // 诊断代码始终标明这是沙箱拒绝。把它报告成工具失败会让人去重试,报告成模型失败
    // 会让人去改提示词,而正确的结论是这个动作越出了被强制的边界。
    value.errorCode = reason.isEmpty()
        ? QStringLiteral("sandbox-denied")
        : QStringLiteral("sandbox-denied-") + reason;
    // 自动在沙箱之外重试会把一次成功的强制变成一次延迟的越界,因此永远不允许。
    value.retryOutsideSandbox = false;
    value.attributableToModel = false;
    return value;
}

bool ExecutionSandboxGate::beyondReadOnly(SandboxExecutionAuthority authority)
{
    switch (authority) {
    case SandboxExecutionAuthority::ReadOnly:
        return false;
    case SandboxExecutionAuthority::WorkspaceWrite:
    case SandboxExecutionAuthority::FullAccess:
        return true;
    }
    // 未知权限按越出只读处理:新增的权限级别不应默认被当成无害的。
    return true;
}
