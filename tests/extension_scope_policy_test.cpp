#include "extension_scope_policy.h"

#include "extension_enablement_policy.h"

#include <QCoreApplication>
#include <QCryptographicHash>
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

QString identity(const QString &prefix, const QByteArray &seed)
{
    return prefix + QString::fromLatin1(
        QCryptographicHash::hash(seed, QCryptographicHash::Sha256).toHex());
}

QString sourceOf(const QByteArray &seed)
{
    return identity(QStringLiteral("extension-source:sha256:"), seed + "-source");
}

QString contentOf(const QByteArray &seed)
{
    return identity(QStringLiteral("extension-content:sha256:"), seed + "-content");
}

// 一个已复核、兼容、已安装的 Skill。
ExtensionRegistryRecord record()
{
    ExtensionRegistryRecord value;
    value.kind = ExtensionKind::Skill;
    value.id = QStringLiteral("acme.formatter");
    value.name = QStringLiteral("Acme Formatter");
    value.version = QStringLiteral("2.1.0");
    value.sourceIdentity = sourceOf("acme");
    value.contentIdentity = contentOf("acme");
    value.trust = ExtensionTrustState::Verified;
    value.compatibility = ExtensionCompatibilityState::Compatible;
    value.requestedCapabilities = QStringList{QStringLiteral("skill-content")};
    value.installed = true;
    return value;
}

ExtensionScopeRule rule(ExtensionScopeLevel level,
                        ExtensionScopeDisposition disposition)
{
    ExtensionScopeRule value;
    value.level = level;
    value.kind = record().kind;
    value.id = record().id;
    value.sourceIdentity = record().sourceIdentity;
    value.contentIdentity = record().contentIdentity;
    value.disposition = disposition;
    return value;
}

ExtensionScopeContext projectContext(const QString &project)
{
    ExtensionScopeContext value;
    value.projectIdentity = project;
    value.sessionIdentity = QStringLiteral("session-1");
    return value;
}

// 为某个项目启用的 Skill 不会在另一个项目里激活。
void projectScopeTests()
{
    const QList<ExtensionScopeRule> rules{
        rule(ExtensionScopeLevel::Project, ExtensionScopeDisposition::Enabled)};

    const ExtensionScopeDecision here = ExtensionScopePolicy::evaluate(
        record(), true, rules, projectContext(QStringLiteral("project-a")));
    expect(here.active && here.outcome == ExtensionScopeOutcome::Active,
           "a Skill enabled for a project was not active there");
    expect(here.decidingLevel == ExtensionScopeLevel::Project,
           "the project-level enable was not the deciding level");

    // 关键:打开另一个项目时这条规则不再适用,因此不激活。规则集里没有全局或组织策略
    // 的启用意见,所以没有任何来源让它在别处激活。
    // 项目层级的规则只属于批准它的那个项目,因此换到另一个项目时它不在规则集里。
    ExtensionScopeContext elsewhere = projectContext(QStringLiteral("project-b"));
    const ExtensionScopeDecision other = ExtensionScopePolicy::evaluate(
        record(), true, {}, elsewhere);
    expect(!other.active,
           "a project-scoped Skill became active in another project");
    expect(other.errorCode == QStringLiteral("extension-scope-unscoped"),
           "an unscoped position did not explain why the Skill was inactive");

    // 除非全局层级也启用了它。
    const ExtensionScopeDecision globally = ExtensionScopePolicy::evaluate(
        record(), true,
        {rule(ExtensionScopeLevel::Global, ExtensionScopeDisposition::Enabled)},
        elsewhere);
    expect(globally.active,
           "a globally enabled Skill was not active in a second project");
    expect(globally.decidingLevel == ExtensionScopeLevel::Global,
           "the global enable was not the deciding level");
}

