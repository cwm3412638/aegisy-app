#ifndef EXTENSION_UPDATE_POLICY_H
#define EXTENSION_UPDATE_POLICY_H

#include "extension_registry.h"
#include "extension_trust_policy.h"

#include <QList>
#include <QString>

// 更新与移除的可逆性判定。更新是内容绑定信任最危险的时刻:当前版本已经被人复核过,
// 可能还持有一份授权;而候选版本按定义是**另一份内容**。如果信任或授权按 ID 或名字
// 传递,那么"更新"就成了让任意新内容以上一版权威运行的通道——而内容绑定身份存在的
// 全部理由正是防住这件事。
//
// 因此这一层的核心结论只有一个形状:候选版本永远从未复核、未授权开始。校验通过只
// 意味着它可以被暂存,不意味着它可以运行。校验失败时当前版本保持不变,候选不执行。
//
// 移除是相反方向的同一条要求:可执行内容被停用或删除,但身份元数据必须保留,否则
// 一次移除会把"这份内容曾经被授权运行过"的历史一并抹掉,使事后审计无从进行。
//
// 这一层不安装、不下载、不写盘、不执行任何东西。它只判定一次更新或移除的结论。
struct ExtensionUpdateCandidate {
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    QString version;
    // 候选自己的来源与内容摘要,格式与注册表一致。
    QString sourceIdentity;
    QString contentIdentity;
    QStringList requestedCapabilities;
};

// 候选必须通过的校验。任何一项失败都让当前版本保持不变,并且候选不得执行。
struct ExtensionUpdateEvidence {
    bool signatureValid = false;
    bool manifestValid = false;
    bool compatible = false;
    bool dependenciesSatisfied = false;
    bool healthy = false;
};

enum class ExtensionUpdateState {
    // 校验或身份检查失败。当前版本不变,候选不执行。
    Rejected,
    // 校验通过。候选可以被暂存,但仍然是未复核、未授权的新内容。
    StagedUnreviewed,
};

struct ExtensionUpdateVerdict {
    ExtensionUpdateState state = ExtensionUpdateState::Rejected;
    // 恒为真:无论结论如何,当前生效的版本都不被这一层改动。
    bool activePreserved = true;
    // 恒为假:候选在被独立复核并授权之前不可运行。
    bool candidateExecutable = false;
    // 恒为假:信任绑定确切内容,不随版本号或标识传递。
    bool inheritsTrust = false;
    // 恒为假:授权同样绑定确切内容。更新后必须重新授权。
    bool inheritsGrant = false;
    // 候选相对当前版本是降级。降级不被禁止,但必须可见:它会重新引入已修复内容。
    bool downgrade = false;
    QString errorCode;
};

enum class ExtensionRemovalState {
    Rejected,
    Ready,
};

struct ExtensionRemovalVerdict {
    ExtensionRemovalState state = ExtensionRemovalState::Rejected;
    // 恒为真:可执行内容被停用或删除。
    bool removesExecutableContent = false;
    // 恒为真:身份元数据保留,否则一次移除会抹掉"这份内容曾被授权运行"的历史。
    bool retainsIdentityMetadata = false;
    // 恒为假:移除必须收回授权。留下授权会让同名内容重新出现时直接继承它。
    bool retainsGrant = false;
    // 被保留下来的不可变身份:种类、ID 与内容摘要。
    QString retainedIdentity;
    QString errorCode;
};

class ExtensionUpdatePolicy
{
public:
    // active 是当前生效的记录,candidate 是待应用的新内容。
    static ExtensionUpdateVerdict evaluate(
        const ExtensionRegistryRecord &active,
        const ExtensionUpdateCandidate &candidate,
        const ExtensionUpdateEvidence &evidence);

    // 移除判定。record 为空指针表示目标已经不存在,此时仍然必须能够收回授权并留下历史。
    static ExtensionRemovalVerdict evaluateRemoval(
        ExtensionKind kind, const QString &id,
        const ExtensionRegistryRecord *record);

    // 候选内容是否可以继承一份针对当前内容的复核记录。恒为假,除非两者内容摘要完全
    // 相同——而那不是一次更新。
    static bool reviewTransfers(const ExtensionReviewPin &pin,
                                const ExtensionUpdateCandidate &candidate);
};

#endif // EXTENSION_UPDATE_POLICY_H
