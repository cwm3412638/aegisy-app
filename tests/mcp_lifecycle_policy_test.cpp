#include "mcp_lifecycle_policy.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSet>
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

QString identityOf(const QByteArray &seed)
{
    return QStringLiteral("extension-content:sha256:")
        + QString::fromLatin1(QCryptographicHash::hash(
            seed, QCryptographicHash::Sha256).toHex());
}

McpStartupEvidence enabledAndTrusted()
{
    McpStartupEvidence value;
    value.effectivelyEnabled = true;
    value.trusted = true;
    return value;
}

McpToolApprovalRequest request()
{
    McpToolApprovalRequest value;
    value.serverId = QStringLiteral("acme.tools");
    value.serverIdentity = identityOf("acme-tools");
    value.toolName = QStringLiteral("search_repository");
    value.argumentNames = QStringList{QStringLiteral("query"),
                                      QStringLiteral("api_token")};
    value.argumentValues = QStringList{QStringLiteral("crash in parser"),
                                       QStringLiteral("sk-live-9f3a2b7c")};
    value.requestedPermissions = QStringList{QStringLiteral("read-files")};
    return value;
}

// 启用且信任后,启动过程的每一个状态都必须各自可辨认。
void lifecycleStateTests()
{
    // 未启用与未信任的服务器不应启动。
    McpStartupEvidence disabled = enabledAndTrusted();
    disabled.effectivelyEnabled = false;
    expect(McpLifecyclePolicy::evaluate(disabled).state
               == McpServerState::Stopped,
           "an extension without effective enablement was started");
    McpStartupEvidence untrusted = enabledAndTrusted();
    untrusted.trusted = false;
    expect(McpLifecyclePolicy::evaluate(untrusted).errorCode
               == QStringLiteral("mcp-not-trusted"),
           "an untrusted MCP server was started");

    // 启动中:握手未完成。
    const McpServerStatus starting =
        McpLifecyclePolicy::evaluate(enabledAndTrusted());
    expect(starting.state == McpServerState::Starting,
           "an enabled and trusted server did not report starting");
    // 关键:未就绪时工具清单不可用。声称可用会让调用落在不存在的通道上。
    expect(!starting.toolsAvailable,
           "tools were offered before the server was ready");

    // 就绪。
    McpStartupEvidence ready = enabledAndTrusted();
    ready.handshakeCompleted = true;
    const McpServerStatus readyStatus = McpLifecyclePolicy::evaluate(ready);
    expect(readyStatus.state == McpServerState::Ready,
           "a completed handshake did not report ready");
    expect(readyStatus.toolsAvailable,
           "a ready server did not expose its tool list");
    expect(readyStatus.errorCode.isEmpty(),
           "a ready server carried an error code");

    // 需要认证:与失败是不同的结论,因为它可由人补上。
    McpStartupEvidence auth = enabledAndTrusted();
    auth.authenticationRequired = true;
    const McpServerStatus authStatus = McpLifecyclePolicy::evaluate(auth);
    expect(authStatus.state == McpServerState::AuthenticationRequired,
           "a server requiring authentication was not distinguished");
    expect(authStatus.resolvableByAuthentication,
           "an authentication gap was not marked as resolvable");
    expect(!authStatus.toolsAvailable,
           "an unauthenticated server offered its tools");

    // 已退出是终态,即使握手曾经完成。
    McpStartupEvidence exited = enabledAndTrusted();
    exited.handshakeCompleted = true;
    exited.exited = true;
    const McpServerStatus failed = McpLifecyclePolicy::evaluate(exited);
    expect(failed.state == McpServerState::Failed,
           "an exited server was still reported as usable");
    expect(!failed.toolsAvailable,
           "an exited server still offered its tools");
    expect(!failed.resolvableByAuthentication,
           "a failed server was presented as an authentication gap");
}

// 依赖失败服务器的回合必须以服务器专属失败结束,既非模型失败也非成功结果。
void attributionTests()
{
    const QList<McpServerState> states{
        McpServerState::Stopped, McpServerState::Starting,
        McpServerState::AuthenticationRequired, McpServerState::Failed,
        McpServerState::Ready};
    for (const McpServerState state : states) {
        McpServerStatus status;
        status.state = state;
        const McpDependentItemOutcome outcome =
            McpLifecyclePolicy::dependentOutcome(
                QStringLiteral("acme.tools"), status);
        // 关键:归因始终是这台服务器。
        expect(outcome.attribution == McpFailureAttribution::Server,
               "a server failure was not attributed to the server");
        expect(!outcome.attributableToModel,
               "a server failure was reported as a model failure");
        expect(!outcome.reportedAsSuccess,
               "a server failure was reported as a successful tool result");
        // 补救说明必须针对这台服务器,否则人不知道该去修什么。
        expect(!outcome.remediation.isEmpty(),
               "a dependent failure carried no server-specific remediation");
        expect(!outcome.errorCode.isEmpty(),
               "a dependent failure carried no diagnostic");
        expect(outcome.serverId == QStringLiteral("acme.tools"),
               "a dependent failure did not name the server it came from");
    }

    // 每一种状态的诊断必须互不相同:合并后人无法分辨该补认证还是该查日志。
    QStringList codes;
    for (const McpServerState state : states) {
        McpServerStatus status;
        status.state = state;
        codes.append(McpLifecyclePolicy::dependentOutcome(
            QStringLiteral("acme.tools"), status).errorCode);
    }
    expect(QSet<QString>(codes.begin(), codes.end()).size() == codes.size(),
           "distinct server states collapsed into one diagnostic");

    // 未分类状态同样归因于服务器,而不是默默变成模型失败。
    McpServerStatus unknown;
    unknown.state = static_cast<McpServerState>(9999);
    const McpDependentItemOutcome outcome =
        McpLifecyclePolicy::dependentOutcome(QStringLiteral("acme.tools"), unknown);
    expect(!outcome.attributableToModel && !outcome.reportedAsSuccess,
           "an unclassified server state became a model failure");
    expect(outcome.errorCode == QStringLiteral("mcp-dependency-state-unknown"),
           "an unclassified server state carried no diagnostic");
}

