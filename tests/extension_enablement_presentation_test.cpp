#include "extension_enablement_presentation.h"

#include "extension_enablement_policy.h"
#include "extension_enablement_workflow.h"

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

// 三道门禁均已满足的记录：这是唯一应当出现可点击启用动作的状态。
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
    value.scope = QStringLiteral("user");
    value.requestedCapabilities = QStringList{QStringLiteral("skill-content")};
    value.installed = true;
    return value;
}

QStringList granted()
{
    return QStringList{QStringLiteral("filesystem-read"), QStringLiteral("mcp-tools"),
                       QStringLiteral("network"), QStringLiteral("skill-content")};
}

ExtensionEnablementPrompt build(const ExtensionRegistryRecord &value)
{
    return ExtensionEnablementPresentation::build(value, granted(), false, QString());
}

bool warns(const ExtensionEnablementPrompt &prompt,
           ExtensionEnablementWarning warning)
{
    return prompt.warnings.contains(warning);
}

void readyTests()
{
    const ExtensionEnablementPrompt prompt = build(record());
    expect(prompt.state == ExtensionEnablementPromptState::Ready,
           "a reviewed, compatible, installed record was not presentable");
    expect(prompt.blockReason == ExtensionEnablementBlockReason::None,
           "a ready prompt carried a block reason");
    expect(prompt.errorCode.isEmpty(), "a ready prompt carried an error code");
    expect(prompt.title == QStringLiteral("Acme Formatter"),
           "the prompt title is not the record name");
    expect(prompt.identifier == QStringLiteral("acme.formatter"),
           "the prompt identifier is not the record id");
    expect(prompt.kindLabel == QStringLiteral("Skill"),
           "the prompt does not label the extension kind");
    expect(prompt.versionLabel == QStringLiteral("2.1.0"),
           "the prompt does not display the version");
    expect(prompt.scopeLabel == QStringLiteral("user"),
           "the prompt does not display the scope");
    expect(prompt.capabilities == QStringList{QStringLiteral("skill-content")},
           "the prompt does not display the requested capabilities");

    // 授权绑定的是人看到的那份摘要，不是缩短后的展示值。
    expect(prompt.reviewedSourceIdentity == record().sourceIdentity
               && prompt.reviewedContentIdentity == record().contentIdentity,
           "the prompt does not echo the exact identities it displayed");
    expect(prompt.sourceIdentity == record().sourceIdentity
               && prompt.contentIdentity == record().contentIdentity,
           "the prompt does not display the full identities");

    // 短摘要保留头尾：只显示前缀会让构造出的前缀碰撞在屏幕上看起来一致。
    const QString hex = record().contentIdentity.section(QLatin1Char(':'), -1);
    expect(prompt.contentFingerprint.startsWith(hex.left(8))
               && prompt.contentFingerprint.endsWith(hex.right(8)),
           "the displayed fingerprint drops one end of the digest");
    expect(prompt.contentFingerprint != prompt.contentIdentity,
           "the fingerprint is the full identity rather than a display value");

    // 授权当前不会让任何内容运行，这一点必须永远被说明。
    expect(warns(prompt, ExtensionEnablementWarning::GrantDoesNotExecuteYet),
           "the prompt does not state that a grant executes nothing yet");
}