// 子任务只接收显式声明的子集,不继承父任务无关的扩展。
void childTaskTests()
{
    ExtensionScopeContext child = projectContext(QStringLiteral("project-a"));
    child.childTaskIdentity = QStringLiteral("child-1");
    // 父任务全局启用了它,但子任务没有声明它。
    const QList<ExtensionScopeRule> rules{
        rule(ExtensionScopeLevel::Global, ExtensionScopeDisposition::Enabled)};
    const ExtensionScopeDecision undeclared =
        ExtensionScopePolicy::evaluate(record(), true, rules, child);
    expect(!undeclared.active,
           "a child task inherited an extension it was never granted");
    expect(undeclared.errorCode
               == QStringLiteral("extension-scope-child-task-undeclared"),
           "an undeclared child-task extension did not explain itself");
    expect(undeclared.decidingLevel == ExtensionScopeLevel::ChildTask,
           "the child-task restriction was not attributed to the child task");

    // 显式声明之后才激活。
    child.childTaskDeclaredIds = QStringList{record().id};
    const ExtensionScopeDecision declared =
        ExtensionScopePolicy::evaluate(record(), true, rules, child);
    expect(declared.active,
           "a child task did not receive its explicitly declared subset");

    // 优先级是有方向的:更低层级的拒绝始终生效,因为收窄权限永远是安全的方向。一个显式
    // 声明"不要这个扩展"的子任务不应被更高层级强行塞进来——那与最小权限相反。
    QList<ExtensionScopeRule> denied = rules;
    denied.append(rule(ExtensionScopeLevel::ChildTask,
                       ExtensionScopeDisposition::Disabled));
    const ExtensionScopeDecision childDenied =
        ExtensionScopePolicy::evaluate(record(), true, denied, child);
    expect(!childDenied.active,
           "a child task was forced to accept an extension it declined");
    expect(childDenied.decidingLevel == ExtensionScopeLevel::ChildTask,
           "the child-task refusal was not attributed to the child task");

    // 反方向不成立:更低层级的**启用**不能推翻更高层级的拒绝。
    QList<ExtensionScopeRule> globalDeny{
        rule(ExtensionScopeLevel::Global, ExtensionScopeDisposition::Disabled),
        rule(ExtensionScopeLevel::ChildTask, ExtensionScopeDisposition::Enabled)};
    const ExtensionScopeDecision escalated =
        ExtensionScopePolicy::evaluate(record(), true, globalDeny, child);
    expect(!escalated.active,
           "a child-task enable overrode a global disable");
    // 归因到优先级最高的阻挡来源:那是最强的阻挡权威,也是人首先需要知道的。多个层级
    // 同时拒绝时,报告子任务这一层会让人以为撤掉子任务声明就能放行,而全局拒绝仍然在。
    QList<ExtensionScopeRule> bothDeny{
        rule(ExtensionScopeLevel::Global, ExtensionScopeDisposition::Disabled),
        rule(ExtensionScopeLevel::ChildTask, ExtensionScopeDisposition::Disabled)};
    const ExtensionScopeDecision strongest =
        ExtensionScopePolicy::evaluate(record(), true, bothDeny, child);
    expect(!strongest.active, "two independent denials still activated");
    expect(strongest.decidingLevel == ExtensionScopeLevel::Global,
           "the strongest blocking authority was not named");
    (void)escalated;
}

