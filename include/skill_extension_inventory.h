#ifndef SKILL_EXTENSION_INVENTORY_H
#define SKILL_EXTENSION_INVENTORY_H

#include "extension_registry.h"

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
    static constexpr int MaxEntries = 4096;
    static constexpr int MaxDepth = 16;
    static constexpr qint64 MaxManifestBytes = 64 * 1024;
    static constexpr qint64 MaxFileBytes = 2 * 1024 * 1024;
    static constexpr qint64 MaxTotalBytes = 16 * 1024 * 1024;

    static SkillExtensionInventoryResult inspectRoot(const QString &rootPath);
};

#endif // SKILL_EXTENSION_INVENTORY_H
