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
};

#endif // SKILL_EXTENSION_INVENTORY_H