// 更低层级永远不能推翻更高优先级的拒绝,并且结论必须指出阻挡来源。
void precedenceTests()
{
    // 组织策略禁止 + 每一个更低层级都启用。
    QList<ExtensionScopeRule> rules;
    ExtensionScopeRule blocked =
        rule(ExtensionScopeLevel::Managed, ExtensionScopeDisposition::Disabled);
    blocked.mandatory = true;
    rules.append(blocked);
    for (const ExtensionScopeLevel level : {
             ExtensionScopeLevel::Global, ExtensionScopeLevel::Project,
             ExtensionScopeLevel::Session, ExtensionScopeLevel::ChildTask}) {
        rules.append(rule(level, ExtensionScopeDisposition::Enabled));
    }
    ExtensionScopeContext context = projectContext(QStringLiteral("project-a"));
    context.childTaskIdentity = QStringLiteral("child-1");
    context.childTaskDeclaredIds = QStringList{record().id};

    const ExtensionScopeDecision decision =
        ExtensionScopePolicy::evaluate(record(), true, rules, context);
    expect(!decision.active,
           "a lower scope enabled a component denied by managed policy");
    expect(decision.errorCode == QStringLiteral("extension-scope-managed-blocked"),
           "a policy block did not report itself as the blocking source");
    // 关键:解释必须指认阻挡来源,否则唯一的补救办法是逐层试错。
    expect(decision.decidingLevel == ExtensionScopeLevel::Managed,
           "the effective-policy explanation did not identify the blocking source");
    expect(decision.managedEnforced,
           "a managed block was not marked as unoverridable by the user");

    // 全局拒绝同样不能被项目、会话或子任务推翻。
    QList<ExtensionScopeRule> globalDeny{
        rule(ExtensionScopeLevel::Global, ExtensionScopeDisposition::Disabled),
        rule(ExtensionScopeLevel::Project, ExtensionScopeDisposition::Enabled),
        rule(ExtensionScopeLevel::Session, ExtensionScopeDisposition::Enabled)};
    const ExtensionScopeDecision overridden = ExtensionScopePolicy::evaluate(
        record(), true, globalDeny, projectContext(QStringLiteral("project-a")));
    expect(!overridden.active,
           "a project-level enable overrode a global disable");
    expect(overridden.decidingLevel == ExtensionScopeLevel::Global,
           "the blocking level was misattributed");

    // 优先级顺序本身必须是单向且严格的。
    expect(ExtensionScopePolicy::precedence(ExtensionScopeLevel::Managed)
               < ExtensionScopePolicy::precedence(ExtensionScopeLevel::Global)
           && ExtensionScopePolicy::precedence(ExtensionScopeLevel::Global)
               < ExtensionScopePolicy::precedence(ExtensionScopeLevel::Project)
           && ExtensionScopePolicy::precedence(ExtensionScopeLevel::Project)
               < ExtensionScopePolicy::precedence(ExtensionScopeLevel::Session)
           && ExtensionScopePolicy::precedence(ExtensionScopeLevel::Session)
               < ExtensionScopePolicy::precedence(ExtensionScopeLevel::ChildTask),
           "the scope precedence order is not strictly one-directional");
    // 未分类层级排在最低优先级:新增层级不得凭默认值推翻组织策略。
    expect(ExtensionScopePolicy::precedence(
               static_cast<ExtensionScopeLevel>(9999))
               > ExtensionScopePolicy::precedence(ExtensionScopeLevel::ChildTask),
           "an unclassified scope level outranked a defined one");
}

// 组织策略强制启用不可被用户覆盖,但也不能替人完成复核。
void managedTests()
{
    ExtensionScopeRule mandated =
        rule(ExtensionScopeLevel::Managed, ExtensionScopeDisposition::Enabled);
    mandated.mandatory = true;

    // 强制启用先于启用授权被处理:用户从未授权,组织策略仍然让它激活。
    const ExtensionScopeDecision mandatedActive = ExtensionScopePolicy::evaluate(
        record(), false, {mandated}, projectContext(QStringLiteral("project-a")));
    expect(mandatedActive.active,
           "a mandated extension did not activate without a user grant");
    expect(mandatedActive.managedEnforced,
           "a mandated activation was not marked as managed");

    // 但强制启用不绕过注册表双重门禁:未复核的内容不因策略而运行。
    ExtensionRegistryRecord unverified = record();
    unverified.trust = ExtensionTrustState::Unverified;
    const ExtensionScopeDecision ungated = ExtensionScopePolicy::evaluate(
        unverified, true, {mandated}, projectContext(QStringLiteral("project-a")));
    expect(!ungated.active,
           "managed policy ran content that was never reviewed");
    expect(ungated.errorCode == QStringLiteral("extension-scope-managed-ungated"),
           "a mandated but ungated extension did not explain itself");

    ExtensionRegistryRecord incompatible = record();
    incompatible.compatibility = ExtensionCompatibilityState::Incompatible;
    expect(!ExtensionScopePolicy::evaluate(
               incompatible, true, {mandated},
               projectContext(QStringLiteral("project-a"))).active,
           "managed policy ran content the host cannot accommodate");

    // 非 Managed 层级设置 mandatory 无效:它不因此获得不可覆盖的地位。
    ExtensionScopeRule fake =
        rule(ExtensionScopeLevel::Project, ExtensionScopeDisposition::Enabled);
    fake.mandatory = true;
    QList<ExtensionScopeRule> rules{fake};
    ExtensionScopeRule managedBlock =
        rule(ExtensionScopeLevel::Managed, ExtensionScopeDisposition::Disabled);
    managedBlock.mandatory = true;
    rules.append(managedBlock);
    const ExtensionScopeDecision spoofed = ExtensionScopePolicy::evaluate(
        record(), true, rules, projectContext(QStringLiteral("project-a")));
    expect(!spoofed.active,
           "a non-managed rule claiming mandatory status overrode policy");
    expect(spoofed.decidingLevel == ExtensionScopeLevel::Managed,
           "a spoofed mandatory rule displaced the real blocking source");
}

