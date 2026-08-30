#include "extension_scope_policy.h"

#include "extension_display_safety.h"

namespace {

using Safety = ExtensionDisplaySafety;

const QString &contentPrefix()
{
    static const QString value = QStringLiteral("extension-content:sha256:");
    return value;
}

const QString &sourcePrefix()
{
    static const QString value = QStringLiteral("extension-source:sha256:");
    return value;
}

ExtensionScopeDecision refuse(const QString &code, ExtensionScopeLevel level,
                              ExtensionScopeOutcome outcome)
{
    ExtensionScopeDecision decision;
    decision.outcome = outcome;
    // 拒绝路径上永不激活。这一点在每一条返回路径上成立,而不是只在成功路径上被设置。
    decision.active = false;
    decision.decidingLevel = level;
    decision.errorCode = code;
    return decision;
}

// 该层级在当前位置是否适用。为空的位置标识意味着这次判定不在那个位置里,因此对应层级
// 的规则不适用——而不是适用于任意位置。
bool levelApplicable(ExtensionScopeLevel level,
                     const ExtensionScopeContext &context)
{
    switch (level) {
    case ExtensionScopeLevel::Managed:
    case ExtensionScopeLevel::Global:
        return true;
    case ExtensionScopeLevel::Project:
        return !context.projectIdentity.isEmpty();
    case ExtensionScopeLevel::Session:
        return !context.sessionIdentity.isEmpty();
    case ExtensionScopeLevel::ChildTask:
        return !context.childTaskIdentity.isEmpty();
    }
    // 未分类层级不适用:一个还未被纳入优先级模型的层级不应默默参与判定。
    return false;
}

} // namespace

int ExtensionScopePolicy::precedence(ExtensionScopeLevel level)
{
    switch (level) {
    case ExtensionScopeLevel::Managed:
        return 0;
    case ExtensionScopeLevel::Global:
        return 1;
    case ExtensionScopeLevel::Project:
        return 2;
    case ExtensionScopeLevel::Session:
        return 3;
    case ExtensionScopeLevel::ChildTask:
        return 4;
    }
    // 未分类层级排在最低优先级:新增层级不得凭默认值获得推翻组织策略的能力。
    return 1000;
}

QString ExtensionScopePolicy::levelLabel(ExtensionScopeLevel level)
{
    switch (level) {
    case ExtensionScopeLevel::Managed:
        return QStringLiteral("组织策略");
    case ExtensionScopeLevel::Global:
        return QStringLiteral("全局");
    case ExtensionScopeLevel::Project:
        return QStringLiteral("项目");
    case ExtensionScopeLevel::Session:
        return QStringLiteral("会话");
    case ExtensionScopeLevel::ChildTask:
        return QStringLiteral("子任务");
    }
    return QStringLiteral("未知层级");
}

bool ExtensionScopePolicy::appliesTo(const ExtensionScopeRule &rule,
                                    const ExtensionRegistryRecord &record)
{
    // 规则绑定确切内容,与启用授权同构。按标识或名字绑定会让一份被替换过的内容继承
    // 上一份内容在某个作用域里的启用结论。
    return rule.kind == record.kind
        && !rule.id.isEmpty()
        && rule.id == record.id
        && !rule.contentIdentity.isEmpty()
        && rule.contentIdentity == record.contentIdentity
        && !rule.sourceIdentity.isEmpty()
        && rule.sourceIdentity == record.sourceIdentity;
}

