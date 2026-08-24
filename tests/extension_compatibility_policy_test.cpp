#include "extension_compatibility_policy.h"

#include <QCoreApplication>
#include <QSet>
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

ExtensionHostProfile host(const QString &codexVersion,
                          const QStringList &granted =
                              ExtensionCompatibilityPolicy::defaultGrantedCapabilities())
{
    ExtensionHostProfile profile;
    profile.codexVersion = codexVersion;
    profile.grantedCapabilities = granted;
    return profile;
}

ExtensionRegistryRecord record(ExtensionKind kind,
                               const QString &version,
                               const QStringList &requested)
{
    ExtensionRegistryRecord value;
    value.kind = kind;
    value.id = QStringLiteral("sample");
    value.name = QStringLiteral("Sample");
    value.version = version;
    value.requestedCapabilities = requested;
    value.installed = true;
    return value;
}

bool compatible(const ExtensionCompatibilityDecision &decision)
{
    // 注册表要求 Compatible 的理由必须为空，否则 build() 会拒绝整条记录。
    return decision.state == ExtensionCompatibilityState::Compatible
        && decision.reason.isEmpty();
}

bool incompatible(const ExtensionCompatibilityDecision &decision,
                  const QString &reason)
{
    return decision.state == ExtensionCompatibilityState::Incompatible
        && decision.reason == reason;
}

bool unknown(const ExtensionCompatibilityDecision &decision, const QString &reason)
{
    return decision.state == ExtensionCompatibilityState::Unknown
        && decision.reason == reason;
}

void grantedSetTests()
{
    const QStringList granted =
        ExtensionCompatibilityPolicy::defaultGrantedCapabilities();
    const QSet<QString> set(granted.cbegin(), granted.cend());
    // 只读授权的核心断言：进程执行不在授予集合内。
    expect(!set.contains(QStringLiteral("process")),
           "the read-only host grants process execution");
    expect(set.contains(QStringLiteral("filesystem-read"))
               && set.contains(QStringLiteral("mcp-tools"))
               && set.contains(QStringLiteral("network"))
               && set.contains(QStringLiteral("skill-content")),
           "the read-only host lost a granted read capability");
    expect(granted.size() == 4 && set.size() == 4,
           "the granted capability set changed shape");
    QStringList sorted = granted;
    sorted.sort();
    expect(sorted == granted, "granted capabilities are not deterministically ordered");
}

void capabilityTests()
{
    // 请求进程执行的 Skill 在当前授权下确定不兼容，而不是"未知"。
    expect(incompatible(
               ExtensionCompatibilityPolicy::evaluate(
                   record(ExtensionKind::Skill, QStringLiteral("1.0.0"),
                          {QStringLiteral("skill-content"), QStringLiteral("process")}),
                   host(QStringLiteral("1.2.3"))),
               QStringLiteral("extension-capability-not-granted")),
           "a process-requesting skill was not rejected");

    // MCP stdio 服务器请求进程执行，因此在只读授权下同样确定不兼容。
    expect(incompatible(
               ExtensionCompatibilityPolicy::evaluate(
                   record(ExtensionKind::Mcp, QString(),
                          {QStringLiteral("process"), QStringLiteral("mcp-tools")}),
                   host(QString())),
               QStringLiteral("extension-capability-not-granted")),
           "a stdio MCP server was not rejected under read-only authority");

    // 远程 MCP 服务器只请求网络与 MCP 工具，两者都在授予集合内。
    expect(compatible(ExtensionCompatibilityPolicy::evaluate(
               record(ExtensionKind::Mcp, QString(),
                      {QStringLiteral("network"), QStringLiteral("mcp-tools")}),
               host(QString()))),
           "a granted remote MCP server was not accepted");

    // 能力判定先于其他检查：不可读版本不能把确定的拒绝降级成"未知"。
    expect(incompatible(
               ExtensionCompatibilityPolicy::evaluate(
                   record(ExtensionKind::Skill, QStringLiteral("dev"),
                          {QStringLiteral("process")}),
                   host(QStringLiteral("1.2.3"))),
               QStringLiteral("extension-capability-not-granted")),
           "an unreadable version downgraded a definite rejection");

    // 宿主证据缺失也不能把确定的拒绝降级。
    expect(incompatible(
               ExtensionCompatibilityPolicy::evaluate(
                   record(ExtensionKind::CodexPlugin, QString(),
                          {QStringLiteral("process")}),
                   host(QString())),
               QStringLiteral("extension-capability-not-granted")),
           "a missing host version downgraded a definite rejection");

    // 未知能力同样不在授予集合内，因此确定不兼容。
    expect(incompatible(
               ExtensionCompatibilityPolicy::evaluate(
                   record(ExtensionKind::Skill, QStringLiteral("1.0.0"),
                          {QStringLiteral("shell-execute")}),
                   host(QStringLiteral("1.2.3"))),
               QStringLiteral("extension-capability-not-granted")),
           "an unknown capability was not rejected");

    // 空请求集合不请求任何东西，因此没有可拒绝的理由。
    expect(compatible(ExtensionCompatibilityPolicy::evaluate(
               record(ExtensionKind::Skill, QStringLiteral("1.0.0"), {}),
               host(QStringLiteral("1.2.3")))),
           "a skill requesting nothing was not accepted");

    // 宿主授予集合为空时，任何请求都确定不兼容。
    expect(incompatible(
               ExtensionCompatibilityPolicy::evaluate(
                   record(ExtensionKind::Skill, QStringLiteral("1.0.0"),
                          {QStringLiteral("skill-content")}),
                   host(QStringLiteral("1.2.3"), {})),
               QStringLiteral("extension-capability-not-granted")),
           "an empty grant set still accepted a request");
}