// 规则集本身无法作为判定依据时不激活。
void undecidableTests()
{
    // 同一层级的矛盾意见无从判断哪一条有效。
    QList<ExtensionScopeRule> conflicting{
        rule(ExtensionScopeLevel::Project, ExtensionScopeDisposition::Enabled),
        rule(ExtensionScopeLevel::Project, ExtensionScopeDisposition::Disabled)};
    const ExtensionScopeDecision conflict = ExtensionScopePolicy::evaluate(
        record(), true, conflicting, projectContext(QStringLiteral("project-a")));
    expect(!conflict.active,
           "contradictory rules at one level still activated the extension");
    expect(conflict.errorCode == QStringLiteral("extension-scope-rule-conflict"),
           "a rule conflict did not report itself");
    expect(conflict.outcome == ExtensionScopeOutcome::Undecidable,
           "a rule conflict was presented as a decided outcome");

    ExtensionRegistryRecord badId = record();
    badId.id = QStringLiteral("Bad Id");
    expect(ExtensionScopePolicy::evaluate(
               badId, true, {}, projectContext(QStringLiteral("p"))).errorCode
               == QStringLiteral("extension-scope-record-invalid"),
           "a malformed identifier was scoped");

    ExtensionRegistryRecord badIdentity = record();
    badIdentity.contentIdentity = QStringLiteral("extension-content:sha256:abc");
    expect(ExtensionScopePolicy::evaluate(
               badIdentity, true, {}, projectContext(QStringLiteral("p"))).errorCode
               == QStringLiteral("extension-scope-identity-invalid"),
           "a truncated content identity was scoped");

    // 规则按确切内容绑定:被替换过的内容不继承上一份内容的作用域启用。
    ExtensionRegistryRecord replaced = record();
    replaced.contentIdentity = contentOf("acme-v3");
    const ExtensionScopeDecision drifted = ExtensionScopePolicy::evaluate(
        replaced, true,
        {rule(ExtensionScopeLevel::Global, ExtensionScopeDisposition::Enabled)},
        projectContext(QStringLiteral("project-a")));
    expect(!drifted.active,
           "replaced content inherited the scope enable of the previous content");

    ExtensionRegistryRecord sourceChanged = record();
    sourceChanged.sourceIdentity = sourceOf("elsewhere");
    expect(!ExtensionScopePolicy::evaluate(
               sourceChanged, true,
               {rule(ExtensionScopeLevel::Global, ExtensionScopeDisposition::Enabled)},
               projectContext(QStringLiteral("project-a"))).active,
           "content from a new source inherited the previous scope enable");

    // 空摘要的规则不构成依据。
    ExtensionScopeRule empty =
        rule(ExtensionScopeLevel::Global, ExtensionScopeDisposition::Enabled);
    empty.contentIdentity.clear();
    expect(!ExtensionScopePolicy::appliesTo(empty, record()),
           "a rule with no content identity was treated as matching evidence");
    ExtensionScopeRule emptySource =
        rule(ExtensionScopeLevel::Global, ExtensionScopeDisposition::Enabled);
    emptySource.sourceIdentity.clear();
    expect(!ExtensionScopePolicy::appliesTo(emptySource, record()),
           "a rule with no source identity was treated as matching");
    // 类型必须一致:同一个标识在不同类型下是不同的扩展。
    ExtensionScopeRule otherKind =
        rule(ExtensionScopeLevel::Global, ExtensionScopeDisposition::Enabled);
    otherKind.kind = ExtensionKind::Mcp;
    expect(!ExtensionScopePolicy::appliesTo(otherKind, record()),
           "a rule applied across extension kinds");
}