// 呈现与授权之间的差异是这一层存在的理由：屏幕上的字符必须与被授权的字符串一致。
void spoofingTests()
{
    for (const QString &unsafe : {
             QStringLiteral("Acme‮Formatter"),   // 双向覆盖
             QStringLiteral("Acme​Formatter"),   // 零宽空格
             QStringLiteral("Acme⁦Formatter"),   // 双向隔离
             QStringLiteral("Acme\nFormatter"),       // 控制字符
             QStringLiteral("Acme") + QChar(0x0000) + QStringLiteral("Formatter"), // NUL
             QStringLiteral("﻿Acme"),            // BOM
             QStringLiteral("Acme Formatter"),   // 行分隔符
             QStringLiteral(" Acme Formatter"),       // 前导空白
             QStringLiteral("Acme Formatter ")}) {    // 尾随空白
        ExtensionRegistryRecord value = record();
        value.name = unsafe;
        const ExtensionEnablementPrompt prompt = build(value);
        expect(prompt.state == ExtensionEnablementPromptState::Unpresentable
                   && prompt.errorCode
                       == QStringLiteral("extension-enablement-prompt-name-unsafe"),
               "an unsafe name was presentable for enablement");
    }

    // 超长名称被拒绝而不是截断：截断会让两个不同的扩展在屏幕上看起来完全一样。
    ExtensionRegistryRecord tooLong = record();
    tooLong.name = QString(
        ExtensionEnablementPresentation::MaxTitleCharacters + 1, QLatin1Char('a'));
    expect(build(tooLong).state == ExtensionEnablementPromptState::Unpresentable,
           "an over-long name was truncated instead of rejected");

    // 名称与标识无关时必须警告：那正是冒充另一个已被授权的扩展的方式。
    ExtensionRegistryRecord mismatched = record();
    mismatched.name = QStringLiteral("System Update");
    expect(warns(build(mismatched),
                 ExtensionEnablementWarning::NameMismatchesIdentifier),
           "a name unrelated to its identifier was not flagged");

    // 版本与作用域同样来自不可信来源。
    ExtensionRegistryRecord unsafeVersion = record();
    unsafeVersion.version = QStringLiteral("1.0‮0");
    expect(build(unsafeVersion).errorCode
               == QStringLiteral("extension-enablement-prompt-version-unsafe"),
           "an unsafe version was presentable");
    ExtensionRegistryRecord unsafeScope = record();
    unsafeScope.scope = QStringLiteral("user​");
    expect(build(unsafeScope).errorCode
               == QStringLiteral("extension-enablement-prompt-scope-unsafe"),
           "an unsafe scope was presentable");

    // 摘要形式不合法时没有可授权的目标。
    for (const QString &bad : {
             QStringLiteral(""),
             QStringLiteral("extension-content:sha256:abc"),
             QStringLiteral("extension-content:sha256:")
                 + QString(64, QLatin1Char('A')),
             QStringLiteral("extension-source:sha256:")
                 + QString(64, QLatin1Char('a'))}) {
        ExtensionRegistryRecord value = record();
        value.contentIdentity = bad;
        expect(build(value).errorCode
                   == QStringLiteral("extension-enablement-prompt-identity-invalid"),
               "a malformed content identity was presentable");
    }
    ExtensionRegistryRecord badId = record();
    badId.id = QStringLiteral("Acme..Formatter");
    expect(build(badId).errorCode
               == QStringLiteral("extension-enablement-prompt-id-invalid"),
           "a malformed identifier was presentable");
}

// 三道门禁必须在提问之前满足，否则界面会邀请人授权一件此刻无法被授权的事。
void gateTests()
{
    ExtensionRegistryRecord unreviewed = record();
    unreviewed.trust = ExtensionTrustState::Unverified;
    const ExtensionEnablementPrompt trustBlocked = build(unreviewed);
    expect(trustBlocked.state == ExtensionEnablementPromptState::Blocked
               && trustBlocked.blockReason
                   == ExtensionEnablementBlockReason::TrustMissing,
           "an unreviewed record offered an enable action");
    // Blocked 仍然完成呈现：界面需要说明原因，而不是显示一个空对话框。
    expect(!trustBlocked.title.isEmpty() && !trustBlocked.identifier.isEmpty(),
           "a blocked prompt rendered nothing to explain itself");

    for (const ExtensionCompatibilityState state : {
             ExtensionCompatibilityState::Unknown,
             ExtensionCompatibilityState::Incompatible}) {
        ExtensionRegistryRecord value = record();
        value.compatibility = state;
        const ExtensionEnablementPrompt prompt = build(value);
        expect(prompt.state == ExtensionEnablementPromptState::Blocked
                   && prompt.blockReason
                       == ExtensionEnablementBlockReason::CompatibilityMissing,
               "an incompatible record offered an enable action");
    }

    ExtensionRegistryRecord absent = record();
    absent.installed = false;
    const ExtensionEnablementPrompt notInstalled = build(absent);
    expect(notInstalled.state == ExtensionEnablementPromptState::Blocked
               && notInstalled.blockReason
                   == ExtensionEnablementBlockReason::NotInstalled,
           "an uninstalled record offered an enable action");

    // 缺少复核与缺少兼容必须是可区分的诊断：把前者显示成后者会让人以为换台机器就能
    // 运行一份从未被人看过的内容。安装门禁排在最前，因为未安装时另两项无从判断。
    ExtensionRegistryRecord everything = record();
    everything.installed = false;
    everything.trust = ExtensionTrustState::Unverified;
    everything.compatibility = ExtensionCompatibilityState::Incompatible;
    expect(build(everything).blockReason
               == ExtensionEnablementBlockReason::NotInstalled,
           "the block reason order does not report the installation gate first");
    ExtensionRegistryRecord reviewedGate = record();
    reviewedGate.trust = ExtensionTrustState::Unverified;
    reviewedGate.compatibility = ExtensionCompatibilityState::Incompatible;
    expect(build(reviewedGate).blockReason
               == ExtensionEnablementBlockReason::TrustMissing,
           "a missing review was reported as a compatibility problem");

    // 门禁未满足时也绝不能出现 Ready：Ready 是授权动作出现的唯一条件。
    expect(trustBlocked.state != ExtensionEnablementPromptState::Ready
               && notInstalled.state != ExtensionEnablementPromptState::Ready,
           "a blocked record was presented as ready to enable");
}

