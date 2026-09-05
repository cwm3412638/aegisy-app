#ifndef MCP_CONFIGURATION_INVENTORY_H
#define MCP_CONFIGURATION_INVENTORY_H

#include "extension_registry.h"
#include "extension_tree_capture.h"

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

    // MCP 设置文件备份的树捕获域。`mcp:` 主体的诚实备份单元是整个设置文件的原始字节
    // （文件同时是来源身份与变更的单位，且被该文件里所有 `mcp:` 主体共享），因此备份以
    // 一棵合成单条目树（固定相对路径 `settings.json`）进入共享快照契约，身份按本域摘要。
    // 这是一个全新的身份：本域与下面的 `aegisy-mcp-config-source/0.1\0` 刻意不同——
    // sourceIdentity 摘要的是"带存在性标记的原始字节"，而备份树身份摘要的是"快照清单形状
    // 下的一棵树"，两者输入帧不同，绝不应被声称相等。诊断前缀 `mcp-backup` 与既有的
    // `mcp-config-*` 及 `extension-staging-capture-*` 均不冲突。
    static const ExtensionTreeCaptureDomain &backupCaptureDomain();

    static McpConfigurationInventoryResult inspectFile(const QString &path);
    static McpConfigurationInventoryResult inspectBytes(
        const QByteArray &bytes, bool sourceExists = true);
};

#endif // MCP_CONFIGURATION_INVENTORY_H
