#include "extension_lifecycle_presentation.h"

#include "extension_display_safety.h"
#include "extension_update_policy.h"

namespace {

using Safety = ExtensionDisplaySafety;

ExtensionRemovalPlan reject(const QString &code)
{
    ExtensionRemovalPlan plan;
    plan.state = ExtensionRemovalPlanState::Unpresentable;
    // 被拒绝时同样不删除任何内容，也同样保留身份。这些不变量在每一条返回路径上成立，
    // 而不是只在成功路径上被设置。
    plan.removesSourceContent = false;
    plan.retainsIdentity = true;
    plan.errorCode = code;
    return plan;
}

} // namespace

ExtensionRemovalPlan ExtensionLifecyclePresentation::buildRemoval(
    ExtensionKind kind, const QString &id,
    const ExtensionRegistryRecord *record,
    bool hasReviewPin, bool hasGrant)
{
    if (!Safety::validId(id)) {
        return reject(QStringLiteral("extension-removal-plan-id-invalid"));
    }

    // 这次移除是否成立只有一个来源：判定层。呈现层自己再判一遍必然会与它漂移，而漂移的
    // 方向是界面提供一个判定层会拒绝的移除动作。
    const ExtensionRemovalVerdict verdict =
        ExtensionUpdatePolicy::evaluateRemoval(kind, id, record);
    if (verdict.state != ExtensionRemovalState::Ready) {
        return reject(verdict.errorCode.isEmpty()
            ? QStringLiteral("extension-removal-plan-rejected") : verdict.errorCode);
    }

    ExtensionRemovalPlan plan;
    plan.identifier = id;
    plan.kind = kind;
    plan.kindLabel = Safety::kindLabel(kind);
    plan.withdrawsGrant = hasGrant;
    plan.withdrawsReview = hasReviewPin;
    // 判定层说身份被保留，控制器也确实保留它；这里原样转述，不另行构造一份可能不同的
    // 身份串。
    plan.retainsIdentity = verdict.retainsIdentityMetadata;
    plan.retainedIdentity = verdict.retainedIdentity;
    // 移除不删除磁盘上的内容：控制器只写两份账本。判定层的 removesExecutableContent
    // 说的是"可执行内容被停用"，而在当前产品里停用的手段就是收回授权，不是删除文件。
    // 因此这里必须是假，界面也必须据此说明，否则人会以为内容已经消失而停止清理。
    plan.removesSourceContent = false;

    if (!record) {
        // 来源已消失。移除仍然进行——一个被删掉来源的扩展必须仍然能被收回授权——但界面
        // 必须说明目标已不存在，否则人会以为自己移除的是屏幕上仍然列出的某一项。
        plan.targetAbsent = true;
        plan.title = id;
        plan.state = ExtensionRemovalPlanState::Ready;
        return plan;
    }

    // 名称来自不可信来源。不可展示时退回使用标识本身，而不是拒绝移除：移除必须始终可用，
    // 否则一个把自己的名字做成不可展示文本的扩展就无法被收回授权。
    plan.title = Safety::safeDisplayText(record->name, MaxTitleCharacters)
        ? record->name : id;
    plan.sourceIdentity = Safety::hashIdentity(
            record->sourceIdentity, QStringLiteral("extension-source:sha256:"))
        ? record->sourceIdentity : QString();
    plan.contentIdentity = Safety::hashIdentity(
            record->contentIdentity, QStringLiteral("extension-content:sha256:"))
        ? record->contentIdentity : QString();
    plan.state = ExtensionRemovalPlanState::Ready;
    return plan;
}