void capabilityTests()
{
    // 越界能力必须被标记，而不是静默通过。
    ExtensionRegistryRecord ungranted = record();
    ungranted.requestedCapabilities = QStringList{QStringLiteral("bluetooth")};
    expect(warns(build(ungranted),
                 ExtensionEnablementWarning::CapabilityNotGranted),
           "a capability outside the granted set was not flagged");

    // 写入与执行类能力对只读产品是显式风险。
    for (const QString &capability : {
             QStringLiteral("process"), QStringLiteral("command-execution"),
             QStringLiteral("git-mutation"), QStringLiteral("filesystem-write")}) {
        ExtensionRegistryRecord value = record();
        value.requestedCapabilities = QStringList{capability};
        expect(warns(build(value),
                     ExtensionEnablementWarning::CapabilityBeyondReadOnly),
               "a write or execution capability was not flagged against read-only");
    }

    ExtensionRegistryRecord duplicated = record();
    duplicated.requestedCapabilities = QStringList{
        QStringLiteral("skill-content"), QStringLiteral("skill-content")};
    expect(build(duplicated).errorCode
               == QStringLiteral("extension-enablement-prompt-capability-duplicate"),
           "a duplicated capability list was presentable");

    ExtensionRegistryRecord tooMany = record();
    for (int i = 0; i <= ExtensionEnablementPresentation::MaxCapabilities; ++i) {
        tooMany.requestedCapabilities.append(
            QStringLiteral("capability-%1").arg(i));
    }
    expect(build(tooMany).errorCode
               == QStringLiteral("extension-enablement-prompt-capability-limit"),
           "an over-limit capability list was presentable");

    ExtensionRegistryRecord unsafeCapability = record();
    unsafeCapability.requestedCapabilities =
        QStringList{QStringLiteral("skill‮content")};
    expect(build(unsafeCapability).errorCode
               == QStringLiteral("extension-enablement-prompt-capability-unsafe"),
           "an unsafe capability name was presentable");
}

void existingGrantTests()
{
    const ExtensionRegistryRecord value = record();

    // 等效授权已存在时必须说明本次不会改变任何状态，否则人以为自己做了一次新决定。
    const ExtensionEnablementPrompt same = ExtensionEnablementPresentation::build(
        value, granted(), true, value.contentIdentity);
    expect(warns(same, ExtensionEnablementWarning::AlreadyGranted),
           "an identical existing grant was not disclosed");
    expect(!warns(same, ExtensionEnablementWarning::ContentChangedSinceGrant),
           "an unchanged content identity was reported as drift");

    // 内容自上次授权后发生变化时，这是一次重新授权而不是首次授权。
    const ExtensionEnablementPrompt drifted = ExtensionEnablementPresentation::build(
        value, granted(), true, contentOf("older"));
    expect(warns(drifted, ExtensionEnablementWarning::ContentChangedSinceGrant),
           "content changed since the existing grant was not disclosed");
    expect(!warns(drifted, ExtensionEnablementWarning::AlreadyGranted),
           "a drifted grant was reported as already granted");

    // 声称已有授权却没有可比较的摘要时无法判断是哪一种情况。
    for (const QString &bad : {
             QStringLiteral(""), QStringLiteral("extension-content:sha256:zz")}) {
        expect(ExtensionEnablementPresentation::build(value, granted(), true, bad)
                   .errorCode
                   == QStringLiteral("extension-enablement-prompt-grant-invalid"),
               "a claimed grant without a usable digest was presentable");
    }
}

