#ifndef MCP_CONFIGURATION_INVENTORY_H
#define MCP_CONFIGURATION_INVENTORY_H

#include "extension_registry.h"

#include <QJsonObject>

enum class McpConfigurationInventoryState {
    Empty,
    Ready,
    Invalid,
    Unavailable,
};

struct McpConfigurationInventoryResult {
    McpConfigurationInventoryState state = McpConfigurationInventoryState::Invalid;
    QJsonObject root;
    QList<ExtensionRegistryRecord> records;
    QString sourceIdentity;
    QString errorCode;
};

class McpConfigurationInventory
{
public:
    static constexpr qint64 MaxFileBytes = 1024 * 1024;
    static constexpr int MaxServers = 128;

    static McpConfigurationInventoryResult inspectFile(const QString &path);
    static McpConfigurationInventoryResult inspectBytes(
        const QByteArray &bytes, bool sourceExists = true);
};

#endif // MCP_CONFIGURATION_INVENTORY_H
