#include "extension_enablement_presentation.h"

#include "extension_display_safety.h"

#include <QSet>

namespace {

using Safety = ExtensionDisplaySafety;

ExtensionEnablementPrompt reject(const QString &code)
{
    ExtensionEnablementPrompt prompt;
    prompt.state = ExtensionEnablementPromptState::Unpresentable;
    prompt.errorCode = code;
    return prompt;
}

ExtensionRevocationPrompt rejectRevocation(const QString &code)
{
    ExtensionRevocationPrompt prompt;
    prompt.state = ExtensionRevocationPromptState::Unpresentable;
    prompt.errorCode = code;
    return prompt;
}

} // namespace

ExtensionEnablementPrompt ExtensionEnablementPresentation::build(
    const ExtensionRegistryRecord &record, const QStringList &grantedCapabilities,
    bool alreadyGranted, const QString &grantedContentIdentity)
{
    if (!Safety::validId(record.id)) {
        return reject(QStringLiteral("extension-enablement-prompt-id-invalid"));
    }
    // 摘要不合法时无法呈现可授权的目标：人看到的摘要就是授权所绑定的内容。
    if (!Safety::hashIdentity(record.sourceIdentity,
                              QStringLiteral("extension-source:sha256:"))
            || !Safety::hashIdentity(record.contentIdentity,
                                     QStringLiteral("extension-content:sha256:"))) {
        return reject(
            QStringLiteral("extension-enablement-prompt-identity-invalid"));
    }
    // 名称超长或含有不可安全展示的字符时整体拒绝，而不是截断或清洗：截断会让两个不同
    // 的扩展在屏幕上看起来完全一样，清洗会让人看到一个并不存在的名称。
    if (!Safety::safeDisplayText(record.name, MaxTitleCharacters)) {
        return reject(QStringLiteral("extension-enablement-prompt-name-unsafe"));
    }
    if (!record.version.isEmpty()
            && !Safety::safeDisplayText(record.version, MaxVersionCharacters)) {
        return reject(QStringLiteral("extension-enablement-prompt-version-unsafe"));
    }
    if (!record.scope.isEmpty()
            && !Safety::safeDisplayText(record.scope, MaxTitleCharacters)) {
        return reject(QStringLiteral("extension-enablement-prompt-scope-unsafe"));
    }
    if (record.requestedCapabilities.size() > MaxCapabilities) {
        return reject(
            QStringLiteral("extension-enablement-prompt-capability-limit"));
    }
    for (const QString &capability : record.requestedCapabilities) {
        if (!Safety::safeDisplayText(capability, MaxCapabilityCharacters)) {
            return reject(
                QStringLiteral("extension-enablement-prompt-capability-unsafe"));
        }
    }
    // 请求集合里有重复项时，屏幕上的条数与实际请求不一致。
    if (QSet<QString>(record.requestedCapabilities.cbegin(),
                      record.requestedCapabilities.cend()).size()
            != record.requestedCapabilities.size()) {
        return reject(
            QStringLiteral("extension-enablement-prompt-capability-duplicate"));
    }
    // 声称已有授权却没有可比较的摘要时，无法判断这次是否是内容变更后的重新授权。
    if (alreadyGranted
            && !Safety::hashIdentity(grantedContentIdentity,
                                     QStringLiteral("extension-content:sha256:"))) {
        return reject(QStringLiteral("extension-enablement-prompt-grant-invalid"));
    }

    ExtensionEnablementPrompt prompt;
    prompt.title = record.name;
    prompt.identifier = record.id;
    prompt.kind = record.kind;
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
    // 回传的摘要就是展示的摘要，因此启用流程能够检测出渲染之后发生的漂移。
    prompt.reviewedSourceIdentity = record.sourceIdentity;
    prompt.reviewedContentIdentity = record.contentIdentity;

    // 警告按固定顺序输出，避免排版顺序影响人的判断。
    if (!Safety::nameAgreesWithIdentifier(record.name, record.id)) {
        prompt.warnings.append(
            ExtensionEnablementWarning::NameMismatchesIdentifier);
    }
    if (record.version.isEmpty()) {
        prompt.warnings.append(ExtensionEnablementWarning::VersionUnknown);
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
        prompt.warnings.append(ExtensionEnablementWarning::CapabilityNotGranted);
    }
    if (writable) {
        prompt.warnings.append(
            ExtensionEnablementWarning::CapabilityBeyondReadOnly);
    }
    if (alreadyGranted && grantedContentIdentity != record.contentIdentity) {
        prompt.warnings.append(
            ExtensionEnablementWarning::ContentChangedSinceGrant);
    }
    if (alreadyGranted && grantedContentIdentity == record.contentIdentity) {
        prompt.warnings.append(ExtensionEnablementWarning::AlreadyGranted);
    }
    // 授权当前不会让任何内容运行。必须显式说明，否则人会以为自己刚刚开启了执行，
    // 而权限、审批、沙箱与恢复门禁尚未完成。
    prompt.warnings.append(
        ExtensionEnablementWarning::GrantDoesNotExecuteYet);

    // 三道门禁在提问之前就必须满足。未满足时呈现仍然完成——界面需要说明原因——但状态是
    // Blocked，不提供启用动作：一份此刻无法生效的授权会以已认证的形式留在账本里，等门禁
    // 出现的那一刻自动生效，也就是在为未来的内容预先授权。
    // 顺序固定为安装 -> 复核 -> 兼容：未安装时另外两项无从判断，而把"没人复核过"显示成
    // "当前主机装不下"会让人以为换台机器就能运行一份从未被人看过的内容。
    if (!record.installed) {
        prompt.state = ExtensionEnablementPromptState::Blocked;
        prompt.blockReason = ExtensionEnablementBlockReason::NotInstalled;
        return prompt;
    }
    if (record.trust != ExtensionTrustState::Verified) {
        prompt.state = ExtensionEnablementPromptState::Blocked;
        prompt.blockReason = ExtensionEnablementBlockReason::TrustMissing;
        return prompt;
    }
    if (record.compatibility != ExtensionCompatibilityState::Compatible) {
        prompt.state = ExtensionEnablementPromptState::Blocked;
        prompt.blockReason = ExtensionEnablementBlockReason::CompatibilityMissing;
        return prompt;
    }

    prompt.state = ExtensionEnablementPromptState::Ready;
    return prompt;
}

