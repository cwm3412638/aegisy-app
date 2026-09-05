#ifndef SKILL_EXTENSION_INVENTORY_H
#define SKILL_EXTENSION_INVENTORY_H

#include "extension_registry.h"
#include "extension_tree_capture.h"

#include <QList>
#include <QString>

enum class SkillExtensionInventoryState {
    Empty,
    Ready,
    Invalid,
    Unavailable,
};

struct SkillExtensionInventoryResult {
    SkillExtensionInventoryState state = SkillExtensionInventoryState::Invalid;
    QList<ExtensionRegistryRecord> records;
    QString sourceIdentity;
    QString errorCode;
};

class SkillExtensionInventory
{
public:
    static constexpr int MaxSkills = 128;
    static constexpr int MaxEntries = ExtensionTreeCapture::MaxEntries;
    static constexpr int MaxDepth = ExtensionTreeCapture::MaxDepth;
    static constexpr qint64 MaxManifestBytes = 64 * 1024;
    static constexpr qint64 MaxFileBytes = ExtensionTreeCapture::MaxFileBytes;
    static constexpr qint64 MaxTotalBytes = ExtensionTreeCapture::MaxTotalBytes;

    static SkillExtensionInventoryResult inspectRoot(const QString &rootPath);

    // 技能扩展树的捕获域。身份域字节与诊断前缀已随历史摘要与诊断串发布，逐字节固定。
    // 暂存备份捕获等其他层必须经这个访问器复用同一份域，而不是复制第二份常量：两份
    // 副本会各自漂移，而树身份正是之后一切授权与审计决定绑定的对象。
    static const ExtensionTreeCaptureDomain &treeCaptureDomain();
};

#endif // SKILL_EXTENSION_INVENTORY_H
