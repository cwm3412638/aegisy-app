#ifndef EXTENSION_ENABLEMENT_POLICY_H
#define EXTENSION_ENABLEMENT_POLICY_H

#include "extension_registry.h"

#include <QList>
#include <QString>

// 生效启用是注册表 `Verified + Compatible` 门禁之外的那个独立动作。复核只回答
// "这份内容被人看过并认可"，兼容性只回答"当前宿主授权容得下它"，两者齐备仍然不等于
// 用户要求启用它。这一层把"用户确实要求启用这份内容"变成一个可判定的结论。
//
// 启用授权与复核记录一样绑定确切内容，而不是名字：种类、ID、来源身份与内容身份被
// 联合绑定。否则一个被替换过的扩展会直接继承前一份内容的启用授权，而替换内容正是
// 启用授权最需要防住的事情。
//
// 这一层只做判定。它不安装、不写盘、不执行任何东西，也不改写信任与兼容性判定：它
// 只写 `effectiveEnabled`，而注册表仍然独立地要求 `Verified + Compatible` 才接受
// 这个值，因此两道闸门互不依赖。请求写入或执行类能力的扩展在只读授权下确定不兼容，
// 因此永远无法通过这一层获得启用。
struct ExtensionEnablementGrant {
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    // 授权启用时确切的来源与内容摘要，格式与注册表一致。
    QString sourceIdentity;
    QString contentIdentity;
};

enum class ExtensionEnablementEvidence {
    // 从未被要求启用。
    NotGranted,
    // 授权过，但内容摘要已经变化：授权不能延续到新内容。
    ContentDrifted,
    // 授权过，但来源摘要已经变化：同名内容换了来源同样不能延续。
    SourceDrifted,
    // 同一扩展存在互相冲突的启用授权，无法判断哪一条有效。
    GrantConflict,
    // 启用授权本身格式不合法，不能作为依据。
    GrantMalformed,
    // 该扩展当前未安装，启用一个不存在的东西等于预先授权将来出现的内容。
    NotInstalled,
    // 尚未通过人工复核。
    TrustMissing,
    // 兼容性未确立或明确不兼容。
    CompatibilityMissing,
    // 授权与当前内容完全一致，且其余门禁齐备。
    GrantMatched,
};

struct ExtensionEnablementDecision {
    bool enabled = false;
    ExtensionEnablementEvidence evidence = ExtensionEnablementEvidence::NotGranted;
    // 固定诊断代码。注册表记录没有启用理由字段，因此这个值只用于诊断与测试，
    // 不会被写入记录。
    QString reason;
};

class ExtensionEnablementPolicy
{
public:
    static constexpr int MaxGrants = ExtensionRegistry::MaxRecords;

    static ExtensionEnablementDecision evaluate(
        const ExtensionRegistryRecord &record,
        const QList<ExtensionEnablementGrant> &grants);

    // 就地判定一批记录的生效启用状态。信任与兼容性判定一概不动，因此这一层必须在
    // 两者之后运行；提前运行时记录仍然保持默认的 Unverified/Unknown，结论因此是
    // 拒绝启用而不是错误授权。
    static void apply(QList<ExtensionRegistryRecord> *records,
                      const QList<ExtensionEnablementGrant> &grants);
};

#endif // EXTENSION_ENABLEMENT_POLICY_H
