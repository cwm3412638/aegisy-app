#ifndef EXTENSION_TRUST_POLICY_H
#define EXTENSION_TRUST_POLICY_H

#include "extension_registry.h"

#include <QList>
#include <QString>

// 信任只能来自一次针对确切内容的人工复核，绝不能来自扩展的自我声明。复核记录把
// 种类、ID、来源身份与内容身份联合绑定：任何一项变化都让复核失效，因为被复核的
// 不是"这个名字"，而是"这份内容"。
//
// 这一层只做判定。它不安装、不启用、不执行、不持久化任何东西：判定 Verified 只是
// 满足注册表启用门禁的一半，另一半是 ExtensionCompatibilityPolicy 给出的
// Compatible，两者齐备也仍然需要一个独立的启用动作。
struct ExtensionReviewPin {
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    // 复核时确切的来源与内容摘要，格式与注册表一致。
    QString sourceIdentity;
    QString contentIdentity;
};

enum class ExtensionTrustEvidence {
    // 从未复核过。
    Unreviewed,
    // 复核过，但内容摘要已经变化：复核结论不能延续到新内容。
    ContentDrifted,
    // 复核过，但来源摘要已经变化：同名内容换了来源同样不能延续。
    SourceDrifted,
    // 同一扩展存在互相冲突的复核记录，无法判断哪一条有效。
    ReviewConflict,
    // 复核记录本身格式不合法，不能作为证据。
    ReviewMalformed,
    // 复核记录与当前内容完全一致。
    ReviewMatched,
};

struct ExtensionTrustDecision {
    ExtensionTrustState state = ExtensionTrustState::Unverified;
    ExtensionTrustEvidence evidence = ExtensionTrustEvidence::Unreviewed;
    // 固定诊断代码。注册表记录没有信任理由字段，因此这个值只用于诊断与测试，
    // 不会被写入记录。
    QString reason;
};

class ExtensionTrustPolicy
{
public:
    static constexpr int MaxReviewPins = ExtensionRegistry::MaxRecords;

    static ExtensionTrustDecision evaluate(const ExtensionRegistryRecord &record,
                                           const QList<ExtensionReviewPin> &pins);

    // 就地判定一批记录的信任状态。兼容性与启用状态一概不动。
    static void apply(QList<ExtensionRegistryRecord> *records,
                      const QList<ExtensionReviewPin> &pins);
};

#endif // EXTENSION_TRUST_POLICY_H
