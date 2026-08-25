#include "extension_review_presentation.h"

#include "extension_review_workflow.h"

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

ExtensionRegistryRecord record()
{
    ExtensionRegistryRecord value;
    value.kind = ExtensionKind::Skill;
    value.id = QStringLiteral("acme.formatter");
    value.name = QStringLiteral("Acme Formatter");
    value.version = QStringLiteral("2.1.0");
    value.sourceIdentity = sourceOf("acme");
    value.contentIdentity = contentOf("acme");
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

ExtensionReviewPrompt build(const ExtensionRegistryRecord &value)
{
    return ExtensionReviewPresentation::build(value, granted(), false, QString());
}

bool rejected(const ExtensionReviewPrompt &prompt, const QString &code)
{
    return prompt.state == ExtensionReviewPromptState::Unpresentable
        && prompt.errorCode == code && prompt.title.isEmpty()
        && prompt.reviewedContentIdentity.isEmpty()
        && prompt.reviewedSourceIdentity.isEmpty()
        && prompt.capabilities.isEmpty() && prompt.warnings.isEmpty();
}

bool hasWarning(const ExtensionReviewPrompt &prompt, ExtensionReviewWarning warning)
{
    return prompt.warnings.contains(warning);
}

void readyTests()
{
    const ExtensionRegistryRecord value = record();
    const ExtensionReviewPrompt prompt = build(value);
    expect(prompt.state == ExtensionReviewPromptState::Ready
               && prompt.errorCode.isEmpty()
               && prompt.title == value.name
               && prompt.identifier == value.id
               && prompt.kindLabel == QStringLiteral("Skill")
               && prompt.versionLabel == value.version
               && prompt.scopeLabel == value.scope
               && prompt.capabilities == value.requestedCapabilities
               && prompt.warnings.isEmpty(),
           "a clean record did not produce a clean prompt");

    // 回传的摘要必须是展示的摘要：这是复核流程检测渲染后漂移的唯一依据。
    expect(prompt.reviewedSourceIdentity == value.sourceIdentity
               && prompt.reviewedContentIdentity == value.contentIdentity
               && prompt.sourceIdentity == value.sourceIdentity
               && prompt.contentIdentity == value.contentIdentity,
           "the prompt did not carry the exact identities it displayed");

    // 展示用短摘要同时保留头尾，因为只显示前缀会让构造出的前缀碰撞看起来一致。
    const QString hex = value.contentIdentity.section(QLatin1Char(':'), -1);
    expect(prompt.contentFingerprint.startsWith(hex.left(8))
               && prompt.contentFingerprint.endsWith(hex.right(8))
               && prompt.contentFingerprint != hex,
           "the displayed fingerprint did not preserve both ends of the digest");

    // 展示层与复核流程必须对同一条记录达成一致，且批准使用完整摘要而不是短摘要。
    ExtensionReviewRequest request;
    request.action = ExtensionReviewAction::Approve;
    request.kind = value.kind;
    request.id = value.id;
    request.reviewedSourceIdentity = prompt.reviewedSourceIdentity;
    request.reviewedContentIdentity = prompt.reviewedContentIdentity;
    ExtensionReviewLedgerStoreResult ledger;
    ledger.state = ExtensionReviewLedgerStoreState::Empty;
    const ExtensionReviewPlan plan =
        ExtensionReviewWorkflow::plan(request, {value}, ledger);
    expect(plan.state == ExtensionReviewPlanState::Ready && plan.pins.size() == 1,
           "the prompt's identities were not accepted by the review workflow");

    // 每种扩展都必须有可读标签，否则界面上会出现空白的类型列。
    for (const ExtensionKind kind : {ExtensionKind::CodexPlugin, ExtensionKind::Skill,
                                     ExtensionKind::Mcp}) {
        ExtensionRegistryRecord typed = value;
        typed.kind = kind;
        expect(!build(typed).kindLabel.isEmpty(),
               "an extension kind rendered without a label");
    }

    // 缺失的可选字段被标成"未知"，不能显示为空白：空白无法与真实的空值区分。
    ExtensionRegistryRecord sparse = value;
    sparse.version.clear();
    sparse.scope.clear();
    const ExtensionReviewPrompt sparsePrompt = build(sparse);
    expect(sparsePrompt.state == ExtensionReviewPromptState::Ready
               && sparsePrompt.versionLabel == QStringLiteral("未知")
               && sparsePrompt.scopeLabel == QStringLiteral("未知")
               && hasWarning(sparsePrompt, ExtensionReviewWarning::VersionUnknown),
           "a missing version was rendered blank instead of flagged");
}

void spoofingTests()
{
    const ExtensionRegistryRecord value = record();

    // 双向控制字符可以让名称在屏幕上反向显示，从而冒充另一个扩展。
    ExtensionRegistryRecord bidi = value;
    bidi.name = QStringLiteral("Acme ") + QChar(0x202e) + QStringLiteral("rotamroF");
    expect(rejected(build(bidi),
                    QStringLiteral("extension-review-prompt-name-unsafe")),
           "a bidirectional override was rendered into the review prompt");

    // 零宽字符让两个不同的名称在屏幕上完全一致。
    ExtensionRegistryRecord zeroWidth = value;
    zeroWidth.name = QStringLiteral("Acme") + QChar(0x200b)
        + QStringLiteral(" Formatter");
    expect(rejected(build(zeroWidth),
                    QStringLiteral("extension-review-prompt-name-unsafe")),
           "a zero-width character was rendered into the review prompt");

    // 换行可以把伪造的字段推到复核界面上。
    ExtensionRegistryRecord injected = value;
    injected.name = QStringLiteral("Acme\n信任: 已验证");
    expect(rejected(build(injected),
                    QStringLiteral("extension-review-prompt-name-unsafe")),
           "a newline let a forged field be rendered as review evidence");

    // 行/段分隔符不属于控制或格式类别，但同样会在界面上换行。
    for (const ushort code : {ushort(0x2028), ushort(0x2029), ushort(0xfeff),
                              ushort(0x2066)}) {
        ExtensionRegistryRecord separated = value;
        separated.name = QStringLiteral("Acme") + QChar(code)
            + QStringLiteral(" Formatter");
        expect(rejected(build(separated),
                        QStringLiteral("extension-review-prompt-name-unsafe")),
               "a separator or invisible character was rendered into the prompt");
    }

    // 前后空白让两个不同的名称在表格里看起来一致。
    ExtensionRegistryRecord padded = value;
    padded.name = QStringLiteral("Acme Formatter ");
    expect(rejected(build(padded),
                    QStringLiteral("extension-review-prompt-name-unsafe")),
           "trailing whitespace was rendered instead of rejected");

    // 超长名称必须整体拒绝而不是截断：截断会让两个不同的扩展看起来完全一样。
    ExtensionRegistryRecord overlong = value;
    overlong.name = QString(ExtensionReviewPresentation::MaxTitleCharacters + 1,
                            QLatin1Char('a'));
    expect(rejected(build(overlong),
                    QStringLiteral("extension-review-prompt-name-unsafe")),
           "an over-long name was truncated instead of rejected");
    ExtensionRegistryRecord exact = value;
    exact.name = QString(ExtensionReviewPresentation::MaxTitleCharacters,
                         QLatin1Char('a'));
    expect(build(exact).state == ExtensionReviewPromptState::Ready,
           "a name at the exact limit was rejected");

    // 名称与标识无关时必须显式标记，因为界面上最醒目的就是名称。
    ExtensionRegistryRecord masquerade = value;
    masquerade.name = QStringLiteral("Official Codex Runtime");
    const ExtensionReviewPrompt masqueradePrompt = build(masquerade);
    expect(masqueradePrompt.state == ExtensionReviewPromptState::Ready
               && hasWarning(masqueradePrompt,
                             ExtensionReviewWarning::NameMismatchesIdentifier),
           "a name unrelated to its identifier was presented without a warning");
    expect(!hasWarning(build(value),
                       ExtensionReviewWarning::NameMismatchesIdentifier),
           "a name agreeing with its identifier was flagged as a mismatch");

    // 版本与作用域同样会被展示，因此同样必须可安全展示。
    ExtensionRegistryRecord badVersion = value;
    badVersion.version = QStringLiteral("2.1.0") + QChar(0x202e);
    expect(rejected(build(badVersion),
                    QStringLiteral("extension-review-prompt-version-unsafe")),
           "an unsafe version string was rendered into the review prompt");
    ExtensionRegistryRecord badScope = value;
    badScope.scope = QStringLiteral("user") + QChar(0x200f);
    expect(rejected(build(badScope),
                    QStringLiteral("extension-review-prompt-scope-unsafe")),
           "an unsafe scope string was rendered into the review prompt");
}

void targetTests()
{
    const ExtensionRegistryRecord value = record();

    // 标识与摘要不合法的记录无法呈现可批准的目标。
    ExtensionRegistryRecord badId = value;
    badId.id = QStringLiteral("Acme/Formatter");
    expect(rejected(build(badId),
                    QStringLiteral("extension-review-prompt-id-invalid")),
           "an invalid identifier was presented for review");
    ExtensionRegistryRecord truncated = value;
    truncated.contentIdentity = QStringLiteral("extension-content:sha256:abc");
    expect(rejected(build(truncated),
                    QStringLiteral("extension-review-prompt-identity-invalid")),
           "a truncated digest was presented as review evidence");
    ExtensionRegistryRecord unprefixed = value;
    unprefixed.sourceIdentity =
        value.sourceIdentity.section(QLatin1Char(':'), -1);
    expect(rejected(build(unprefixed),
                    QStringLiteral("extension-review-prompt-identity-invalid")),
           "a digest without its domain prefix was presented as review evidence");

    // 未安装的记录不能被呈现为可复核目标：复核流程也会拒绝它。
    ExtensionRegistryRecord uninstalled = value;
    uninstalled.installed = false;
    expect(rejected(build(uninstalled),
                    QStringLiteral("extension-review-prompt-not-installed")),
           "an uninstalled extension was presented for approval");
}

void capabilityTests()
{
    ExtensionRegistryRecord value = record();

    // 超出授予集合的能力必须被标记，而不是静默展示成普通条目。
    value.requestedCapabilities = QStringList{QStringLiteral("skill-content"),
                                              QStringLiteral("telemetry-upload")};
    const ExtensionReviewPrompt ungranted = build(value);
    expect(ungranted.state == ExtensionReviewPromptState::Ready
               && hasWarning(ungranted,
                             ExtensionReviewWarning::CapabilityNotGranted),
           "a capability outside the granted set was presented without a warning");

    // 写入与执行类能力必须额外标记：当前产品保持只读。
    for (const QString &capability : {QStringLiteral("process"),
                                      QStringLiteral("filesystem-write"),
                                      QStringLiteral("command-execution"),
                                      QStringLiteral("git-mutation")}) {
        ExtensionRegistryRecord writable = record();
        writable.requestedCapabilities = QStringList{capability};
        const ExtensionReviewPrompt prompt = build(writable);
        expect(prompt.state == ExtensionReviewPromptState::Ready
                   && hasWarning(prompt,
                                 ExtensionReviewWarning::CapabilityBeyondReadOnly),
               "a write or execution capability was presented as read-only");
    }
    expect(!hasWarning(build(record()),
                       ExtensionReviewWarning::CapabilityBeyondReadOnly),
           "a read-only capability was flagged as going beyond read-only");

    // 重复条目让屏幕上的条数与实际请求不一致。
    ExtensionRegistryRecord duplicated = record();
    duplicated.requestedCapabilities = QStringList{QStringLiteral("skill-content"),
                                                    QStringLiteral("skill-content")};
    expect(rejected(build(duplicated),
                    QStringLiteral("extension-review-prompt-capability-duplicate")),
           "duplicate capabilities were rendered as separate entries");

    // 能力清单过长时整体拒绝，而不是只展示前几条。
    ExtensionRegistryRecord many = record();
    for (int i = 0; i <= ExtensionReviewPresentation::MaxCapabilities; ++i) {
        many.requestedCapabilities.append(QStringLiteral("capability-%1").arg(i));
    }
    expect(rejected(build(many),
                    QStringLiteral("extension-review-prompt-capability-limit")),
           "an over-long capability list was silently shortened");

    // 单条能力本身不可安全展示时同样拒绝。
    ExtensionRegistryRecord unsafe = record();
    unsafe.requestedCapabilities =
        QStringList{QStringLiteral("skill") + QChar(0x202e)
                    + QStringLiteral("-content")};
    expect(rejected(build(unsafe),
                    QStringLiteral("extension-review-prompt-capability-unsafe")),
           "an unsafe capability name was rendered into the review prompt");
}

void historyTests()
{
    const ExtensionRegistryRecord value = record();

    // 内容变更后的重新复核必须被明确标记：否则人会以为自己在确认一个已知结论。
    const ExtensionReviewPrompt changed = ExtensionReviewPresentation::build(
        value, granted(), true, contentOf("acme-old"));
    expect(changed.state == ExtensionReviewPromptState::Ready
               && hasWarning(changed,
                             ExtensionReviewWarning::ContentChangedSinceReview),
           "content that changed since the last review was presented as unchanged");
    const ExtensionReviewPrompt same = ExtensionReviewPresentation::build(
        value, granted(), true, value.contentIdentity);
    expect(same.state == ExtensionReviewPromptState::Ready
               && !hasWarning(same,
                              ExtensionReviewWarning::ContentChangedSinceReview),
           "unchanged content was flagged as having drifted");

    // 声称复核过却没有可比较的旧摘要时无法判断是否变更，必须拒绝而不是假设未变。
    expect(rejected(ExtensionReviewPresentation::build(value, granted(), true,
                                                       QString()),
                    QStringLiteral("extension-review-prompt-history-invalid")),
           "an unusable review history was assumed to be unchanged");
    expect(rejected(ExtensionReviewPresentation::build(
                        value, granted(), true,
                        QStringLiteral("extension-content:sha256:short")),
                    QStringLiteral("extension-review-prompt-history-invalid")),
           "a malformed review history was assumed to be unchanged");

    // 兼容性未解决时必须标记：复核不能替代兼容性证据。
    for (const ExtensionCompatibilityState state : {
             ExtensionCompatibilityState::Unknown,
             ExtensionCompatibilityState::Incompatible}) {
        ExtensionRegistryRecord unresolved = value;
        unresolved.compatibility = state;
        expect(hasWarning(build(unresolved),
                          ExtensionReviewWarning::CompatibilityUnresolved),
               "an unresolved compatibility state was presented without a warning");
    }
    expect(!hasWarning(build(value),
                       ExtensionReviewWarning::CompatibilityUnresolved),
           "a compatible record was flagged as unresolved");

    // 警告顺序固定，避免排版顺序影响人的判断。
    ExtensionRegistryRecord noisy = value;
    noisy.name = QStringLiteral("Official Codex Runtime");
    noisy.version.clear();
    noisy.compatibility = ExtensionCompatibilityState::Unknown;
    noisy.requestedCapabilities = QStringList{QStringLiteral("filesystem-write")};
    const ExtensionReviewPrompt ordered = ExtensionReviewPresentation::build(
        noisy, granted(), true, contentOf("acme-old"));
    expect(ordered.warnings
               == QList<ExtensionReviewWarning>{
                      ExtensionReviewWarning::NameMismatchesIdentifier,
                      ExtensionReviewWarning::VersionUnknown,
                      ExtensionReviewWarning::CapabilityNotGranted,
                      ExtensionReviewWarning::CapabilityBeyondReadOnly,
                      ExtensionReviewWarning::CompatibilityUnresolved,
                      ExtensionReviewWarning::ContentChangedSinceReview},
           "the review warnings were not emitted in a fixed order");
}

void authorityTests()
{
    // 呈现不授予启用权，也不改判信任：它只决定人能不能安全地看到这条记录。
    ExtensionRegistryRecord value = record();
    value.trust = ExtensionTrustState::Unverified;
    const ExtensionReviewPrompt prompt = build(value);
    expect(prompt.state == ExtensionReviewPromptState::Ready
               && value.trust == ExtensionTrustState::Unverified
               && !value.effectiveEnabled,
           "building a review prompt changed trust or enablement");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    readyTests();
    spoofingTests();
    targetTests();
    capabilityTests();
    historyTests();
    authorityTests();
    if (failures == 0) {
        QTextStream(stdout) << "extension review presentation tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