ExtensionScopeDecision ExtensionScopePolicy::evaluate(
    const ExtensionRegistryRecord &record,
    bool grantEnabled,
    const QList<ExtensionScopeRule> &rules,
    const ExtensionScopeContext &context)
{
    if (!Safety::validId(record.id)) {
        return refuse(QStringLiteral("extension-scope-record-invalid"),
                      ExtensionScopeLevel::Managed,
                      ExtensionScopeOutcome::Undecidable);
    }
    if (!Safety::hashIdentity(record.contentIdentity, contentPrefix())
            || !Safety::hashIdentity(record.sourceIdentity, sourcePrefix())) {
        return refuse(QStringLiteral("extension-scope-identity-invalid"),
                      ExtensionScopeLevel::Managed,
                      ExtensionScopeOutcome::Undecidable);
    }
    if (rules.size() > MaxRules) {
        return refuse(QStringLiteral("extension-scope-rule-limit"),
                      ExtensionScopeLevel::Managed,
                      ExtensionScopeOutcome::Undecidable);
    }

    // 先收集适用于这份确切内容且其层级在当前位置有效的规则,并在此过程中拒绝无法判定的
    // 规则集。同一层级出现互相矛盾的意见时无从判断哪一条有效。
    QList<ExtensionScopeRule> applicable;
    for (const ExtensionScopeRule &rule : rules) {
        if (!appliesTo(rule, record)) continue;
        if (!levelApplicable(rule.level, context)) continue;
        for (const ExtensionScopeRule &seen : applicable) {
            if (seen.level == rule.level && seen.disposition != rule.disposition) {
                return refuse(QStringLiteral("extension-scope-rule-conflict"),
                              rule.level, ExtensionScopeOutcome::Undecidable);
            }
        }
        applicable.append(rule);
    }

    // Managed 强制结论先于一切被处理,包括先于启用授权:组织策略强制启用或禁止时,
    // 用户层级的意见不参与判定,这正是"不可被用户覆盖"的含义。
    for (const ExtensionScopeRule &rule : applicable) {
        if (rule.level != ExtensionScopeLevel::Managed || !rule.mandatory) continue;
        if (rule.disposition == ExtensionScopeDisposition::Disabled) {
            ExtensionScopeDecision decision =
                refuse(QStringLiteral("extension-scope-managed-blocked"),
                       ExtensionScopeLevel::Managed, ExtensionScopeOutcome::Inactive);
            decision.managedEnforced = true;
            return decision;
        }
        if (rule.disposition == ExtensionScopeDisposition::Enabled) {
            // 组织策略强制启用仍然不能绕过注册表的双重门禁:未复核或不兼容的内容不会
            // 因为策略强制而运行。策略能强制"允许",不能替人完成复核。
            if (record.trust != ExtensionTrustState::Verified
                    || record.compatibility != ExtensionCompatibilityState::Compatible) {
                ExtensionScopeDecision decision =
                    refuse(QStringLiteral("extension-scope-managed-ungated"),
                           ExtensionScopeLevel::Managed,
                           ExtensionScopeOutcome::Inactive);
                decision.managedEnforced = true;
                return decision;
            }
            ExtensionScopeDecision decision;
            decision.outcome = ExtensionScopeOutcome::Active;
            decision.active = true;
            decision.decidingLevel = ExtensionScopeLevel::Managed;
            decision.managedEnforced = true;
            return decision;
        }
    }

    // 这一层从不重新判定授权,也永不把一个未获授权的扩展变成激活。作用域只能收窄一份
    // 已有授权的适用范围,不能创造授权。
    if (!grantEnabled) {
        return refuse(QStringLiteral("extension-scope-grant-absent"),
                      ExtensionScopeLevel::Global, ExtensionScopeOutcome::Inactive);
    }

    // 子任务只接收显式声明的子集:父任务的启用不会顺带传给子任务。
    if (!context.childTaskIdentity.isEmpty()
            && !context.childTaskDeclaredIds.contains(record.id)) {
        return refuse(QStringLiteral("extension-scope-child-task-undeclared"),
                      ExtensionScopeLevel::ChildTask, ExtensionScopeOutcome::Inactive);
    }

    // 优先级是**有方向的**。更低层级的拒绝始终生效,因为收窄权限永远是安全的方向;
    // 更低层级的启用则不能推翻更高层级的拒绝,那正是"策略"的含义。把两个方向同等对待
    // 会让一个显式声明"不要这个扩展"的子任务被更高层级强行塞进来,而这与最小权限相反。
    // 因此:任何适用层级的拒绝都让它不激活,并归因到其中优先级最高的那个层级——那是
    // 最强的阻挡权威,也是人首先需要知道的来源。
    bool denied = false;
    ExtensionScopeLevel denyingLevel = ExtensionScopeLevel::ChildTask;
    for (const ExtensionScopeRule &rule : applicable) {
        if (rule.disposition != ExtensionScopeDisposition::Disabled) continue;
        if (!denied || precedence(rule.level) < precedence(denyingLevel)) {
            denyingLevel = rule.level;
        }
        denied = true;
    }
    if (denied) {
        return refuse(QStringLiteral("extension-scope-disabled-at-level"),
                      denyingLevel, ExtensionScopeOutcome::Inactive);
    }

    // 没有任何拒绝时,由优先级最高的启用层级决定,并把它记为结论来源。
    for (int rank = 0; rank <= precedence(ExtensionScopeLevel::ChildTask); ++rank) {
        for (const ExtensionScopeRule &rule : applicable) {
            if (precedence(rule.level) != rank) continue;
            if (rule.disposition != ExtensionScopeDisposition::Enabled) continue;
            ExtensionScopeDecision decision;
            decision.outcome = ExtensionScopeOutcome::Active;
            decision.active = true;
            decision.decidingLevel = rule.level;
            return decision;
        }
    }

    // 没有任何适用层级表达意见时不激活。一份授权在没有作用域声明的位置默认不适用,
    // 否则"为某个项目启用"会等于"到处启用"。
    return refuse(QStringLiteral("extension-scope-unscoped"),
                  ExtensionScopeLevel::Global, ExtensionScopeOutcome::Inactive);
}