// 审批必须展示服务器身份、工具、参数(机密遮蔽)、权限与持久化选项。
void approvalPromptTests()
{
    const McpToolApprovalPrompt prompt =
        McpLifecyclePolicy::approvalPrompt(request());
    expect(prompt.state == McpApprovalPromptState::Ready,
           "a presentable approval request was refused");
    expect(!prompt.serverFingerprint.isEmpty() && !prompt.serverLabel.isEmpty(),
           "the approval did not identify the server");
    expect(prompt.toolName == QStringLiteral("search_repository"),
           "the approval did not name the tool");
    // 参数必须逐个展示:看不到参数就不知道自己批准了什么。
    if (expect(prompt.argumentNames.size() == 2
                   && prompt.displayedValues.size() == 2,
               "the approval did not show the arguments")) {
        expect(prompt.displayedValues.at(0) == QStringLiteral("crash in parser"),
               "a non-secret argument was not shown");
        // 关键:机密被遮蔽。
        expect(prompt.displayedValues.at(1)
                   != QStringLiteral("sk-live-9f3a2b7c"),
               "a secret argument value was displayed in the approval");
    }
    // 遮蔽这件事本身可见:人需要知道自己在批准一个含凭据的调用。
    expect(prompt.redactedArguments == QStringList{QStringLiteral("api_token")},
           "the redaction was not disclosed to the approver");
    expect(prompt.requestedPermissions
               == QStringList{QStringLiteral("read-files")},
           "the requested permissions were not shown");
    // 持久化选项必须被列出。
    expect(prompt.persistenceOptions.size() == 2,
           "a read-only invocation offered no persistence choice");
    // 渲染一次审批不等于批准它。
    expect(!prompt.grantsInvocation, "rendering an approval granted invocation");

    // 越出只读边界的调用不提供记住选项:那样的授权必须每次单独作出。
    McpToolApprovalRequest write = request();
    write.requestedPermissions.append(QStringLiteral("filesystem-write"));
    const McpToolApprovalPrompt writePrompt =
        McpLifecyclePolicy::approvalPrompt(write);
    expect(writePrompt.beyondReadOnly,
           "a write permission was not disclosed");
    expect(writePrompt.persistenceOptions
               == QStringList{QStringLiteral("仅这一次")},
           "a beyond-read-only invocation offered a reusable rule");

    // 每一种可能承载机密的参数名都必须被遮蔽。
    for (const QString &name : {
             QStringLiteral("token"), QStringLiteral("api_key"),
             QStringLiteral("apiKey"), QStringLiteral("password"),
             QStringLiteral("Authorization"), QStringLiteral("session_cookie"),
             QStringLiteral("client_secret"), QStringLiteral("privateKey"),
             QStringLiteral("bearer_token"), QStringLiteral("credentials")}) {
        expect(McpLifecyclePolicy::secretBearing(name),
               "an argument name that can carry a secret was not redacted");
    }
    expect(!McpLifecyclePolicy::secretBearing(QStringLiteral("query")),
           "an ordinary argument name was treated as a secret");
}

