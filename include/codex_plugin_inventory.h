#ifndef CODEX_PLUGIN_INVENTORY_H
#define CODEX_PLUGIN_INVENTORY_H

#include "extension_registry.h"

#include <QByteArray>
#include <QList>
#include <QString>

enum class CodexPluginInventoryState {
    Ready,
    Invalid,
};

struct CodexPluginInventoryResult {
    CodexPluginInventoryState state = CodexPluginInventoryState::Invalid;
    QList<ExtensionRegistryRecord> records;
    QString sourceIdentity;
    QString errorCode;
};

class CodexPluginInventory
{
public:
    static constexpr qint64 MaxCapturedBytes = 1024 * 1024;
    static constexpr int MaxPlugins = ExtensionRegistry::MaxRecords;

    static CodexPluginInventoryResult inspectCapturedOutput(const QByteArray &bytes);
};

#endif // CODEX_PLUGIN_INVENTORY_H