void versionTests()
{
    for (const QString &value : {QStringLiteral("1.0"), QStringLiteral("1.2.3"),
                                 QStringLiteral("0.1.2.3"),
                                 QStringLiteral("1.2.3-beta.1"),
                                 QStringLiteral("1.2.3+build7")}) {
        expect(compatible(ExtensionCompatibilityPolicy::evaluate(
                   record(ExtensionKind::Skill, value,
                          {QStringLiteral("skill-content")}),
                   host(QString()))),
               "a readable release version was rejected");
    }
    for (const QString &value : {QStringLiteral("1"), QStringLiteral("unknown"),
                                 QStringLiteral("v1.2.3"),
                                 QStringLiteral("1.2.3.4.5"),
                                 QStringLiteral("1..2"),
                                 QStringLiteral("1.2.3-"),
                                 QStringLiteral("1.2.3 "),
                                 QStringLiteral("1.2.3-beta 1"),
                                 QStringLiteral("1234567890.0")}) {
        expect(unknown(ExtensionCompatibilityPolicy::evaluate(
                   record(ExtensionKind::Skill, value,
                          {QStringLiteral("skill-content")}),
                   host(QString())),
                   QStringLiteral("extension-version-unreadable")),
               "an unreadable version did not resolve to unknown");
    }
    // 版本缺失对非 Codex 记录不是不可核查：MCP 配置本身不带版本。
    expect(compatible(ExtensionCompatibilityPolicy::evaluate(
               record(ExtensionKind::Mcp, QString(),
                      {QStringLiteral("mcp-tools")}),
               host(QString()))),
           "a versionless MCP record was not accepted");
}

void codexHostTests()
{
    const QStringList granted{QStringLiteral("filesystem-read")};

    // 宿主版本缺失：只能"未知"。绝不能因为"看起来没问题"而判定兼容。
    expect(unknown(ExtensionCompatibilityPolicy::evaluate(
               record(ExtensionKind::CodexPlugin, QStringLiteral("1.0.0"), {}),
               host(QString())),
               QStringLiteral("codex-plugin-host-version-unknown")),
           "a Codex plugin was judged without host version evidence");

    // 宿主版本读不出：同样"未知"，但用不同的代码，便于区分证据缺失与证据损坏。
    expect(unknown(ExtensionCompatibilityPolicy::evaluate(
               record(ExtensionKind::CodexPlugin, QStringLiteral("1.0.0"), {}),
               host(QStringLiteral("not-a-version"))),
               QStringLiteral("codex-plugin-host-version-unreadable")),
           "an unreadable host version was not distinguished");

    // 插件自身没有版本时无法核查，即使宿主版本可读。
    expect(unknown(ExtensionCompatibilityPolicy::evaluate(
               record(ExtensionKind::CodexPlugin, QString(), {}),
               host(QStringLiteral("1.2.3"))),
               QStringLiteral("codex-plugin-version-missing")),
           "a versionless Codex plugin was judged compatible");

    // 两侧版本都可读且能力已授予：兼容。
    expect(compatible(ExtensionCompatibilityPolicy::evaluate(
               record(ExtensionKind::CodexPlugin, QStringLiteral("2.0.1"),
                      {QStringLiteral("filesystem-read")}),
               host(QStringLiteral("1.2.3"), granted))),
           "a fully evidenced Codex plugin was not accepted");

    // 宿主版本证据只对 Codex 插件是必需的：Skill 与 MCP 不在 Codex 内运行。
    expect(compatible(ExtensionCompatibilityPolicy::evaluate(
               record(ExtensionKind::Skill, QStringLiteral("1.0.0"),
                      {QStringLiteral("skill-content")}),
               host(QString()))),
           "a skill required Codex host version evidence");
}

