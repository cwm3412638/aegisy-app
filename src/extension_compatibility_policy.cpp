#include "extension_compatibility_policy.h"

#include <QRegularExpression>
#include <QSet>

namespace {

// 判定顺序是固定的：确定不兼容 -> 证据不足 -> 兼容。任何顺序调整都会把一个
// 确定的拒绝降级成"未知"，因此 product_scope_policy 固定了这个顺序。

// 与 ToolManager::extractVersion 的产出保持一致：至少 major.minor，最多四段，
// 允许预发布/构建后缀。"unknown"、"dev"、空串一律读不出版本。
bool readableRelease(const QString &value)
{
    return QRegularExpression(
               QStringLiteral("^\\d{1,9}(?:\\.\\d{1,9}){1,3}(?:[-+][0-9A-Za-z.-]{1,64})?$"))
        .match(value).hasMatch();
}

ExtensionCompatibilityDecision verdict(ExtensionCompatibilityState state,
                                       const QString &reason)
{
    ExtensionCompatibilityDecision decision;
    decision.state = state;
    // 注册表要求 Compatible 的理由为空、非 Compatible 的理由非空。
    decision.reason = state == ExtensionCompatibilityState::Compatible
        ? QString() : reason;
    return decision;
}

} // namespace

QStringList ExtensionCompatibilityPolicy::defaultGrantedCapabilities()
{
    // `process` 与任何写入能力都不在其中：Agent/Codex 在权限、审批、沙箱与恢复
    // 门禁完成前保持只读，所以请求进程执行的扩展在当前授权下确定不兼容。
    return {QStringLiteral("filesystem-read"), QStringLiteral("mcp-tools"),
            QStringLiteral("network"), QStringLiteral("skill-content")};
}

ExtensionCompatibilityDecision ExtensionCompatibilityPolicy::evaluate(
    const ExtensionRegistryRecord &record,
    const ExtensionHostProfile &host)
{
    const QSet<QString> granted(host.grantedCapabilities.cbegin(),
                                host.grantedCapabilities.cend());
    for (const QString &capability : record.requestedCapabilities) {
        if (!granted.contains(capability)) {
            return verdict(ExtensionCompatibilityState::Incompatible,
                           QStringLiteral("extension-capability-not-granted"));
        }
    }

    // 扩展自己声明的版本不能作为兼容证据，但读不出来的版本说明清单本身不可核查。
    if (!record.version.isEmpty() && !readableRelease(record.version)) {
        return verdict(ExtensionCompatibilityState::Unknown,
                       QStringLiteral("extension-version-unreadable"));
    }

    if (record.kind == ExtensionKind::CodexPlugin) {
        // Codex 插件运行在 Codex CLI 内部，宿主版本是唯一的宿主侧证据。本仓库
        // 没有任何已核查的最低 Codex 版本，因此这里不编造下限：证据缺失或读不出
        // 只能得出"未知"，绝不能因为"看起来没问题"而判定兼容。
        if (host.codexVersion.isEmpty()) {
            return verdict(ExtensionCompatibilityState::Unknown,
                           QStringLiteral("codex-plugin-host-version-unknown"));
        }
        if (!readableRelease(host.codexVersion)) {
            return verdict(ExtensionCompatibilityState::Unknown,
                           QStringLiteral("codex-plugin-host-version-unreadable"));
        }
        if (record.version.isEmpty()) {
            return verdict(ExtensionCompatibilityState::Unknown,
                           QStringLiteral("codex-plugin-version-missing"));
        }
    }

    return verdict(ExtensionCompatibilityState::Compatible, QString());
}

void ExtensionCompatibilityPolicy::apply(QList<ExtensionRegistryRecord> *records,
                                         const ExtensionHostProfile &host)
{
    if (!records) return;
    for (ExtensionRegistryRecord &record : *records) {
        const ExtensionCompatibilityDecision decision = evaluate(record, host);
        // 只写兼容性结论。信任状态与生效启用状态一概不动：判定兼容不等于授权，
        // 启用仍然需要注册表要求的 Verified + Compatible。
        record.compatibility = decision.state;
        record.compatibilityReason = decision.reason;
    }
}
