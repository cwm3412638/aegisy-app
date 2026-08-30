#ifndef MCP_LIFECYCLE_POLICY_H
#define MCP_LIFECYCLE_POLICY_H

#include <QList>
#include <QString>
#include <QStringList>

// MCP 服务器生命周期与工具可观察性。一个 MCP 服务器是**外部进程**,因此它有一整套自己的
// 失败方式:没启动、启动了但没就绪、要求认证、中途退出。这一层存在的理由是这些失败必须
// 各自可辨认。
//
// 最要紧的一条:依赖一个失败服务器的回合必须以**服务器专属**的失败结束,既不能报成模型
// 失败,也不能报成一次成功的工具结果。报成模型失败会让人去修提示词,而问题在服务器;报成
// 成功结果更糟——模型会把一个不存在的返回值当作事实继续往下推理,而没有任何人知道那个值
// 是凭空出现的。归因错误不是显示问题,它决定了人接下来会去修什么。
//
// 审批同样有自己的要求:批准一次工具调用时,人必须看到服务器身份、工具名、参数、请求的
// 权限与持久化选项,而参数里的机密必须被遮蔽。看不到参数就不知道自己批准了什么;看得到
// 未遮蔽的机密则等于把凭据写进审批记录。
//
// 这一层不启动进程、不连接网络、不执行工具。它把一组已观测到的状态变成可判定的结论。
enum class McpServerState {
    // 尚未获得启用与信任,因此不应启动。
    Stopped,
    Starting,
    // 已完成握手,工具清单可用。
    Ready,
    // 需要认证才能继续。它与 Failed 是不同的结论:认证缺失可由人补上。
    AuthenticationRequired,
    Failed,
};

// 一次启动尝试所依据的证据。
struct McpStartupEvidence {
    // 生效启用与信任。两者齐备才允许启动。
    bool effectivelyEnabled = false;
    bool trusted = false;
    // 握手是否完成。
    bool handshakeCompleted = false;
    // 服务器是否要求认证。
    bool authenticationRequired = false;
    // 进程是否已退出。
    bool exited = false;
    // 服务器自己报告的失败原因,原样保留供人检视。
    QString reportedFailure;
};

struct McpServerStatus {
    McpServerState state = McpServerState::Stopped;
    // 固定诊断代码。
    QString errorCode;
    // 该结论是否可由人补充认证后继续。
    bool resolvableByAuthentication = false;
    // 该服务器的工具清单是否可用。
    bool toolsAvailable = false;
};

// 一条被观测到的日志行。日志必须有界:一个失败服务器可以无限输出,而无界日志会挤掉
// 其他一切。
struct McpLogLine {
    QString text;
};

struct McpBoundedLog {
    QStringList lines;
    // 被丢弃的行数。它必须可见,否则截断后的日志看起来像完整日志。
    int droppedLines = 0;
    bool truncated = false;
};

// 一次工具调用审批请求。
struct McpToolApprovalRequest {
    QString serverId;
    QString serverIdentity;
    QString toolName;
    // 参数名到值的扁平列表,顺序保持不变。
    QStringList argumentNames;
    QStringList argumentValues;
    QStringList requestedPermissions;
};

enum class McpApprovalPromptState {
    Ready,
    // 无法安全展示,因此不能作为决定的依据。
    Unpresentable,
};

struct McpToolApprovalPrompt {
    McpApprovalPromptState state = McpApprovalPromptState::Unpresentable;
    QString serverLabel;
    QString serverFingerprint;
    QString toolName;
    // 与 `argumentNames` 一一对应的展示值,其中机密已被遮蔽。
    QStringList argumentNames;
    QStringList displayedValues;
    // 被遮蔽的参数名。它必须可见:人需要知道自己在批准一个含凭据的调用。
    QStringList redactedArguments;
    QStringList requestedPermissions;
    // 请求了越出只读边界的权限。
    bool beyondReadOnly = false;
    // 可供选择的持久化选项。
    QStringList persistenceOptions;
    // 恒为假:渲染一次审批不等于批准它。
    bool grantsInvocation = false;
    QString errorCode;
};

enum class McpFailureAttribution {
    // 归因于这台服务器,并带有针对它的补救说明。
    Server,
};

// 依赖失败服务器的回合以此结束。
struct McpDependentItemOutcome {
    McpFailureAttribution attribution = McpFailureAttribution::Server;
    QString serverId;
    QString errorCode;
    // 针对这台服务器的补救说明。
    QString remediation;
    // 恒为假:服务器故障不是模型失败。
    bool attributableToModel = false;
    // 恒为假:它更不是一次成功的工具结果。
    bool reportedAsSuccess = false;
};

class McpLifecyclePolicy
{
public:
    static constexpr int MaxLogLines = 512;
    static constexpr int MaxLogLineLength = 4096;
    static constexpr int MaxArguments = 64;

    static McpServerStatus evaluate(const McpStartupEvidence &evidence);

    // 依赖一个失败或未就绪服务器的回合的结论。
    static McpDependentItemOutcome dependentOutcome(
        const QString &serverId, const McpServerStatus &status);

    static McpToolApprovalPrompt approvalPrompt(
        const McpToolApprovalRequest &request);

    // 日志有界。超出上限时丢弃最旧的行并把丢弃数量记下来。
    static McpBoundedLog boundLog(const QList<McpLogLine> &lines);

    // 该参数名是否可能承载机密,因此其值必须被遮蔽。
    static bool secretBearing(const QString &argumentName);

    static QString stateLabel(McpServerState state);
};

#endif // MCP_LIFECYCLE_POLICY_H
