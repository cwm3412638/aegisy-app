#include "instruction_context_manifest.h"

#include "extension_display_safety.h"

#include <QSet>

namespace {

using Safety = ExtensionDisplaySafety;

const QString &contentPrefix()
{
    static const QString value = QStringLiteral("extension-content:sha256:");
    return value;
}

InstructionContextManifest unusable(const QString &code)
{
    InstructionContextManifest manifest;
    manifest.state = InstructionManifestState::Unusable;
    // 拒绝路径上同样不授予执行。这一点在每一条返回路径上成立。
    manifest.grantsExecution = false;
    manifest.errorCode = code;
    return manifest;
}

// 表达策略的行为。只有 Managed 来源可以声明它们;其他来源声明时被拒绝,因为磁盘上的
// 文本与一段普通说明在字节层面无从区分。
bool assertsPolicy(const QString &behavior)
{
    static const QStringList claims{
        QStringLiteral("policy"),
        QStringLiteral("trusted"),
        QStringLiteral("override"),
        QStringLiteral("bypass"),
        QStringLiteral("grant"),
        QStringLiteral("always-allow"),
        QStringLiteral("skip-approval"),
        QStringLiteral("disable-sandbox"),
    };
    const QString lowered = behavior.toLower();
    for (const QString &claim : claims) {
        if (lowered.contains(claim)) return true;
    }
    return false;
}

} // namespace

int InstructionContextPolicy::basePrecedence(InstructionSourceKind kind)
{
    switch (kind) {
    case InstructionSourceKind::Managed:
        return 0;
    case InstructionSourceKind::UserGlobal:
        return 1000;
    case InstructionSourceKind::ProjectRoot:
        return 2000;
    // 嵌套项目指令覆盖项目根指令,具体位置由目录深度决定:越靠近目标文件优先级越高。
    case InstructionSourceKind::ProjectNested:
        return 3000;
    // Skill 内容是这一轮被显式调用的,因此最后生效。
    case InstructionSourceKind::Skill:
        return 4000;
    }
    // 未分类来源排在最低优先级:一个还未被纳入模型的来源不应默默覆盖已有指令。
    return 100000;
}

bool InstructionContextPolicy::policyAuthority(InstructionSourceKind kind)
{
    switch (kind) {
    case InstructionSourceKind::Managed:
        return true;
    case InstructionSourceKind::UserGlobal:
    case InstructionSourceKind::ProjectRoot:
    case InstructionSourceKind::ProjectNested:
    case InstructionSourceKind::Skill:
        return false;
    }
    // 未分类来源没有策略权威:新增来源不得凭默认值获得表达策略的资格。
    return false;
}

QString InstructionContextPolicy::sourceLabel(InstructionSourceKind kind)
{
    switch (kind) {
    case InstructionSourceKind::Managed:
        return QStringLiteral("组织策略");
    case InstructionSourceKind::UserGlobal:
        return QStringLiteral("全局用户指令");
    case InstructionSourceKind::ProjectRoot:
        return QStringLiteral("项目根指令");
    case InstructionSourceKind::ProjectNested:
        return QStringLiteral("嵌套项目指令");
    case InstructionSourceKind::Skill:
        return QStringLiteral("Skill 指令");
    }
    return QStringLiteral("未知来源");
}

