#include "extension_review_presentation.h"

#include "extension_display_safety.h"

#include <QSet>

namespace {

// 可展示性、摘要形式、短摘要与相似性判定都在 ExtensionDisplaySafety 里，因为启用授权
// 界面面对同一批不可信文本。两份副本会各自漂移，于是同一个扩展在两处呈现不同。
using Safety = ExtensionDisplaySafety;

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
    if (!Safety::validId(record.id)) {
        return reject(QStringLiteral("extension-review-prompt-id-invalid"));
    }
    // 摘要不合法时无法呈现可批准的目标：人看到的摘要就是批准所绑定的内容。
    if (!Safety::hashIdentity(record.sourceIdentity,
                      QStringLiteral("extension-source:sha256:"))
            || !Safety::hashIdentity(record.contentIdentity,
                             QStringLiteral("extension-content:sha256:"))) {
        return reject(QStringLiteral("extension-review-prompt-identity-invalid"));
    }
    if (!record.installed) {
        return reject(QStringLiteral("extension-review-prompt-not-installed"));
    }
    // 名称超长或含有不可安全展示的字符时整体拒绝，而不是截断或清洗：截断会让两个
    // 不同的扩展在屏幕上看起来完全一样，清洗会让人看到一个并不存在的名称。
    if (!Safety::safeDisplayText(record.name, MaxTitleCharacters)) {
        return reject(QStringLiteral("extension-review-prompt-name-unsafe"));
    }
    if (!record.version.isEmpty()
            && !Safety::safeDisplayText(record.version, MaxVersionCharacters)) {
        return reject(QStringLiteral("extension-review-prompt-version-unsafe"));
    }
    if (!record.scope.isEmpty()
            && !Safety::safeDisplayText(record.scope, MaxTitleCharacters)) {
        return reject(QStringLiteral("extension-review-prompt-scope-unsafe"));
    }
    if (record.requestedCapabilities.size() > MaxCapabilities) {
        return reject(QStringLiteral("extension-review-prompt-capability-limit"));
    }
    for (const QString &capability : record.requestedCapabilities) {
        if (!Safety::safeDisplayText(capability, MaxCapabilityCharacters)) {
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
            && !Safety::hashIdentity(previousContentIdentity,
                             QStringLiteral("extension-content:sha256:"))) {
        return reject(QStringLiteral("extension-review-prompt-history-invalid"));
    }

    ExtensionReviewPrompt prompt;
    prompt.title = record.name;
    prompt.identifier = record.id;
    prompt.kindLabel = Safety::kindLabel(record.kind);
    prompt.versionLabel = record.version.isEmpty()
        ? QStringLiteral("未知") : record.version;
    prompt.scopeLabel = record.scope.isEmpty()
        ? QStringLiteral("未知") : record.scope;
    prompt.sourceIdentity = record.sourceIdentity;
    prompt.contentIdentity = record.contentIdentity;
    prompt.sourceFingerprint = Safety::fingerprint(record.sourceIdentity);
    prompt.contentFingerprint = Safety::fingerprint(record.contentIdentity);
    prompt.capabilities = record.requestedCapabilities;
    // 回传的摘要就是展示的摘要，因此复核流程能够检测出渲染之后发生的漂移。
    prompt.reviewedSourceIdentity = record.sourceIdentity;
    prompt.reviewedContentIdentity = record.contentIdentity;

    // 警告按固定顺序输出，避免排版顺序影响人的判断。
    if (!Safety::nameAgreesWithIdentifier(record.name, record.id)) {
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
        if (Safety::beyondReadOnly(capability)) writable = true;
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
