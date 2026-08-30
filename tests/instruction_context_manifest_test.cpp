#include "instruction_context_manifest.h"

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

QString contentOf(const QByteArray &seed)
{
    return QStringLiteral("extension-content:sha256:")
        + QString::fromLatin1(QCryptographicHash::hash(
            seed + "-content", QCryptographicHash::Sha256).toHex());
}

InstructionSource source(InstructionSourceKind kind, const QString &path,
                         int depth = 0, const QStringList &behaviors = {})
{
    InstructionSource value;
    value.kind = kind;
    value.sourcePath = path;
    value.contentIdentity = contentOf(path.toUtf8());
    value.directoryDepth = depth;
    value.requestedBehaviors = behaviors;
    value.contextBytes = 1024;
    return value;
}

SkillInvocationRecord invocation(const QStringList &permissions = {})
{
    SkillInvocationRecord value;
    value.id = QStringLiteral("acme.formatter");
    value.version = QStringLiteral("2.1.0");
    value.sourcePath = QStringLiteral("/skills/acme.formatter/SKILL.md");
    value.contentIdentity = contentOf("acme-skill");
    value.includedReferences = QStringList{QStringLiteral("reference.md")};
    value.scriptPermissions = permissions;
    return value;
}

// 更靠近目标文件的嵌套指令覆盖更外层的,而覆盖关系必须是清单里的一条记录。
void nestedPrecedenceTests()
{
    const QList<InstructionSource> sources{
        source(InstructionSourceKind::ProjectRoot,
               QStringLiteral("/repo/AGENTS.md")),
        source(InstructionSourceKind::ProjectNested,
               QStringLiteral("/repo/src/AGENTS.md"), 2),
        source(InstructionSourceKind::ProjectNested,
               QStringLiteral("/repo/src/deep/AGENTS.md"), 4),
        source(InstructionSourceKind::UserGlobal,
               QStringLiteral("/home/user/AGENTS.md")),
    };
    const InstructionContextManifest manifest =
        InstructionContextPolicy::build(sources, {});
    expect(manifest.state == InstructionManifestState::Ready,
           "a readable instruction chain was not usable");
    // 关键:整条适用链都在清单里,一条不少。少记录一条会让排查只能靠猜。
    expect(manifest.chain.size() == sources.size(),
           "the manifest did not include the whole applicable chain");

    // 优先级顺序:全局用户指令 → 项目根 → 较浅嵌套 → 较深嵌套。
    if (!expect(manifest.chain.size() == 4,
                "the nested chain was not fully recorded")) return;
    expect(manifest.chain.at(0).sourcePath
               == QStringLiteral("/home/user/AGENTS.md"),
           "the global instruction did not come first in precedence");
    expect(manifest.chain.at(1).sourcePath == QStringLiteral("/repo/AGENTS.md"),
           "the project root instruction was misordered");
    expect(manifest.chain.at(2).sourcePath
               == QStringLiteral("/repo/src/deep/AGENTS.md"),
           "the closer nested instruction did not take precedence");
    expect(manifest.chain.at(3).sourcePath
               == QStringLiteral("/repo/src/AGENTS.md"),
           "the shallower nested instruction was not overridden");

    if (manifest.chain.size() != sources.size()) return;
    // 每条记录都带出处、内容摘要与优先级说明,否则无法追溯。
    for (const InstructionManifestEntry &entry : manifest.chain) {
        expect(!entry.sourcePath.isEmpty() && !entry.contentFingerprint.isEmpty()
                   && !entry.precedenceLabel.isEmpty(),
               "a manifest entry is not traceable to its origin");
    }
    // 上下文字节被核算。
    expect(manifest.totalContextBytes == 4 * 1024,
           "the context size accounting did not match the sources");
    expect(!manifest.grantsExecution, "building a manifest granted execution");
}