ExtensionRevocationPrompt ExtensionEnablementPresentation::buildRevocation(
    ExtensionKind kind, const QString &id, const ExtensionRegistryRecord *record)
{
    if (!Safety::validId(id)) {
        return rejectRevocation(
            QStringLiteral("extension-revocation-prompt-id-invalid"));
    }
    // 撤销不设门禁：内容漂移、复核被撤回、来源消失的扩展都必须仍然可以收回授权，
    // 否则一个被篡改的扩展将永远无法被撤销。
    ExtensionRevocationPrompt prompt;
    prompt.identifier = id;
    prompt.kindLabel = Safety::kindLabel(kind);
    if (!record) {
        // 来源已消失。撤销仍然进行，但界面必须说明目标已不存在，否则人会以为自己撤销的
        // 是屏幕上仍然列出的某一项。
        prompt.targetAbsent = true;
        prompt.title = id;
        prompt.state = ExtensionRevocationPromptState::Ready;
        return prompt;
    }
    // 名称来自不可信来源，因此撤销确认同样不能展示不安全的文本。名称不可展示时退回
    // 使用标识本身，而不是拒绝撤销：撤销必须始终可用。
    prompt.title = Safety::safeDisplayText(record->name, MaxTitleCharacters)
        ? record->name : id;
    prompt.state = ExtensionRevocationPromptState::Ready;
    return prompt;
}
