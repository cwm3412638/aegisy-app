#ifndef EXTENSION_COMPATIBILITY_POLICY_H
#define EXTENSION_COMPATIBILITY_POLICY_H

#include "extension_registry.h"

#include <QList>
#include <QString>
#include <QStringList>

// 兼容性必须来自可核查的证据，绝不能由扩展自我声明。宿主授予的能力集合是产品
// 决定的固定事实；扩展请求的能力来自已经严格解析过的清单。两者的差集是一个可以
// 判定的结论，而缺失的宿主证据只能得出"未知"，不能得出"兼容"。
//
// 这一层只做判定，不安装、不启用、不执行、不改变任何授权：`effectiveEnabled`
// 仍然由注册表要求 Verified + Compatible 才允许，而各来源一律不自我声明信任。
struct ExtensionHostProfile {
    // 空字符串表示宿主版本证据缺失，Codex 插件因此只能得出"未知"。
    QString codexVersion;
    // 宿主当前实际授予的能力。读写文件与进程执行不在其中，因为 Agent/Codex
    // 仍然是只读的；请求它们的扩展在当前授权下确定不兼容。
    QStringList grantedCapabilities;
};

struct ExtensionCompatibilityDecision {
    ExtensionCompatibilityState state = ExtensionCompatibilityState::Unknown;
    QString reason;
};

class ExtensionCompatibilityPolicy
{
public:
    // 当前只读授权下的固定授予集合。
    static QStringList defaultGrantedCapabilities();

    static ExtensionCompatibilityDecision evaluate(
        const ExtensionRegistryRecord &record,
        const ExtensionHostProfile &host);

    // 就地判定一批记录。信任状态与启用状态一概不动。
    static void apply(QList<ExtensionRegistryRecord> *records,
                      const ExtensionHostProfile &host);
};

#endif // EXTENSION_COMPATIBILITY_POLICY_H
