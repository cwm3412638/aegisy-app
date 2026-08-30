#include "mcp_lifecycle_policy.h"

#include "extension_display_safety.h"

namespace {

using Safety = ExtensionDisplaySafety;

McpToolApprovalPrompt unpresentable(const QString &code)
{
    McpToolApprovalPrompt prompt;
    prompt.state = McpApprovalPromptState::Unpresentable;
    // 拒绝路径上同样不授予调用。这一点在每一条返回路径上成立。
    prompt.grantsInvocation = false;
    prompt.errorCode = code;
    return prompt;
}

} // namespace

bool McpLifecyclePolicy::secretBearing(const QString &argumentName)
{
    static const QStringList markers{
        QStringLiteral("token"),
        QStringLiteral("secret"),
        QStringLiteral("password"),
        QStringLiteral("passwd"),
        QStringLiteral("credential"),
        QStringLiteral("apikey"),
        QStringLiteral("api_key"),
        QStringLiteral("api-key"),
        QStringLiteral("authorization"),
        QStringLiteral("auth"),
        QStringLiteral("bearer"),
        QStringLiteral("private"),
        QStringLiteral("signature"),
        QStringLiteral("session"),
        QStringLiteral("cookie"),
    };
    const QString lowered = argumentName.toLower();
    for (const QString &marker : markers) {
        if (lowered.contains(marker)) return true;
    }
    return false;
}

QString McpLifecyclePolicy::stateLabel(McpServerState state)
{
    switch (state) {
    case McpServerState::Stopped:
        return QStringLiteral("已停止");
    case McpServerState::Starting:
        return QStringLiteral("正在启动");
    case McpServerState::Ready:
        return QStringLiteral("就绪");
    case McpServerState::AuthenticationRequired:
        return QStringLiteral("需要认证");
    case McpServerState::Failed:
        return QStringLiteral("启动失败");
    }
    return QStringLiteral("未知状态");
}

McpServerStatus McpLifecyclePolicy::evaluate(const McpStartupEvidence &evidence)
{
    McpServerStatus status;
    // 未启用或未信任的服务器不应启动。这两者由更早的门禁判定,这一层只是不越过它们。
    if (!evidence.effectivelyEnabled) {
        status.state = McpServerState::Stopped;
        status.errorCode = QStringLiteral("mcp-not-enabled");
        return status;
    }
    if (!evidence.trusted) {
        status.state = McpServerState::Stopped;
        status.errorCode = QStringLiteral("mcp-not-trusted");
        return status;
    }
    // 进程已退出是终态,无论握手是否曾经完成:一个退出过的服务器不再能服务这一回合。
    if (evidence.exited) {
        status.state = McpServerState::Failed;
        status.errorCode = QStringLiteral("mcp-server-exited");
        return status;
    }
    // 认证缺失与失败是不同的结论:前者可由人补上,后者不能。把两者合并会让人对一个
    // 只差一次登录的服务器去排查故障。
    if (evidence.authenticationRequired) {
        status.state = McpServerState::AuthenticationRequired;
        status.errorCode = QStringLiteral("mcp-authentication-required");
        status.resolvableByAuthentication = true;
        return status;
    }
    if (!evidence.handshakeCompleted) {
        status.state = McpServerState::Starting;
        status.errorCode = QStringLiteral("mcp-handshake-pending");
        return status;
    }
    status.state = McpServerState::Ready;
    // 只有就绪状态下工具清单可用。未就绪时声称工具可用会让调用落在一个不存在的通道上。
    status.toolsAvailable = true;
    return status;
}

McpDependentItemOutcome McpLifecyclePolicy::dependentOutcome(
    const QString &serverId, const McpServerStatus &status)
{
    McpDependentItemOutcome outcome;
    outcome.serverId = serverId;
    // 归因始终是这台服务器。报成模型失败会让人去修提示词而问题在服务器;报成成功结果
    // 更糟——模型会把一个不存在的返回值当作事实继续推理。
    outcome.attribution = McpFailureAttribution::Server;
    outcome.attributableToModel = false;
    outcome.reportedAsSuccess = false;

    switch (status.state) {
    case McpServerState::AuthenticationRequired:
        outcome.errorCode = QStringLiteral("mcp-dependency-authentication-required");
        outcome.remediation =
            QStringLiteral("该 MCP 服务器要求认证。完成认证后重试这一项。");
        return outcome;
    case McpServerState::Failed:
        outcome.errorCode = QStringLiteral("mcp-dependency-failed");
        outcome.remediation =
            QStringLiteral("该 MCP 服务器启动失败。检查它的日志与配置后重试。");
        return outcome;
    case McpServerState::Starting:
        outcome.errorCode = QStringLiteral("mcp-dependency-not-ready");
        outcome.remediation =
            QStringLiteral("该 MCP 服务器尚未就绪。等待它完成握手后重试。");
        return outcome;
    case McpServerState::Stopped:
        outcome.errorCode = QStringLiteral("mcp-dependency-stopped");
        outcome.remediation =
            QStringLiteral("该 MCP 服务器未启用或未信任。先完成启用与复核。");
        return outcome;
    case McpServerState::Ready:
        // 就绪服务器上的失败仍然属于服务器,而不属于模型:这一层从不把任何失败改写成
        // 模型失败。
        outcome.errorCode = QStringLiteral("mcp-dependency-unavailable");
        outcome.remediation =
            QStringLiteral("该 MCP 服务器已就绪但这一项不可用。检查它的工具清单。");
        return outcome;
    }
    // 未分类状态同样归因于服务器,而不是默默变成模型失败。
    outcome.errorCode = QStringLiteral("mcp-dependency-state-unknown");
    outcome.remediation =
        QStringLiteral("该 MCP 服务器状态无法判定。检查它的日志后重试。");
    return outcome;
}

