#ifndef EXTENSION_ENABLEMENT_PRESENTATION_H
#define EXTENSION_ENABLEMENT_PRESENTATION_H

#include "extension_registry.h"

#include <QList>
#include <QString>

// 启用授权的结论只能和呈现给人的内容一样可靠，因此启用请求也需要一层不可欺骗的呈现。
// 它与复核呈现回答的是两个不同的问题：复核问"有人看过这份内容吗"，启用问"你要让这份
// 内容运行吗"。后者是更强的授权，因此它额外要求三道门禁在提问之前就已经满足——未复核、
// 不兼容或未安装的扩展根本不该出现可点击的启用动作，否则界面会邀请人去授权一件当前无法
// 被授权的事，而那份授权会以已认证的形式留在账本里，等门禁出现的那一刻自动生效。
//
// 这一层只做呈现。它不授予启用、不持久化、不判定信任、不执行任何东西。
enum class ExtensionEnablementPromptState {
    // 三道门禁均已满足，可以展示并允许人做出启用决定。
    Ready,
    // 可以安全展示，但当前不允许启用：界面必须说明原因而不是提供动作。
    Blocked,
    // 内容无法安全展示，因此既不能启用也不该显示。
    Unpresentable,
};

// Blocked 的确切原因。诊断必须区分"没人复核过"与"当前主机装不下"：把前者显示成后者
// 会让人以为换台机器就能运行一份从未被人看过的内容。
enum class ExtensionEnablementBlockReason {
    None,
    NotInstalled,
    TrustMissing,
    CompatibilityMissing,
};

// 需要在启用界面上明确标记的风险，按固定顺序输出，避免"看起来没问题"的排版。
enum class ExtensionEnablementWarning {
    // 名称与实际标识不一致，可能是在冒充另一个已被授权的扩展。
    NameMismatchesIdentifier,
    // 版本缺失，因此无法判断授权的是哪一版。
    VersionUnknown,
    // 请求了超出授予集合的能力。
    CapabilityNotGranted,
    // 请求了写入或执行类能力，而当前产品保持只读。
    CapabilityBeyondReadOnly,
    // 已经存在一份授权，本次是内容变更后的重新授权。
    ContentChangedSinceGrant,
    // 已经存在一份等效授权，本次不会改变任何状态。
    AlreadyGranted,
    // 授权被记录，但当前不会让任何内容运行：权限、审批、沙箱与恢复门禁尚未完成。
    // 必须显式说明，否则人会以为自己刚刚开启了执行。
    GrantDoesNotExecuteYet,
};

struct ExtensionEnablementPrompt {
    ExtensionEnablementPromptState state =
        ExtensionEnablementPromptState::Unpresentable;
    ExtensionEnablementBlockReason blockReason =
        ExtensionEnablementBlockReason::None;
    // 全部为可安全展示的文本，长度有界。
    QString title;
    QString identifier;
    QString kindLabel;
    QString versionLabel;
    QString scopeLabel;
    QString sourceIdentity;
    QString contentIdentity;
    // 缩短后的摘要，仅用于展示；授权仍然使用完整摘要。
    QString sourceFingerprint;
    QString contentFingerprint;
    QStringList capabilities;
    QList<ExtensionEnablementWarning> warnings;
    // 人在屏幕上看到的确切摘要，必须原样回传给启用流程。
    QString reviewedSourceIdentity;
    QString reviewedContentIdentity;
    QString errorCode;
};

// 撤销授权的确认。撤销永远允许：内容漂移、复核被撤回、来源消失的扩展都必须仍然可以
// 收回授权，否则一个被篡改的扩展将永远无法被撤销。因此这里没有门禁，只有安全展示。
enum class ExtensionRevocationPromptState {
    Ready,
    Unpresentable,
};

struct ExtensionRevocationPrompt {
    ExtensionRevocationPromptState state =
        ExtensionRevocationPromptState::Unpresentable;
    QString title;
    QString identifier;
    QString kindLabel;
    // 来源已消失时为真：撤销仍然进行，但界面必须说明撤销的是一份不再存在的目标。
    bool targetAbsent = false;
    QString errorCode;
};

class ExtensionEnablementPresentation
{
public:
    // 展示文本的上界。超出上界的输入被拒绝而不是截断：截断会让两个不同的扩展在屏幕上
    // 看起来完全一样。
    static constexpr int MaxTitleCharacters = 128;
    static constexpr int MaxVersionCharacters = 64;
    static constexpr int MaxCapabilityCharacters = 64;
    static constexpr int MaxCapabilities = 32;

    // grantedCapabilities 是当前主机授予的能力集合，用于标记越界请求。
    // alreadyGranted / grantedContentIdentity 描述账本里已有的等效授权。
    static ExtensionEnablementPrompt build(
        const ExtensionRegistryRecord &record,
        const QStringList &grantedCapabilities,
        bool alreadyGranted,
        const QString &grantedContentIdentity);

    // 撤销确认。record 为空指针表示来源已经消失。
    static ExtensionRevocationPrompt buildRevocation(
        ExtensionKind kind, const QString &id,
        const ExtensionRegistryRecord *record);
};

#endif // EXTENSION_ENABLEMENT_PRESENTATION_H