// 一次 Skill 调用必须记录身份、版本/摘要、出处、引用与权限。
void skillInvocationTests()
{
    const InstructionContextManifest manifest =
        InstructionContextPolicy::build(
            {source(InstructionSourceKind::Skill,
                    QStringLiteral("/skills/acme.formatter/SKILL.md"))},
            {invocation({QStringLiteral("read-files")})});
    expect(manifest.state == InstructionManifestState::Ready,
           "a Skill invocation was not recordable");
    if (!expect(manifest.skillInvocations.size() == 1,
                "the turn did not record the Skill invocation")) return;
    const SkillInvocationRecord &record = manifest.skillInvocations.at(0);
    expect(record.id == QStringLiteral("acme.formatter")
               && record.version == QStringLiteral("2.1.0")
               && !record.contentIdentity.isEmpty()
               && !record.sourcePath.isEmpty(),
           "the Skill invocation record cannot identify what ran");
    expect(record.includedReferences
               == QStringList{QStringLiteral("reference.md")},
           "the included references were not recorded");
    expect(record.scriptPermissions
               == QStringList{QStringLiteral("read-files")},
           "the declared script permissions were not recorded");
    expect(record.deniedPermissions.isEmpty(),
           "a read-only permission was denied");

    // 越出只读边界的脚本权限被记录并拒绝,而不是静默采纳。
    const InstructionContextManifest write =
        InstructionContextPolicy::build(
            {}, {invocation({QStringLiteral("read-files"),
                             QStringLiteral("filesystem-write")})});
    if (expect(!write.skillInvocations.isEmpty(),
               "a Skill requesting a write permission was not recorded")) {
        const SkillInvocationRecord &denied = write.skillInvocations.at(0);
        expect(denied.deniedPermissions
                   == QStringList{QStringLiteral("filesystem-write")},
               "a write permission was not denied");
        // 关键:被拒绝的权限仍然留在声明里。失败关闭不等于把证据一起丢掉。
        expect(denied.scriptPermissions.contains(
                   QStringLiteral("filesystem-write")),
               "denying a permission discarded the evidence that it was requested");
    }
    expect(!write.grantsExecution, "recording a Skill granted execution");
}

// 运行时策略始终胜出,拒绝可见,且被拒绝的指令不被改写成一条已授权的策略。
void runtimePolicyWinsTests()
{
    const InstructionContextManifest manifest =
        InstructionContextPolicy::build(
            {source(InstructionSourceKind::ProjectRoot,
                    QStringLiteral("/repo/AGENTS.md"), 0,
                    {QStringLiteral("read-files"),
                     QStringLiteral("command-execution")})},
            {});
    expect(manifest.state == InstructionManifestState::Ready,
           "a chain containing a forbidden request was unusable");
    // 拒绝必须可见。守卫被破坏时断言必须报告失败而不是崩溃:崩溃会让后面的断言全部
    // 不再运行,于是一次破坏只暴露一个问题。
    if (expect(manifest.denials.size() == 1,
               "a forbidden instruction was not denied visibly")) {
        expect(manifest.denials.at(0).behavior
                   == QStringLiteral("command-execution"),
               "the denial did not name the forbidden behavior");
        expect(manifest.denials.at(0).reason
                   == InstructionDenialReason::ForbiddenByRuntimePolicy,
               "the denial did not attribute itself to runtime policy");
        expect(manifest.denials.at(0).sourcePath
                   == QStringLiteral("/repo/AGENTS.md"),
               "the denial did not name the instruction it came from");
    }
    // 指令本身仍然留在链上,未被改写成一条策略。
    if (expect(manifest.chain.size() == 1,
               "denying a behavior removed the instruction from the manifest")) {
        // 关键:被拒绝的行为不出现在被采纳的行为里。
        expect(manifest.chain.at(0).acceptedBehaviors
                   == QStringList{QStringLiteral("read-files")},
               "a forbidden behavior was accepted alongside the permitted one");
        expect(!manifest.chain.at(0).policyAuthority,
               "a project instruction was rewritten as trusted policy");
    }

    // 磁盘上的文本不是策略:非 Managed 来源表达策略时被拒绝。
    const InstructionContextManifest claimed =
        InstructionContextPolicy::build(
            {source(InstructionSourceKind::ProjectRoot,
                    QStringLiteral("/repo/AGENTS.md"), 0,
                    {QStringLiteral("always-allow-tools")})},
            {});
    if (expect(claimed.denials.size() == 1,
               "a project instruction was accepted as a policy statement")) {
        expect(claimed.denials.at(0).reason
                   == InstructionDenialReason::NotPolicyAuthority,
               "a policy claim was not attributed to missing policy authority");
        expect(claimed.denials.at(0).errorCode
                   == QStringLiteral("instruction-not-policy-authority"),
               "a policy claim denial did not report why");
    }
    if (expect(!claimed.chain.isEmpty(),
               "a denied policy claim removed the instruction from the chain")) {
        expect(claimed.chain.at(0).acceptedBehaviors.isEmpty(),
               "a policy claim from an untrusted source was accepted");
    }

    // Skill 内容同样不是策略。
    const InstructionContextManifest bySkill =
        InstructionContextPolicy::build(
            {source(InstructionSourceKind::Skill,
                    QStringLiteral("/skills/x/SKILL.md"), 0,
                    {QStringLiteral("bypass-approval")})},
            {});
    expect(bySkill.denials.size() == 1,
           "a Skill was allowed to declare policy");

    // 只有 Managed 来源可以表达策略。
    expect(InstructionContextPolicy::policyAuthority(
               InstructionSourceKind::Managed),
           "managed instructions cannot express policy");
    for (const InstructionSourceKind kind : {
             InstructionSourceKind::UserGlobal,
             InstructionSourceKind::ProjectRoot,
             InstructionSourceKind::ProjectNested,
             InstructionSourceKind::Skill}) {
        expect(!InstructionContextPolicy::policyAuthority(kind),
               "a disk instruction source was treated as policy authority");
    }
    // 未分类来源没有策略权威。
    expect(!InstructionContextPolicy::policyAuthority(
               static_cast<InstructionSourceKind>(9999)),
           "an unclassified instruction source claimed policy authority");
}

