#ifndef EXTENSION_SCOPE_POLICY_H
#define EXTENSION_SCOPE_POLICY_H

#include "extension_registry.h"

#include <QList>
#include <QString>

// 作用域生效策略。一份启用授权回答的是"用户要让这份内容运行吗",但它不回答"在哪里
// 运行"。没有作用域模型时,任何一次启用都是全局启用:一个只为某个项目批准的 Skill 会
// 在打开另一个项目时继续激活,而一个被组织策略禁止的扩展会因为某个更低层级把它打开而
// 实际运行。这两件事都不是授权判定失误,而是缺少一个把"授权"与"适用范围"分开的层。
//
// 因此优先级模型必须显式且单向:Managed 策略最高,其后是 Global、Project、Session、
// ChildTask。更低层级永远不能推翻更高层级的拒绝——那正是"策略"这个词的含义。反过来,
// 更低层级的启用只在更高层级没有拒绝时生效。
//
// 冲突必须可解释。一个组件保持关闭时,人需要知道究竟是哪一个来源挡住了它,否则唯一的
// 补救办法是逐个层级试错,而在授权界面上试错等于反复请求批准。
enum class ExtensionScopeLevel {
    // 组织策略。它既能强制启用也能强制禁止,并且不可被用户覆盖。
    Managed,
    Global,
    Project,
    Session,
    // 子任务。父任务委派工作时,子任务只获得显式声明的子集。
    ChildTask,
};

enum class ExtensionScopeDisposition {
    // 该层级对这个扩展没有意见,判定继续向下。
    Unspecified,
    Enabled,
    Disabled,
};

// 单个层级对单个扩展的意见。它绑定确切内容摘要,与启用授权同构:按名字或标识绑定会让
// 一份被替换过的内容继承上一份内容在某个作用域里的启用。
struct ExtensionScopeRule {
    ExtensionScopeLevel level = ExtensionScopeLevel::ChildTask;
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    QString sourceIdentity;
    QString contentIdentity;
    ExtensionScopeDisposition disposition = ExtensionScopeDisposition::Unspecified;
    // Managed 层级独有:组织策略强制的结论不可被用户覆盖。其他层级设置它无效。
    bool mandatory = false;
};

// 一次判定所处的位置。项目标识为空表示当前没有打开项目,因此项目层级的规则不适用——
// 而不是适用于任意项目。
struct ExtensionScopeContext {
    QString projectIdentity;
    QString sessionIdentity;
    // 子任务只接收显式声明的子集。为空表示这次判定不在子任务里。
    QString childTaskIdentity;
    // 子任务被显式授予的扩展标识集合。子任务判定时,不在这个集合里的扩展不激活,
    // 即使父任务自己启用了它。
    QStringList childTaskDeclaredIds;
};

enum class ExtensionScopeOutcome {
    // 在这个位置激活。
    Active,
    // 在这个位置不激活,并且能指出是哪一个来源挡住的。
    Inactive,
    // 规则集本身无法作为判定依据。
    Undecidable,
};

struct ExtensionScopeDecision {
    ExtensionScopeOutcome outcome = ExtensionScopeOutcome::Inactive;
    bool active = false;
    // 决定这个结论的层级。被拒绝时它就是阻挡来源。
    ExtensionScopeLevel decidingLevel = ExtensionScopeLevel::Managed;
    // 该结论是否由组织策略强制,因此不可被用户覆盖。
    bool managedEnforced = false;
    // 固定诊断代码,用于向人解释为什么它没有激活。
    QString errorCode;
};

class ExtensionScopePolicy
{
public:
    static constexpr int MaxRules = 4096;

    // 判定单个扩展在给定位置是否激活。`grantEnabled` 是启用授权层已经得出的结论:
    // 这一层从不重新判定授权,也永不把一个未获授权的扩展变成激活。
    static ExtensionScopeDecision evaluate(
        const ExtensionRegistryRecord &record,
        bool grantEnabled,
        const QList<ExtensionScopeRule> &rules,
        const ExtensionScopeContext &context);

    // 层级优先级。数值越小优先级越高。
    static int precedence(ExtensionScopeLevel level);

    static QString levelLabel(ExtensionScopeLevel level);

    // 规则是否描述眼前这份确切内容。
    static bool appliesTo(const ExtensionScopeRule &rule,
                          const ExtensionRegistryRecord &record);
};

#endif // EXTENSION_SCOPE_POLICY_H
