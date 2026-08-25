#include "extension_review_presentation.h"

#include <QRegularExpression>
#include <QSet>

namespace {

// 除制表符外的控制字符、格式字符与代理码位都不能进入复核界面：它们可以让屏幕上的
// 文本与实际字符串不一致，从而让人批准一个自己没有看到的扩展。
bool safeDisplayText(const QString &value, int maximum)
{
    if (value.isEmpty() || value.size() > maximum) return false;
    for (const QChar character : value) {
        const QChar::Category category = character.category();
        if (character.isNull() || category == QChar::Other_Control
                || category == QChar::Other_Format
                || category == QChar::Other_Surrogate
                || category == QChar::Other_PrivateUse
                || category == QChar::Other_NotAssigned) {
            return false;
        }
        // 双向控制与零宽字符即使不属于格式类，也必须显式排除。
        const ushort code = character.unicode();
        if (code == 0x7f || (code >= 0x2028 && code <= 0x202f)
                || (code >= 0x2066 && code <= 0x2069)
                || (code >= 0x200b && code <= 0x200f) || code == 0xfeff) {
            return false;
        }
    }
    // 前后空白会让两个不同的名称在表格里看起来一致。
    return value.trimmed() == value;
}

// 摘要必须是完整的规范形式才能作为复核依据：截断或异常形式无法与任何内容对齐。
bool hashIdentity(const QString &value, const QString &prefix)
{
    return QRegularExpression(QStringLiteral("^%1[0-9a-f]{64}$")
        .arg(QRegularExpression::escape(prefix))).match(value).hasMatch();
}

bool validId(const QString &value)
{
    return QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
        .match(value).hasMatch();
}

// 展示用的短摘要同时保留头尾，因为只显示前缀会让构造出的前缀碰撞在屏幕上看起来一致。
QString fingerprint(const QString &identity)
{
    const QString hex = identity.section(QLatin1Char(':'), -1);
    if (hex.size() < 20) return hex;
    return hex.left(8) + QStringLiteral("…") + hex.right(8);
}

QString kindLabel(ExtensionKind kind)
{
    switch (kind) {
    case ExtensionKind::CodexPlugin: return QStringLiteral("Codex 插件");
    case ExtensionKind::Skill: return QStringLiteral("Skill");
    case ExtensionKind::Mcp: return QStringLiteral("MCP 服务器");
    }
    return QString();
}

// 只读产品不授予写入或执行类能力，因此请求它们必须被显式标记，而不是静默通过。
bool beyondReadOnly(const QString &capability)
{
    static const QSet<QString> denied{
        QStringLiteral("process"), QStringLiteral("command-execution"),
        QStringLiteral("git-mutation"), QStringLiteral("filesystem-write")};
    return denied.contains(capability) || capability.contains(QStringLiteral("write"))
        || capability.contains(QStringLiteral("exec"))
        || capability.contains(QStringLiteral("mutation"))
        || capability.contains(QStringLiteral("delete"));
}

// 名称与标识不一致时不能只显示名称：那正是冒充另一个扩展的方式。
bool nameAgreesWithIdentifier(const QString &name, const QString &id)
{
    const QString foldedName = name.toCaseFolded()
        .remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    const QString foldedId = id.toCaseFolded()
        .remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    if (foldedName.isEmpty() || foldedId.isEmpty()) return false;
    return foldedId.contains(foldedName) || foldedName.contains(foldedId);
}

ExtensionReviewPrompt reject(const QString &code)
{
    ExtensionReviewPrompt prompt;
    prompt.state = ExtensionReviewPromptState::Unpresentable;
    prompt.errorCode = code;
    return prompt;
}

} // namespace