// 来源集合本身无法作为依据时不产出清单。
void unusableTests()
{
    InstructionSource noPath = source(InstructionSourceKind::ProjectRoot,
                                      QStringLiteral("/repo/AGENTS.md"));
    noPath.sourcePath.clear();
    expect(InstructionContextPolicy::build({noPath}, {}).errorCode
               == QStringLiteral("instruction-source-path-unsafe"),
           "an instruction with no origin entered the manifest");

    InstructionSource spoofed = source(InstructionSourceKind::ProjectRoot,
                                       QStringLiteral("/repo/AGENTS.md"));
    spoofed.sourcePath = QStringLiteral("/repo/A‮GENTS.md");
    expect(InstructionContextPolicy::build({spoofed}, {}).state
               == InstructionManifestState::Unusable,
           "a bidirectional override in a source path was displayed");

    // 同一路径出现两次时无法判断哪一份内容生效。
    const InstructionSource duplicated =
        source(InstructionSourceKind::ProjectRoot,
               QStringLiteral("/repo/AGENTS.md"));
    expect(InstructionContextPolicy::build({duplicated, duplicated}, {}).errorCode
               == QStringLiteral("instruction-source-duplicate"),
           "a duplicated instruction path was accepted");

    InstructionSource badIdentity = source(InstructionSourceKind::ProjectRoot,
                                           QStringLiteral("/repo/AGENTS.md"));
    badIdentity.contentIdentity = QStringLiteral("extension-content:sha256:abc");
    expect(InstructionContextPolicy::build({badIdentity}, {}).errorCode
               == QStringLiteral("instruction-content-identity-invalid"),
           "a truncated instruction content identity was accepted");

    InstructionSource badDepth = source(InstructionSourceKind::ProjectNested,
                                        QStringLiteral("/repo/src/AGENTS.md"));
    badDepth.directoryDepth = -1;
    expect(InstructionContextPolicy::build({badDepth}, {}).errorCode
               == QStringLiteral("instruction-depth-invalid"),
           "a negative directory depth was accepted");

    // 上下文预算被核算而不是被截断:悄悄丢掉一段指令会让清单与模型看到的内容不一致。
    InstructionSource huge = source(InstructionSourceKind::ProjectRoot,
                                    QStringLiteral("/repo/AGENTS.md"));
    huge.contextBytes = InstructionContextPolicy::MaxContextBytes + 1;
    expect(InstructionContextPolicy::build({huge}, {}).errorCode
               == QStringLiteral("instruction-context-budget-exceeded"),
           "an over-budget context was silently truncated");

    QList<InstructionSource> flood;
    for (int index = 0; index <= InstructionContextPolicy::MaxSources; ++index) {
        flood.append(source(InstructionSourceKind::ProjectNested,
                            QStringLiteral("/repo/n%1/AGENTS.md").arg(index)));
    }
    expect(InstructionContextPolicy::build(flood, {}).errorCode
               == QStringLiteral("instruction-source-limit"),
           "an unbounded source set was evaluated");

    // 无法使用时不得泄露任何条目。
    const InstructionContextManifest rejected =
        InstructionContextPolicy::build({noPath}, {});
    expect(rejected.chain.isEmpty() && rejected.denials.isEmpty()
               && !rejected.grantsExecution,
           "an unusable source set still produced manifest entries");

    // Skill 调用记录同样必须可展示、可识别。
    SkillInvocationRecord badId = invocation();
    badId.id = QStringLiteral("Bad Id");
    expect(InstructionContextPolicy::build({}, {badId}).errorCode
               == QStringLiteral("instruction-skill-id-invalid"),
           "a malformed Skill identifier was recorded");
    SkillInvocationRecord badSkillIdentity = invocation();
    badSkillIdentity.contentIdentity = QStringLiteral("nope");
    expect(InstructionContextPolicy::build({}, {badSkillIdentity}).errorCode
               == QStringLiteral("instruction-skill-identity-invalid"),
           "a Skill with no content identity was recorded");
    SkillInvocationRecord noSkillPath = invocation();
    noSkillPath.sourcePath.clear();
    expect(InstructionContextPolicy::build({}, {noSkillPath}).errorCode
               == QStringLiteral("instruction-skill-path-unsafe"),
           "a Skill with no source path was recorded");
}

