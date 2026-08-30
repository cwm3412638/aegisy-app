#ifndef INSTRUCTION_CONTEXT_MANIFEST_H
#define INSTRUCTION_CONTEXT_MANIFEST_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

// 指令上下文清单。项目指令与 Skill 内容都是**模型可见**的文本,而模型可见的文本就是模型
// 会照着做的文本。这带来两个独立的问题,而它们的答案方向相反。
//
// 第一个是可检视性:如果无法确定这一轮究竟加载了哪些指令、来自何处、以什么优先级生效,
// 那么当模型的行为出乎意料时,唯一的排查办法是猜。嵌套目录让这件事更严重——一个更靠近
// 目标文件的目录里的指令会覆盖更外层的,而覆盖关系必须是清单里的一条记录,而不是读代码
// 才能推断出来的事实。
//
// 第二个是权威:磁盘上的指令文本不是策略。一段写着"你可以直接执行命令"的项目指令与一段
// 普通说明在字节层面无从区分,因此运行时策略必须始终胜出,而拒绝必须可见。**关键在于这
// 一层不把被拒绝的指令改写成一条已授权的策略**:改写会让下一个读清单的人看到一条看似
// 合法的授权,而它的来源其实是不可信磁盘内容。被拒绝的指令原样保留,并单独标注拒绝理由。
//
// 这一层不加载文件、不执行任何东西、不改写策略。它把一组已读取的指令来源变成一份可判定
// 的清单。
enum class InstructionSourceKind {
    // 全局用户指令。
    UserGlobal,
    // 项目根目录指令。
    ProjectRoot,
    // 更靠近目标文件的嵌套目录指令。
    ProjectNested,
    // 一个被调用的 Skill 携带的指令内容。
    Skill,
    // 组织策略下发的指令。它是唯一可以表达策略的来源。
    Managed,
};

// 一段模型可见的指令来源。`directoryDepth` 只对嵌套项目指令有意义:数值越大表示越靠近
// 目标文件,因此优先级越高。
struct InstructionSource {
    InstructionSourceKind kind = InstructionSourceKind::ProjectNested;
    // 该来源在磁盘上的出处。它必须出现在清单里:没有出处的指令无法追溯。
    QString sourcePath;
    // 内容摘要。指令内容变化时清单里的条目也随之变化,因此可以事后比对。
    QString contentIdentity;
    int directoryDepth = 0;
    // 该来源请求的行为。运行时策略禁止的项会被拒绝,而不是被采纳。
    QStringList requestedBehaviors;
    // 该来源引用的其他文件。它们同样是模型可见内容,因此必须一并记录。
    QStringList includedReferences;
    // 计入上下文预算的字节数。
    qint64 contextBytes = 0;
};

// 一次 Skill 调用需要记录的身份。没有这些字段,事后无法确定这一轮到底运行了什么。
struct SkillInvocationRecord {
    QString id;
    QString version;
    QString sourcePath;
    QString contentIdentity;
    QStringList includedReferences;
    // 该 Skill 声明的脚本或工具权限。为空表示它只提供文本。
    QStringList scriptPermissions;
    // 其中越出当前只读边界的权限。它们被记录并拒绝,而不是静默采纳。
    QStringList deniedPermissions;
};

enum class InstructionDenialReason {
    // 该行为被运行时策略禁止。
    ForbiddenByRuntimePolicy,
    // 非 Managed 来源试图表达策略。磁盘上的文本不是策略。
    NotPolicyAuthority,
};

// 一条可见的拒绝。指令原文不被改写,拒绝单独记录。
struct InstructionDenial {
    QString sourcePath;
    QString behavior;
    InstructionDenialReason reason = InstructionDenialReason::ForbiddenByRuntimePolicy;
    QString errorCode;
};

// 清单里的一条指令记录,带它的生效优先级。
struct InstructionManifestEntry {
    InstructionSourceKind kind = InstructionSourceKind::ProjectNested;
    QString sourcePath;
    QString contentFingerprint;
    // 生效优先级。数值越小越先生效。
    int precedence = 0;
    // 该条目在链中的位置说明,用于向人解释覆盖关系。
    QString precedenceLabel;
    QStringList includedReferences;
    // 被采纳的行为。被拒绝的行为不在这里,而在 `denials` 中。
    QStringList acceptedBehaviors;
    qint64 contextBytes = 0;
    // 该来源是否可以表达策略。只有 Managed 为真。
    bool policyAuthority = false;
};

enum class InstructionManifestState {
    Ready,
    // 来源集合本身无法作为依据。
    Unusable,
};

struct InstructionContextManifest {
    InstructionManifestState state = InstructionManifestState::Unusable;
    // 按生效优先级排序的完整指令链。
    QList<InstructionManifestEntry> chain;
    QList<SkillInvocationRecord> skillInvocations;
    // 每一条被拒绝的指令都在这里,且对应指令仍然留在链上未被改写。
    QList<InstructionDenial> denials;
    qint64 totalContextBytes = 0;
    // 恒为假:这一层不执行任何东西。
    bool grantsExecution = false;
    QString errorCode;
};

class InstructionContextPolicy
{
public:
    static constexpr int MaxSources = 256;
    static constexpr int MaxDirectoryDepth = 64;
    static constexpr qint64 MaxContextBytes = 4 * 1024 * 1024;

    static InstructionContextManifest build(
        const QList<InstructionSource> &sources,
        const QList<SkillInvocationRecord> &invocations);

    // 来源类别的基础优先级。数值越小越先生效。
    static int basePrecedence(InstructionSourceKind kind);

    // 该来源是否有资格表达策略。只有组织策略下发的指令有。
    static bool policyAuthority(InstructionSourceKind kind);

    static QString sourceLabel(InstructionSourceKind kind);
};

#endif // INSTRUCTION_CONTEXT_MANIFEST_H