// 无法安全展示的审批请求不能作为决定的依据。
void unpresentableApprovalTests()
{
    McpToolApprovalRequest badId = request();
    badId.serverId = QStringLiteral("Bad Id");
    expect(McpLifecyclePolicy::approvalPrompt(badId).errorCode
               == QStringLiteral("mcp-approval-server-id-invalid"),
           "a malformed server identifier was presented for approval");

    McpToolApprovalRequest badIdentity = request();
    badIdentity.serverIdentity = QStringLiteral("extension-content:sha256:abc");
    expect(McpLifecyclePolicy::approvalPrompt(badIdentity).errorCode
               == QStringLiteral("mcp-approval-identity-invalid"),
           "a truncated server identity was presented for approval");

    McpToolApprovalRequest spoofedTool = request();
    spoofedTool.toolName = QStringLiteral("search‮yreuq");
    expect(McpLifecyclePolicy::approvalPrompt(spoofedTool).state
               == McpApprovalPromptState::Unpresentable,
           "a bidirectional override in a tool name was presented");

    // 参数名与值数量不一致时会错位展示,人会批准一个自己没有看懂的调用。
    McpToolApprovalRequest misaligned = request();
    misaligned.argumentValues.removeLast();
    expect(McpLifecyclePolicy::approvalPrompt(misaligned).errorCode
               == QStringLiteral("mcp-approval-arguments-misaligned"),
           "misaligned arguments were presented for approval");

    McpToolApprovalRequest duplicated = request();
    duplicated.argumentNames = QStringList{QStringLiteral("query"),
                                           QStringLiteral("query")};
    duplicated.argumentValues = QStringList{QStringLiteral("a"),
                                            QStringLiteral("b")};
    expect(McpLifecyclePolicy::approvalPrompt(duplicated).errorCode
               == QStringLiteral("mcp-approval-argument-duplicate"),
           "duplicate argument names were presented for approval");

    McpToolApprovalRequest spoofedValue = request();
    spoofedValue.argumentValues[0] = QStringLiteral("crash‮resrap ni");
    expect(McpLifecyclePolicy::approvalPrompt(spoofedValue).state
               == McpApprovalPromptState::Unpresentable,
           "a bidirectional override in an argument value was presented");

    McpToolApprovalRequest flood = request();
    flood.argumentNames.clear();
    flood.argumentValues.clear();
    for (int index = 0; index <= McpLifecyclePolicy::MaxArguments; ++index) {
        flood.argumentNames.append(QStringLiteral("arg%1").arg(index));
        flood.argumentValues.append(QStringLiteral("value"));
    }
    expect(McpLifecyclePolicy::approvalPrompt(flood).errorCode
               == QStringLiteral("mcp-approval-argument-limit"),
           "an unbounded argument list was presented for approval");

    // 无法展示时不得泄露任何参数,也不得授予调用。
    const McpToolApprovalPrompt rejected =
        McpLifecyclePolicy::approvalPrompt(badId);
    expect(rejected.argumentNames.isEmpty() && rejected.displayedValues.isEmpty()
               && !rejected.grantsInvocation,
           "an unpresentable approval still produced argument rows");
}

// 日志必须有界,且截断本身可见。
void boundedLogTests()
{
    QList<McpLogLine> lines;
    for (int index = 0; index < McpLifecyclePolicy::MaxLogLines + 50; ++index) {
        lines.append({QStringLiteral("line %1").arg(index)});
    }
    const McpBoundedLog log = McpLifecyclePolicy::boundLog(lines);
    expect(log.lines.size() == McpLifecyclePolicy::MaxLogLines,
           "an unbounded log was not bounded");
    // 关键:截断可见。否则截断后的日志看起来像完整日志。
    expect(log.truncated && log.droppedLines == 50,
           "a truncated log did not disclose how much was dropped");
    // 保留最近的行:失败服务器的最后几行通常才是原因所在。
    expect(log.lines.last()
               == QStringLiteral("line %1")
                      .arg(McpLifecyclePolicy::MaxLogLines + 49),
           "the most recent log lines were discarded");

    // 单行过长同样被裁剪,并计为一次截断。
    const McpBoundedLog longLine = McpLifecyclePolicy::boundLog(
        {{QString(McpLifecyclePolicy::MaxLogLineLength + 10, QLatin1Char('x'))}});
    expect(longLine.lines.at(0).size() == McpLifecyclePolicy::MaxLogLineLength,
           "an unbounded log line was not clipped");
    expect(longLine.truncated,
           "clipping a log line was not disclosed");

    // 未超限时不声称截断。
    const McpBoundedLog small =
        McpLifecyclePolicy::boundLog({{QStringLiteral("ready")}});
    expect(!small.truncated && small.droppedLines == 0,
           "a log within limits was reported as truncated");
}

// 这一层不启动进程、不连接网络、不执行工具。
void authorityTests()
{
    for (const McpToolApprovalPrompt &prompt : {
             McpLifecyclePolicy::approvalPrompt(request()),
             McpLifecyclePolicy::approvalPrompt(McpToolApprovalRequest())}) {
        expect(!prompt.grantsInvocation,
               "some approval path granted invocation");
    }
    // 每一种已定义状态都有展示标签,否则界面会出现一个没有名字的状态。
    for (const McpServerState state : {
             McpServerState::Stopped, McpServerState::Starting,
             McpServerState::Ready, McpServerState::AuthenticationRequired,
             McpServerState::Failed}) {
        expect(!McpLifecyclePolicy::stateLabel(state).isEmpty(),
               "an MCP server state has no display label");
    }
    expect(!McpLifecyclePolicy::stateLabel(
               static_cast<McpServerState>(9999)).isEmpty(),
           "an unclassified server state has no display label");

    // 默认证据(未启用、未信任)必须结论为不启动。
    expect(McpLifecyclePolicy::evaluate(McpStartupEvidence()).state
               == McpServerState::Stopped,
           "a default evidence set concluded anything other than stopped");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    lifecycleStateTests();
    attributionTests();
    approvalPromptTests();
    unpresentableApprovalTests();
    boundedLogTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "mcp lifecycle policy tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