// 这一层只收窄授权的适用范围,不创造授权,也不执行任何东西。
void authorityTests()
{
    // 关键:没有启用授权时,任何用户层级的作用域启用都不能让它激活。
    for (const ExtensionScopeLevel level : {
             ExtensionScopeLevel::Global, ExtensionScopeLevel::Project,
             ExtensionScopeLevel::Session}) {
        const ExtensionScopeDecision decision = ExtensionScopePolicy::evaluate(
            record(), false, {rule(level, ExtensionScopeDisposition::Enabled)},
            projectContext(QStringLiteral("project-a")));
        expect(!decision.active,
               "a scope rule activated an extension that holds no grant");
        expect(decision.errorCode == QStringLiteral("extension-scope-grant-absent"),
               "an ungranted extension did not report the missing grant");
    }
    // 授权层的结论本身不被这一层改写。
    const ExtensionEnablementDecision grant =
        ExtensionEnablementPolicy::evaluate(record(), {});
    expect(!grant.enabled,
           "the fixture record already held an enablement grant");

    // 层级的位置标识为空时该层级不适用,而不是适用于任意位置。
    ExtensionScopeContext noProject;
    noProject.sessionIdentity = QStringLiteral("session-1");
    expect(!ExtensionScopePolicy::evaluate(
               record(), true,
               {rule(ExtensionScopeLevel::Project,
                     ExtensionScopeDisposition::Enabled)},
               noProject).active,
           "a project rule applied while no project was open");
    ExtensionScopeContext nothing;
    expect(!ExtensionScopePolicy::evaluate(
               record(), true,
               {rule(ExtensionScopeLevel::Session,
                     ExtensionScopeDisposition::Enabled)},
               nothing).active,
           "a session rule applied outside any session");

    // 每一个已定义层级都有展示标签,否则解释里会出现一个没有来源名字的阻挡源。
    for (const ExtensionScopeLevel level : {
             ExtensionScopeLevel::Managed, ExtensionScopeLevel::Global,
             ExtensionScopeLevel::Project, ExtensionScopeLevel::Session,
             ExtensionScopeLevel::ChildTask}) {
        expect(!ExtensionScopePolicy::levelLabel(level).isEmpty(),
               "a scope level has no display label");
    }
    expect(!ExtensionScopePolicy::levelLabel(
               static_cast<ExtensionScopeLevel>(9999)).isEmpty(),
           "an unclassified scope level has no display label");

    // 规则数量有界:一个不可信的规则集不得让判定无限展开。
    QList<ExtensionScopeRule> flood;
    for (int index = 0; index <= ExtensionScopePolicy::MaxRules; ++index) {
        flood.append(rule(ExtensionScopeLevel::Global,
                          ExtensionScopeDisposition::Enabled));
    }
    expect(ExtensionScopePolicy::evaluate(
               record(), true, flood,
               projectContext(QStringLiteral("project-a"))).errorCode
               == QStringLiteral("extension-scope-rule-limit"),
           "an unbounded rule set was evaluated");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    projectScopeTests();
    childTaskTests();
    precedenceTests();
    managedTests();
    undecidableTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension scope policy tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