void applyTests()
{
    QList<ExtensionRegistryRecord> records{
        record(ExtensionKind::Skill, QStringLiteral("1.0.0"),
               {QStringLiteral("skill-content")}),
        record(ExtensionKind::Skill, QStringLiteral("1.0.0"),
               {QStringLiteral("process")}),
        record(ExtensionKind::CodexPlugin, QStringLiteral("1.0.0"), {}),
    };
    records[0].trust = ExtensionTrustState::Verified;
    records[1].trust = ExtensionTrustState::Verified;
    records[0].compatibility = ExtensionCompatibilityState::Incompatible;
    records[0].compatibilityReason = QStringLiteral("stale-reason");
    records[1].compatibility = ExtensionCompatibilityState::Compatible;
    records[1].compatibilityReason.clear();

    ExtensionCompatibilityPolicy::apply(&records, host(QString()));

    expect(compatible({records[0].compatibility, records[0].compatibilityReason}),
           "apply did not replace a stale incompatible verdict");
    expect(incompatible({records[1].compatibility, records[1].compatibilityReason},
                        QStringLiteral("extension-capability-not-granted")),
           "apply did not replace a stale compatible verdict");
    expect(unknown({records[2].compatibility, records[2].compatibilityReason},
                   QStringLiteral("codex-plugin-host-version-unknown")),
           "apply did not evaluate every record");

    // 判定兼容不等于授权：信任状态与生效启用状态一概不动。
    expect(records[0].trust == ExtensionTrustState::Verified
               && records[1].trust == ExtensionTrustState::Verified
               && records[2].trust == ExtensionTrustState::Unverified,
           "apply changed a trust state");
    expect(!records[0].effectiveEnabled && !records[1].effectiveEnabled
               && !records[2].effectiveEnabled,
           "apply enabled an extension");
    expect(!records[0].updateAvailable && !records[0].recoveryAvailable,
           "apply asserted update or recovery authority");
    expect(records[0].installed && records[0].id == QStringLiteral("sample")
               && records[0].version == QStringLiteral("1.0.0"),
           "apply mutated source-reported facts");

    // 空指针与空列表都不能崩溃。
    ExtensionCompatibilityPolicy::apply(nullptr, host(QString()));
    QList<ExtensionRegistryRecord> empty;
    ExtensionCompatibilityPolicy::apply(&empty, host(QString()));
    expect(empty.isEmpty(), "apply invented records");
}

void registryAgreementTests()
{
    // 每个非兼容理由都必须通过注册表的固定代码格式，否则整条记录会被拒绝。
    const QStringList reasons{
        QStringLiteral("extension-capability-not-granted"),
        QStringLiteral("extension-version-unreadable"),
        QStringLiteral("codex-plugin-host-version-unknown"),
        QStringLiteral("codex-plugin-host-version-unreadable"),
        QStringLiteral("codex-plugin-version-missing")};
    for (const QString &reason : reasons) {
        bool valid = !reason.isEmpty() && reason.size() <= 96
            && reason.at(0).isLetterOrNumber();
        for (const QChar character : reason) {
            const ushort value = character.unicode();
            if (!((value >= 'a' && value <= 'z')
                    || (value >= '0' && value <= '9') || value == '-')) {
                valid = false;
            }
        }
        expect(valid, "a compatibility reason is not a fixed registry code");
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    grantedSetTests();
    capabilityTests();
    versionTests();
    codexHostTests();
    applyTests();
    registryAgreementTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension compatibility policy tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
