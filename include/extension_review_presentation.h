#ifndef EXTENSION_REVIEW_PRESENTATION_H
#define EXTENSION_REVIEW_PRESENTATION_H

#include "extension_registry.h"

#include <QList>
#include <QString>

// 人工复核的结论只能和呈现给人的内容一样可靠。扩展的名称、版本与能力清单全部来自
// 不可信的磁盘来源，因此这一层把它们变成可以安全展示的字符串，并且拒绝任何可能让
// 人在屏幕上看到与实际批准对象不一致内容的输入。
//
// 这一层只做呈现。它不批准、不持久化、不判定信任、不授予启用：它的唯一职责是让
// ExtensionReviewWorkflow 收到的复核请求确实对应人所看到的那份内容。
enum class ExtensionReviewPromptState {
    // 可以展示并允许人做出决定。
    Ready,
    // 内容无法安全展示，因此不能被批准。
    Unpresentable,
};

// 需要在复核界面上明确标记的风险，按固定顺序输出，避免"看起来没问题"的排版。
enum class ExtensionReviewWarning {
    // 名称与实际标识不一致，可能是在冒充另一个扩展。
    NameMismatchesIdentifier,
    // 版本缺失，因此无法判断复核的是哪一版。
    VersionUnknown,
    // 请求了超出授予集合的能力。
    CapabilityNotGranted,
    // 请求了写入或执行类能力，而当前产品保持只读。
    CapabilityBeyondReadOnly,
    // 兼容性证据不足或明确不兼容。
    CompatibilityUnresolved,
    // 该扩展之前已被复核，本次是内容变更后的重新复核。
    ContentChangedSinceReview,
};

struct ExtensionReviewPrompt {
    ExtensionReviewPromptState state = ExtensionReviewPromptState::Unpresentable;
    // 全部为可安全展示的文本，长度有界。
    QString title;
    QString identifier;
    QString kindLabel;
    QString versionLabel;
    QString scopeLabel;
    QString sourceIdentity;
    QString contentIdentity;
    // 缩短后的摘要，仅用于展示；批准仍然使用完整摘要。
    QString sourceFingerprint;
    QString contentFingerprint;
    QStringList capabilities;
    QList<ExtensionReviewWarning> warnings;
    // 人在屏幕上看到的确切摘要，必须原样回传给复核流程。
    QString reviewedSourceIdentity;
    QString reviewedContentIdentity;
    QString errorCode;
};

class ExtensionReviewPresentation
{
public:
    // 展示文本的上界。超出上界的输入被拒绝而不是截断：截断会让两个不同的扩展在
    // 屏幕上看起来完全一样。
    static constexpr int MaxTitleCharacters = 128;
    static constexpr int MaxVersionCharacters = 64;
    static constexpr int MaxCapabilityCharacters = 64;
    static constexpr int MaxCapabilities = 32;

    static ExtensionReviewPrompt build(const ExtensionRegistryRecord &record,
                                       const QStringList &grantedCapabilities,
                                       bool previouslyReviewed,
                                       const QString &previousContentIdentity);
};

#endif // EXTENSION_REVIEW_PRESENTATION_H