// 这一层不加载文件、不执行任何东西、不改写策略。
void authorityTests()
{
    for (const InstructionContextManifest &manifest : {
             InstructionContextPolicy::build({}, {}),
             InstructionContextPolicy::build(
                 {source(InstructionSourceKind::Managed,
                         QStringLiteral("/policy/AGENTS.md"))},
                 {invocation()})}) {
        expect(!manifest.grantsExecution,
               "some manifest path granted execution");
    }

    // 优先级顺序本身必须单向且严格,且嵌套深度不得越出自己的区间。
    expect(InstructionContextPolicy::basePrecedence(InstructionSourceKind::Managed)
               < InstructionContextPolicy::basePrecedence(
                   InstructionSourceKind::UserGlobal)
           && InstructionContextPolicy::basePrecedence(
                   InstructionSourceKind::UserGlobal)
               < InstructionContextPolicy::basePrecedence(
                   InstructionSourceKind::ProjectRoot)
           && InstructionContextPolicy::basePrecedence(
                   InstructionSourceKind::ProjectRoot)
               < InstructionContextPolicy::basePrecedence(
                   InstructionSourceKind::ProjectNested)
           && InstructionContextPolicy::basePrecedence(
                   InstructionSourceKind::ProjectNested)
               < InstructionContextPolicy::basePrecedence(
                   InstructionSourceKind::Skill),
           "the instruction precedence order is not strictly one-directional");
    // 关键:最深的嵌套指令仍然不得越过 Skill 或 Managed。
    const InstructionContextManifest mixed = InstructionContextPolicy::build(
        {source(InstructionSourceKind::Skill, QStringLiteral("/s/SKILL.md")),
         source(InstructionSourceKind::ProjectNested,
                QStringLiteral("/repo/deep/AGENTS.md"),
                InstructionContextPolicy::MaxDirectoryDepth),
         source(InstructionSourceKind::Managed,
                QStringLiteral("/policy/AGENTS.md"))},
        {});
    if (expect(mixed.chain.size() == 3,
               "a mixed instruction chain was not fully recorded")) {
        expect(mixed.chain.at(0).kind == InstructionSourceKind::Managed,
               "managed instructions lost their highest precedence");
        expect(mixed.chain.at(2).kind == InstructionSourceKind::Skill,
               "a deeply nested instruction outranked the invoked Skill");
    }
    // 未分类来源排在最低优先级。
    expect(InstructionContextPolicy::basePrecedence(
               static_cast<InstructionSourceKind>(9999))
               > InstructionContextPolicy::basePrecedence(
                   InstructionSourceKind::Skill),
           "an unclassified instruction source outranked a defined one");

    // 每个已定义来源都有展示标签,否则解释里会出现一条没有来源名字的记录。
    for (const InstructionSourceKind kind : {
             InstructionSourceKind::Managed, InstructionSourceKind::UserGlobal,
             InstructionSourceKind::ProjectRoot,
             InstructionSourceKind::ProjectNested,
             InstructionSourceKind::Skill}) {
        expect(!InstructionContextPolicy::sourceLabel(kind).isEmpty(),
               "an instruction source has no display label");
    }
    expect(!InstructionContextPolicy::sourceLabel(
               static_cast<InstructionSourceKind>(9999)).isEmpty(),
           "an unclassified instruction source has no display label");

    // 相同优先级时顺序必须确定:顺序不确定的覆盖链无法解释任何事情。
    const QList<InstructionSource> unordered{
        source(InstructionSourceKind::ProjectNested,
               QStringLiteral("/repo/b/AGENTS.md"), 3),
        source(InstructionSourceKind::ProjectNested,
               QStringLiteral("/repo/a/AGENTS.md"), 3)};
    const InstructionContextManifest first =
        InstructionContextPolicy::build(unordered, {});
    QList<InstructionSource> reversed{unordered.at(1), unordered.at(0)};
    const InstructionContextManifest second =
        InstructionContextPolicy::build(reversed, {});
    if (expect(first.chain.size() == 2 && second.chain.size() == 2,
               "an equal-precedence chain was not fully recorded")) {
        expect(first.chain.at(0).sourcePath == second.chain.at(0).sourcePath
                   && first.chain.at(1).sourcePath
                       == second.chain.at(1).sourcePath,
               "the manifest order depends on input order rather than precedence");
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    nestedPrecedenceTests();
    skillInvocationTests();
    runtimePolicyWinsTests();
    unusableTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "instruction context manifest tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