ExtensionReviewPrompt ExtensionReviewPresentation::build(
    const ExtensionRegistryRecord &record, const QStringList &grantedCapabilities,
    bool previouslyReviewed, const QString &previousContentIdentity)
{
    if (!validId(record.id)) {
        return reject(QStringLiteral("extension-review-prompt-id-invalid"));
    }
    // 摘要不合法时无法呈现可批准的目标：人看到的摘要就是批准所绑定的内容。
    if (!hashIdentity(record.sourceIdentity,
                      QStringLiteral("extension-source:sha256:"))
            || !hashIdentity(record.contentIdentity,
                             QStringLiteral("extension-content:sha256:"))) {
        return reject(QStringLiteral("extension-review-prompt-identity-invalid"));
    }
    if (!record.installed) {
        return reject(QStringLiteral("extension-review-prompt-not-installed"));
    }
    // 名称超长或含有不可安全展示的字符时整体拒绝，而不是截断或清洗：截断会让两个
    // 不同的扩展在屏幕上看起来完全一样，清洗会让人看到一个并不存在的名称。
    if (!safeDisplayText(record.name, MaxTitleCharacters)) {
        return reject(QStringLiteral("extension-review-prompt-name-unsafe"));
    }
    if (!record.version.isEmpty()
            && !safeDisplayText(record.version, MaxVersionCharacters)) {
        return reject(QStringLiteral("extension-review-prompt-version-unsafe"));
    }
    if (!record.scope.isEmpty()
            && !safeDisplayText(record.scope, MaxTitleCharacters)) {
        return reject(QStringLiteral("extension-review-prompt-scope-unsafe"));
    }
    if (record.requestedCapabilities.size() > MaxCapabilities) {
        return reject(QStringLiteral("extension-review-prompt-capability-limit"));
    }
    for (const QString &capability : record.requestedCapabilities) {
        if (!safeDisplayText(capability, MaxCapabilityCharacters)) {
            return reject(QStringLiteral("extension-review-prompt-capability-unsafe"));
        }
    }
    // 请求集合里有重复项时，屏幕上的条数与实际请求不一致。
    if (QSet<QString>(record.requestedCapabilities.cbegin(),
                      record.requestedCapabilities.cend()).size()
            != record.requestedCapabilities.size()) {
        return reject(QStringLiteral("extension-review-prompt-capability-duplicate"));
    }
    // 声称已复核过却没有可比较的旧摘要时，无法判断这次是否是内容变更后的重新复核。
    if (previouslyReviewed
            && !hashIdentity(previousContentIdentity,
                             QStringLiteral("extension-content:sha256:"))) {
        return reject(QStringLiteral("extension-review-prompt-history-invalid"));
    }

    ExtensionReviewPrompt prompt;
    prompt.title = record.name;
    prompt.identifier = record.id;
    prompt.kindLabel = kindLabel(record.kind);
    prompt.versionLabel = record.version.isEmpty()
        ? QStringLiteral("未知") : record.version;
    prompt.scopeLabel = record.scope.isEmpty()
        ? QStringLiteral("未知") : record.scope;
    prompt.sourceIdentity = record.sourceIdentity;
    prompt.contentIdentity = record.contentIdentity;
    prompt.sourceFingerprint = fingerprint(record.sourceIdentity);
    prompt.contentFingerprint = fingerprint(record.contentIdentity);
    prompt.capabilities = record.requestedCapabilities;
    // 回传的摘要就是展示的摘要，因此复核流程能够检测出渲染之后发生的漂移。
    prompt.reviewedSourceIdentity = record.sourceIdentity;
    prompt.reviewedContentIdentity = record.contentIdentity;

    // 警告按固定顺序输出，避免排版顺序影响人的判断。
    if (!nameAgreesWithIdentifier(record.name, record.id)) {
        prompt.warnings.append(ExtensionReviewWarning::NameMismatchesIdentifier);
    }
    if (record.version.isEmpty()) {
        prompt.warnings.append(ExtensionReviewWarning::VersionUnknown);
    }
    const QSet<QString> granted(grantedCapabilities.cbegin(),
                                grantedCapabilities.cend());
    bool ungranted = false;
    bool writable = false;
    for (const QString &capability : record.requestedCapabilities) {
        if (!granted.contains(capability)) ungranted = true;
        if (beyondReadOnly(capability)) writable = true;
    }
    if (ungranted) {
        prompt.warnings.append(ExtensionReviewWarning::CapabilityNotGranted);
    }
    if (writable) {
        prompt.warnings.append(ExtensionReviewWarning::CapabilityBeyondReadOnly);
    }
    if (record.compatibility != ExtensionCompatibilityState::Compatible) {
        prompt.warnings.append(ExtensionReviewWarning::CompatibilityUnresolved);
    }
    if (previouslyReviewed && previousContentIdentity != record.contentIdentity) {
        prompt.warnings.append(ExtensionReviewWarning::ContentChangedSinceReview);
    }

    prompt.state = ExtensionReviewPromptState::Ready;
    return prompt;
}
