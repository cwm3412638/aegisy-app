#include "extension_update_presentation.h"

#include "extension_display_safety.h"

namespace {

using Safety = ExtensionDisplaySafety;

const QString &contentPrefix()
{
    static const QString value = QStringLiteral("extension-content:sha256:");
    return value;
}

// 每一条返回路径都必须写出这三个恒定字段。一个被拒绝的计划同样不替换当前版本、同样不授予
// 执行权,而把它们留给结构体默认值意味着源码里没有任何一处声明这件事。
ExtensionUpdatePlan reject(const QString &code)
{
    ExtensionUpdatePlan plan;
    plan.state = ExtensionUpdatePlanState::Unpresentable;
    plan.stagesOnly = true;
    plan.replacesActiveVersion = false;
    plan.grantsExecution = false;
    plan.evidenceIncomplete = true;
    plan.errorCode = code;
    return plan;
}

// 一项证据的呈现。established 为假而 unverifiable 为真表示没有人能核查它;两者都为假表示
// 核查过并且没通过。这两种情况把人送去不同的地方,因此绝不能并成一句"证据不足"。
ExtensionUpdateEvidenceLine line(const QString &label, bool established,
                                 const QString &gap, bool unverifiable)
{
    ExtensionUpdateEvidenceLine item;
    item.label = label;
    item.established = established;
    item.unverifiable = !established && unverifiable;
    // 诊断只在这一项没有被确立时才有意义。确立了还带诊断会让人去查一个不存在的问题。
    item.diagnostic = established ? QString() : gap;
    return item;
}

} // namespace

QString ExtensionUpdatePresentation::stateLabel(ExtensionUpdatePlanState state)
{
    switch (state) {
    case ExtensionUpdatePlanState::NoCandidate:
        return QStringLiteral("尚未选择候选包");
    case ExtensionUpdatePlanState::Stageable:
        // 明确说清楚这是暂存而不是完成:写"更新已完成"会让人认为新版本正在运行。
        return QStringLiteral("候选可以暂存；暂存不会让它运行");
    case ExtensionUpdatePlanState::Blocked:
        return QStringLiteral("这次更新不能进行；当前版本保持不变");
    case ExtensionUpdatePlanState::Unpresentable:
        return QStringLiteral("候选无法安全展示，不能作为决定的依据");
    }
    return QStringLiteral("候选无法安全展示，不能作为决定的依据");
}

ExtensionUpdatePlan ExtensionUpdatePresentation::buildEmpty(
    const ExtensionRegistryRecord &active)
{
    if (!Safety::validId(active.id)) {
        return reject(QStringLiteral("extension-update-plan-id-invalid"));
    }
    ExtensionUpdatePlan plan;
    plan.state = ExtensionUpdatePlanState::NoCandidate;
    plan.identifier = active.id;
    plan.kind = active.kind;
    plan.kindLabel = Safety::kindLabel(active.kind);
    plan.title = Safety::safeDisplayText(active.name, MaxTitleCharacters)
        ? active.name : active.id;
    plan.activeVersionLabel =
        Safety::safeDisplayText(active.version, 64) ? active.version : QString();
    plan.activeFingerprint =
        Safety::hashIdentity(active.contentIdentity, contentPrefix())
            ? Safety::fingerprint(active.contentIdentity) : QString();
    // 空计划同样不替换、不授权。
    plan.stagesOnly = true;
    plan.replacesActiveVersion = false;
    plan.grantsExecution = false;
    plan.evidenceIncomplete = true;
    return plan;
}

ExtensionUpdatePlan ExtensionUpdatePresentation::build(
    const ExtensionRegistryRecord &active,
    const ExtensionUpdateCandidateResult &candidate,
    const ExtensionUpdateVerdict &verdict)
{
    // 还没有候选包时没有任何可判定的东西:构造一份计划会让屏幕上出现一个凭空的证据表。
    if (candidate.state == ExtensionUpdateCandidateState::Absent) {
        return buildEmpty(active);
    }

    // 标识只在一处被读:`buildEmpty`。这里不再复查一遍——复查那一遍无论删掉与否结论都一样,
    // 于是它是一段没有任何测试能证明其存在意义的代码,而这种代码日后会被当成真正的防线。
    ExtensionUpdatePlan plan = buildEmpty(active);
    if (plan.state == ExtensionUpdatePlanState::Unpresentable) return plan;

    // 候选自己读不出来或畸形时,这一层不构造证据表:那张表描述的会是一份没有被读出来的内容。
    if (candidate.state != ExtensionUpdateCandidateState::Ready) {
        plan.state = ExtensionUpdatePlanState::Blocked;
        // 产出层自己的诊断原样带出。这一层再编一个代号会让人拿着一个查不到出处的东西。
        plan.errorCode = candidate.errorCode;
        plan.evidenceIncomplete = true;
        plan.stagesOnly = true;
        plan.replacesActiveVersion = false;
        plan.grantsExecution = false;
        return plan;
    }

    plan.candidateVersionLabel =
        Safety::safeDisplayText(candidate.candidate.version, 64)
            ? candidate.candidate.version : QString();
    plan.candidateFingerprint =
        Safety::hashIdentity(candidate.candidate.contentIdentity, contentPrefix())
            ? Safety::fingerprint(candidate.candidate.contentIdentity) : QString();
    // 降级必须显式说出来:两个版本号并排放着不会让人注意到方向,而降级会重新引入已经被
    // 修复过的内容。结论来自判定层。
    plan.downgrade = verdict.downgrade;
    // 逐组件披露原样带出:判定用并集,展示用逐组件。汇总会让两个组件各自请求"读文件"与
    // "连网"看起来与一个组件同时请求两者完全一样。
    plan.components = candidate.manifest.components;

    // 逐项列出证据,齐备的项也列出:人有权看到这次更新凭什么成立。三个"没有人能核查"的项
    // 必须与"核查失败"分开,否则人会以为是自己这个包有问题而反复重做包。
    plan.evidence = {
        line(QStringLiteral("签名"), candidate.evidence.signatureValid,
             candidate.gaps.signature, true),
        line(QStringLiteral("清单"), candidate.evidence.manifestValid,
             candidate.gaps.manifest, false),
        line(QStringLiteral("兼容性"), candidate.evidence.compatible,
             candidate.gaps.compatibility, false),
        line(QStringLiteral("依赖"), candidate.evidence.dependenciesSatisfied,
             candidate.gaps.dependencies, true),
        line(QStringLiteral("健康检查"), candidate.evidence.healthy,
             candidate.gaps.health, true),
    };
    plan.evidenceIncomplete = false;
    plan.anyUnverifiable = false;
    for (const ExtensionUpdateEvidenceLine &item : plan.evidence) {
        if (!item.established) plan.evidenceIncomplete = true;
        if (item.unverifiable) plan.anyUnverifiable = true;
    }

    // 这次更新是否成立只有一个来源:判定层。这里再判一遍必然会与它漂移,而漂移的方向是界面
    // 提供一个判定层会拒绝的动作。
    if (verdict.state == ExtensionUpdateState::StagedUnreviewed) {
        plan.state = ExtensionUpdatePlanState::Stageable;
    } else {
        plan.state = ExtensionUpdatePlanState::Blocked;
        plan.errorCode = verdict.errorCode;
    }

    // 暂存不是启用。即使证据齐备且判定通过,当前版本仍然不被替换,候选仍然不可执行:候选按
    // 定义是另一份内容,必须重新经过人工复核并重新授权。
    plan.stagesOnly = true;
    plan.replacesActiveVersion = false;
    plan.grantsExecution = false;
    return plan;
}
