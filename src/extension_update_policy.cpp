#include "extension_update_policy.h"

#include "extension_display_safety.h"

namespace {

using Safety = ExtensionDisplaySafety;

const QString &sourcePrefix()
{
    static const QString value = QStringLiteral("extension-source:sha256:");
    return value;
}

const QString &contentPrefix()
{
    static const QString value = QStringLiteral("extension-content:sha256:");
    return value;
}

ExtensionUpdateVerdict rejectUpdate(const QString &code)
{
    ExtensionUpdateVerdict verdict;
    verdict.state = ExtensionUpdateState::Rejected;
    // 被拒绝时当前版本仍然保持不变,候选仍然不可执行,也仍然不继承任何权威。这些不变量
    // 在每一条返回路径上都成立,而不是只在成功路径上被设置。
    verdict.activePreserved = true;
    verdict.candidateExecutable = false;
    verdict.inheritsTrust = false;
    verdict.inheritsGrant = false;
    verdict.errorCode = code;
    return verdict;
}

// 版本号比较仅用于把降级标记出来,不参与任何权威判定:一个可控的版本字符串不得决定
// 内容是否可信。
QList<int> versionParts(const QString &version)
{
    QList<int> parts;
    const QStringList tokens = version.split(QLatin1Char('.'));
    for (const QString &token : tokens) {
        bool ok = false;
        const int value = token.toInt(&ok);
        if (!ok || value < 0) return QList<int>();
        parts.append(value);
    }
    return parts;
}

bool isDowngrade(const QString &activeVersion, const QString &candidateVersion)
{
    const QList<int> active = versionParts(activeVersion);
    const QList<int> candidate = versionParts(candidateVersion);
    // 任一侧无法比较时不声称是降级:那会把"不知道"表述成一个具体结论。
    if (active.isEmpty() || candidate.isEmpty()) return false;
    const int count = qMax(active.size(), candidate.size());
    for (int index = 0; index < count; ++index) {
        const int left = index < active.size() ? active.at(index) : 0;
        const int right = index < candidate.size() ? candidate.at(index) : 0;
        if (right < left) return true;
        if (right > left) return false;
    }
    return false;
}

} // namespace

bool ExtensionUpdatePolicy::reviewTransfers(
    const ExtensionReviewPin &pin, const ExtensionUpdateCandidate &candidate)
{
    // 复核绑定的是确切内容。只有种类、ID、来源与内容摘要全部一致时,这份复核才仍然
    // 描述眼前这份内容——而那种情况下并不存在"更新"。任何按 ID 或版本号的传递都会让
    // 任意新内容以上一版的权威运行。
    return pin.kind == candidate.kind
        && !pin.id.isEmpty()
        && pin.id == candidate.id
        && !pin.contentIdentity.isEmpty()
        && pin.contentIdentity == candidate.contentIdentity
        && !pin.sourceIdentity.isEmpty()
        && pin.sourceIdentity == candidate.sourceIdentity;
}

ExtensionUpdateVerdict ExtensionUpdatePolicy::evaluate(
    const ExtensionRegistryRecord &active,
    const ExtensionUpdateCandidate &candidate,
    const ExtensionUpdateEvidence &evidence)
{
    if (!Safety::validId(candidate.id) || candidate.id != active.id
            || candidate.kind != active.kind) {
        return rejectUpdate(QStringLiteral("extension-update-target-mismatch"));
    }
    if (!Safety::hashIdentity(candidate.contentIdentity, contentPrefix())
            || !Safety::hashIdentity(candidate.sourceIdentity, sourcePrefix())) {
        return rejectUpdate(QStringLiteral("extension-update-identity-invalid"));
    }
    // 内容摘要与当前版本相同的"更新"不是更新。把它当作更新会让一次无变化的操作推进
    // 状态,并且可能被用来刷掉一条尚未被处理的漂移诊断。
    if (candidate.contentIdentity == active.contentIdentity) {
        return rejectUpdate(QStringLiteral("extension-update-content-unchanged"));
    }

    // 校验必须逐项成立。任何一项失败时当前版本保持不变,候选不执行。
    if (!evidence.signatureValid) {
        return rejectUpdate(QStringLiteral("extension-update-signature-invalid"));
    }
    if (!evidence.manifestValid) {
        return rejectUpdate(QStringLiteral("extension-update-manifest-invalid"));
    }
    if (!evidence.compatible) {
        return rejectUpdate(QStringLiteral("extension-update-incompatible"));
    }
    if (!evidence.dependenciesSatisfied) {
        return rejectUpdate(QStringLiteral("extension-update-dependency-unsatisfied"));
    }
    if (!evidence.healthy) {
        return rejectUpdate(QStringLiteral("extension-update-health-failed"));
    }

    ExtensionUpdateVerdict verdict;
    verdict.state = ExtensionUpdateState::StagedUnreviewed;
    // 校验通过只意味着候选可以被暂存。当前版本仍然不被这一层改动:替换是另一次显式操作。
    verdict.activePreserved = true;
    // 候选按定义是另一份内容,因此它从未复核、未授权。这三个不变量是这一层存在的理由:
    // 让信任或授权按 ID 传递,等于把"更新"变成让任意新内容以上一版权威运行的通道。
    verdict.candidateExecutable = false;
    verdict.inheritsTrust = false;
    verdict.inheritsGrant = false;
    // 降级不被禁止,但必须可见:它会重新引入已经被修复过的内容。
    verdict.downgrade = isDowngrade(active.version, candidate.version);
    return verdict;
}

ExtensionRemovalVerdict ExtensionUpdatePolicy::evaluateRemoval(
    ExtensionKind kind, const QString &id,
    const ExtensionRegistryRecord *record)
{
    ExtensionRemovalVerdict verdict;
    if (!Safety::validId(id)) {
        verdict.state = ExtensionRemovalState::Rejected;
        verdict.errorCode = QStringLiteral("extension-removal-id-invalid");
        return verdict;
    }
    if (record && (record->id != id || record->kind != kind)) {
        verdict.state = ExtensionRemovalState::Rejected;
        verdict.errorCode = QStringLiteral("extension-removal-target-mismatch");
        return verdict;
    }

    verdict.state = ExtensionRemovalState::Ready;
    verdict.removesExecutableContent = true;
    // 身份元数据保留。抹掉它会让"这份内容曾被授权运行过"的历史一并消失,使事后审计
    // 无从进行——而移除恰好是最需要留下记录的操作之一。
    verdict.retainsIdentityMetadata = true;
    // 移除必须收回授权。留着授权会让同名同类的内容重新出现时直接继承它,那正是内容
    // 绑定授权要防住的替换。
    verdict.retainsGrant = false;
    // 目标已经消失时仍然保留可辨识的身份:撤销一份不再存在的目标同样需要留下记录。
    const QString contentIdentity = record ? record->contentIdentity : QString();
    verdict.retainedIdentity = QString::number(static_cast<int>(kind))
        + QLatin1Char(':') + id + QLatin1Char(':')
        + (contentIdentity.isEmpty() ? QStringLiteral("absent") : contentIdentity);
    return verdict;
}