InstructionContextManifest InstructionContextPolicy::build(
    const QList<InstructionSource> &sources,
    const QList<SkillInvocationRecord> &invocations)
{
    if (sources.size() > MaxSources) {
        return unusable(QStringLiteral("instruction-source-limit"));
    }

    InstructionContextManifest manifest;
    manifest.grantsExecution = false;

    QSet<QString> seenPaths;
    QList<InstructionManifestEntry> entries;
    for (const InstructionSource &source : sources) {
        // 没有出处的指令无法追溯,因此不能进入清单。
        if (source.sourcePath.isEmpty()
                || !Safety::safeDisplayText(source.sourcePath, 1024)) {
            return unusable(QStringLiteral("instruction-source-path-unsafe"));
        }
        // 同一个路径出现两次时无法判断哪一份内容生效。
        if (seenPaths.contains(source.sourcePath)) {
            return unusable(QStringLiteral("instruction-source-duplicate"));
        }
        seenPaths.insert(source.sourcePath);
        if (!Safety::hashIdentity(source.contentIdentity, contentPrefix())) {
            return unusable(QStringLiteral("instruction-content-identity-invalid"));
        }
        if (source.directoryDepth < 0
                || source.directoryDepth > MaxDirectoryDepth) {
            return unusable(QStringLiteral("instruction-depth-invalid"));
        }
        if (source.contextBytes < 0) {
            return unusable(QStringLiteral("instruction-context-bytes-invalid"));
        }

        InstructionManifestEntry entry;
        entry.kind = source.kind;
        entry.sourcePath = source.sourcePath;
        entry.contentFingerprint = Safety::fingerprint(source.contentIdentity);
        entry.includedReferences = source.includedReferences;
        entry.contextBytes = source.contextBytes;
        entry.policyAuthority = policyAuthority(source.kind);
        // 嵌套指令的优先级由目录深度决定:越靠近目标文件越靠后生效,因此覆盖更外层的。
        // 深度以负号计入,使更深的目录得到更小的数值,与"数值越小越先生效"保持一致——
        // 但仍然落在 ProjectNested 的区间内,不会越过 Skill 或 Managed。
        entry.precedence = basePrecedence(source.kind);
        if (source.kind == InstructionSourceKind::ProjectNested) {
            entry.precedence += MaxDirectoryDepth - source.directoryDepth;
        }
        entry.precedenceLabel = sourceLabel(source.kind);

        for (const QString &behavior : source.requestedBehaviors) {
            if (!Safety::safeDisplayText(behavior, 128)) {
                return unusable(QStringLiteral("instruction-behavior-unsafe"));
            }
            // 磁盘上的指令文本不是策略。非 Managed 来源试图表达策略时被拒绝,而指令
            // 本身仍然留在链上未被改写:改写会让下一个读清单的人看到一条看似合法的
            // 授权,而它的来源其实是不可信磁盘内容。
            if (assertsPolicy(behavior) && !entry.policyAuthority) {
                InstructionDenial denial;
                denial.sourcePath = source.sourcePath;
                denial.behavior = behavior;
                denial.reason = InstructionDenialReason::NotPolicyAuthority;
                denial.errorCode =
                    QStringLiteral("instruction-not-policy-authority");
                manifest.denials.append(denial);
                continue;
            }
            // 运行时策略始终胜出。越出只读边界的行为被拒绝并可见,而不是被采纳。
            if (Safety::beyondReadOnly(behavior)) {
                InstructionDenial denial;
                denial.sourcePath = source.sourcePath;
                denial.behavior = behavior;
                denial.reason =
                    InstructionDenialReason::ForbiddenByRuntimePolicy;
                denial.errorCode =
                    QStringLiteral("instruction-forbidden-by-runtime-policy");
                manifest.denials.append(denial);
                continue;
            }
            entry.acceptedBehaviors.append(behavior);
        }

        for (const QString &reference : source.includedReferences) {
            if (!Safety::safeDisplayText(reference, 1024)) {
                return unusable(QStringLiteral("instruction-reference-unsafe"));
            }
        }

        manifest.totalContextBytes += source.contextBytes;
        entries.append(entry);
    }

    // 上下文预算必须被核算而不是被截断:悄悄丢掉一段指令会让清单与模型实际看到的内容
    // 不一致,而清单存在的全部理由就是这两者一致。
    if (manifest.totalContextBytes > MaxContextBytes) {
        return unusable(QStringLiteral("instruction-context-budget-exceeded"));
    }

    for (const SkillInvocationRecord &invocation : invocations) {
        if (!Safety::validId(invocation.id)) {
            return unusable(QStringLiteral("instruction-skill-id-invalid"));
        }
        if (!Safety::hashIdentity(invocation.contentIdentity, contentPrefix())) {
            return unusable(QStringLiteral("instruction-skill-identity-invalid"));
        }
        if (invocation.sourcePath.isEmpty()
                || !Safety::safeDisplayText(invocation.sourcePath, 1024)) {
            return unusable(QStringLiteral("instruction-skill-path-unsafe"));
        }
        if (!Safety::safeDisplayText(invocation.version, 64)) {
            return unusable(QStringLiteral("instruction-skill-version-unsafe"));
        }

        SkillInvocationRecord record = invocation;
        // 越出只读边界的脚本或工具权限被记录并拒绝,而不是静默采纳。它们仍然留在
        // `scriptPermissions` 里:失败关闭不等于把证据一起丢掉。
        record.deniedPermissions.clear();
        for (const QString &permission : invocation.scriptPermissions) {
            if (!Safety::safeDisplayText(permission, 128)) {
                return unusable(QStringLiteral("instruction-skill-permission-unsafe"));
            }
            if (Safety::beyondReadOnly(permission)) {
                record.deniedPermissions.append(permission);
            }
        }
        manifest.skillInvocations.append(record);
    }

    // 按生效优先级排序。相同优先级时按路径排序,使清单在同一输入下始终一致——顺序不
    // 确定的覆盖链无法用来解释任何事情。
    for (int outer = 1; outer < entries.size(); ++outer) {
        const InstructionManifestEntry current = entries.at(outer);
        int inner = outer - 1;
        while (inner >= 0
               && (entries.at(inner).precedence > current.precedence
                   || (entries.at(inner).precedence == current.precedence
                       && entries.at(inner).sourcePath > current.sourcePath))) {
            entries[inner + 1] = entries.at(inner);
            --inner;
        }
        entries[inner + 1] = current;
    }

    manifest.chain = entries;
    manifest.state = InstructionManifestState::Ready;
    return manifest;
}