// 撤销永远可用：被篡改、被撤回复核、来源消失的扩展都必须仍然可以收回授权。
void revocationTests()
{
    const ExtensionRegistryRecord value = record();
    const ExtensionRevocationPrompt present =
        ExtensionEnablementPresentation::buildRevocation(
            value.kind, value.id, &value);
    expect(present.state == ExtensionRevocationPromptState::Ready
               && !present.targetAbsent
               && present.title == value.name
               && present.identifier == value.id,
           "revoking a present grant was not presentable");

    // 来源消失后仍然可以撤销，但界面必须说明目标已不存在。
    const ExtensionRevocationPrompt absent =
        ExtensionEnablementPresentation::buildRevocation(
            ExtensionKind::Skill, value.id, nullptr);
    expect(absent.state == ExtensionRevocationPromptState::Ready
               && absent.targetAbsent && absent.title == value.id,
           "revoking a vanished target was refused or unlabelled");

    // 门禁不适用于撤销：未复核、不兼容、未安装的记录都必须仍然可撤销。
    for (ExtensionRegistryRecord blocked : {record(), record(), record()}) {
        blocked.trust = ExtensionTrustState::Unverified;
        blocked.compatibility = ExtensionCompatibilityState::Incompatible;
        blocked.installed = false;
        expect(ExtensionEnablementPresentation::buildRevocation(
                   blocked.kind, blocked.id, &blocked).state
                   == ExtensionRevocationPromptState::Ready,
               "a blocked record could not have its grant revoked");
    }

    // 名称不可展示时退回使用标识，而不是拒绝撤销：撤销必须始终可用。
    ExtensionRegistryRecord unsafe = record();
    unsafe.name = QStringLiteral("Acme‮Formatter");
    const ExtensionRevocationPrompt fallback =
        ExtensionEnablementPresentation::buildRevocation(
            unsafe.kind, unsafe.id, &unsafe);
    expect(fallback.state == ExtensionRevocationPromptState::Ready
               && fallback.title == unsafe.id,
           "an unsafe name blocked revocation instead of falling back to the id");

    // 标识本身不合法时无法确定撤销目标。
    expect(ExtensionEnablementPresentation::buildRevocation(
               ExtensionKind::Skill, QStringLiteral("Bad Id"), nullptr).errorCode
               == QStringLiteral("extension-revocation-prompt-id-invalid"),
           "a malformed identifier produced a revocation target");
}

// 呈现层不得持有任何授权：它只把不可信文本变成可安全展示的字符串。
void authorityTests()
{
    const ExtensionEnablementPrompt prompt = build(record());
    // 呈现产出的摘要原样进入启用请求，因此渲染之后的漂移可以被流程检测出来。
    ExtensionEnablementRequest request;
    request.action = ExtensionEnablementAction::Enable;
    request.kind = record().kind;
    request.id = record().id;
    request.reviewedSourceIdentity = prompt.reviewedSourceIdentity;
    request.reviewedContentIdentity = prompt.reviewedContentIdentity;
    expect(request.reviewedContentIdentity == record().contentIdentity,
           "the enablement request does not carry the displayed identity");

    // 一个 Ready 的呈现不会让任何内容运行：启用判定仍然独立要求授权存在。
    const ExtensionEnablementDecision decision =
        ExtensionEnablementPolicy::evaluate(record(), {});
    expect(!decision.enabled,
           "a presentable prompt enabled a record without a grant");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    readyTests();
    spoofingTests();
    gateTests();
    capabilityTests();
    existingGrantTests();
    revocationTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension enablement presentation tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