McpToolApprovalPrompt McpLifecyclePolicy::approvalPrompt(
    const McpToolApprovalRequest &request)
{
    if (!Safety::validId(request.serverId)) {
        return unpresentable(QStringLiteral("mcp-approval-server-id-invalid"));
    }
    if (!Safety::hashIdentity(request.serverIdentity,
                              QStringLiteral("extension-content:sha256:"))) {
        return unpresentable(QStringLiteral("mcp-approval-identity-invalid"));
    }
    if (request.toolName.isEmpty()
            || !Safety::safeDisplayText(request.toolName, 128)) {
        return unpresentable(QStringLiteral("mcp-approval-tool-unsafe"));
    }
    // 参数名与值必须一一对应。数量不一致时无法确定哪个值属于哪个参数,而错位展示会让人
    // 批准一个自己没有看懂的调用。
    if (request.argumentNames.size() != request.argumentValues.size()) {
        return unpresentable(QStringLiteral("mcp-approval-arguments-misaligned"));
    }
    if (request.argumentNames.size() > MaxArguments) {
        return unpresentable(QStringLiteral("mcp-approval-argument-limit"));
    }

    McpToolApprovalPrompt prompt;
    prompt.serverLabel = request.serverId;
    prompt.serverFingerprint = Safety::fingerprint(request.serverIdentity);
    prompt.toolName = request.toolName;
    // 渲染一次审批不等于批准它。
    prompt.grantsInvocation = false;

    for (int index = 0; index < request.argumentNames.size(); ++index) {
        const QString name = request.argumentNames.at(index);
        // 上面已经拒绝了数量不一致的请求,但索引安全不依赖那道检查:一个不可信输入不应
        // 因为某道守卫被改动而变成越界读取。
        if (index >= request.argumentValues.size()) {
            return unpresentable(QStringLiteral("mcp-approval-arguments-misaligned"));
        }
        const QString value = request.argumentValues.at(index);
        if (name.isEmpty() || !Safety::safeDisplayText(name, 128)) {
            return unpresentable(QStringLiteral("mcp-approval-argument-name-unsafe"));
        }
        if (prompt.argumentNames.contains(name)) {
            return unpresentable(
                QStringLiteral("mcp-approval-argument-duplicate"));
        }
        prompt.argumentNames.append(name);
        // 机密被遮蔽,并且遮蔽这件事本身可见:人需要知道自己在批准一个含凭据的调用。
        if (secretBearing(name)) {
            prompt.displayedValues.append(QStringLiteral("已遮蔽"));
            prompt.redactedArguments.append(name);
            continue;
        }
        // 非机密参数仍然必须可安全展示:参数值同样来自不可信来源。
        if (!Safety::safeDisplayText(value, 512)) {
            return unpresentable(
                QStringLiteral("mcp-approval-argument-value-unsafe"));
        }
        prompt.displayedValues.append(value);
    }

    for (const QString &permission : request.requestedPermissions) {
        if (!Safety::safeDisplayText(permission, 128)) {
            return unpresentable(QStringLiteral("mcp-approval-permission-unsafe"));
        }
        prompt.requestedPermissions.append(permission);
        if (Safety::beyondReadOnly(permission)) prompt.beyondReadOnly = true;
    }

    // 持久化选项必须被列出,否则人只能在"这一次"与"不知道会持续多久"之间选择。越出
    // 只读边界的调用不提供记住选项:那样的授权必须每次单独作出。
    prompt.persistenceOptions.append(QStringLiteral("仅这一次"));
    if (!prompt.beyondReadOnly) {
        prompt.persistenceOptions.append(QStringLiteral("记住这台服务器的这个工具"));
    }

    prompt.state = McpApprovalPromptState::Ready;
    return prompt;
}

McpBoundedLog McpLifecyclePolicy::boundLog(const QList<McpLogLine> &lines)
{
    McpBoundedLog log;
    // 保留最近的行:一个失败服务器的最后几行通常才是原因所在。
    const int start = lines.size() > MaxLogLines ? lines.size() - MaxLogLines : 0;
    log.droppedLines = start;
    log.truncated = start > 0;
    for (int index = start; index < lines.size(); ++index) {
        QString text = lines.at(index).text;
        if (text.size() > MaxLogLineLength) {
            text = text.left(MaxLogLineLength);
            // 单行被裁剪同样是一次截断,必须可见。
            log.truncated = true;
        }
        log.lines.append(text);
    }
    return log;
}
