#ifndef EXTENSION_DISPLAY_SAFETY_H
#define EXTENSION_DISPLAY_SAFETY_H

#include "extension_registry.h"

#include <QString>

// 授权界面的共享呈现安全层。复核提问"有人看过这份内容吗"，启用提问"你要让这份内容
// 运行吗"，但两者面对的是同一个危险：扩展的名称、版本与能力清单全部来自不可信的磁盘
// 来源，而人只能依据屏幕上的字符做决定。因此判定"这段文本能否安全展示"的规则必须只有
// 一份——两份副本会各自漂移，而漂移意味着一个界面接受了另一个界面拒绝的双向覆盖字符，
// 于是同一个扩展在两处呈现不同。
//
// 这一层只判定可展示性与相似性。它不批准、不授权、不持久化、不执行任何东西。
class ExtensionDisplaySafety
{
public:
    // 除制表符外的控制字符、格式字符、代理码位与未分配码位都不能进入授权界面：它们
    // 可以让屏幕上的文本与实际字符串不一致，从而让人授权一个自己没有看到的扩展。
    // 超长输入被拒绝而不是截断，因为截断会让两个不同的扩展看起来完全一样。
    static bool safeDisplayText(const QString &value, int maximum);

    // 摘要必须是完整的规范形式才能作为授权依据：截断或异常形式无法与任何内容对齐。
    static bool hashIdentity(const QString &value, const QString &prefix);

    static bool validId(const QString &value);

    // 展示用的短摘要同时保留头尾，因为只显示前缀会让构造出的前缀碰撞在屏幕上看起来
    // 一致。它仅用于展示，授权始终绑定完整摘要。
    static QString fingerprint(const QString &identity);

    static QString kindLabel(ExtensionKind kind);

    // 只读产品不授予写入或执行类能力，因此请求它们必须被显式标记，而不是静默通过。
    static bool beyondReadOnly(const QString &capability);

    // 名称与标识不一致时不能只显示名称：那正是冒充另一个扩展的方式。
    static bool nameAgreesWithIdentifier(const QString &name, const QString &id);
};

#endif // EXTENSION_DISPLAY_SAFETY_H
