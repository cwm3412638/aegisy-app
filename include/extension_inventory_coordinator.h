#ifndef EXTENSION_INVENTORY_COORDINATOR_H
#define EXTENSION_INVENTORY_COORDINATOR_H

#include "extension_compatibility_policy.h"
#include "extension_registry.h"
#include "extension_trust_policy.h"

#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

struct ExtensionInventoryInputs {
    QString codexExecutable;
    QProcessEnvironment sourceEnvironment;
    QString skillsRoot;
    QString mcpConfigurationPath;
    int codexTimeoutMs = 15000;
    // 宿主侧兼容性证据。各来源只报告事实，兼容性由协调器统一判定，因此这里为空
    // 表示证据缺失，Codex 插件只能得出"未知"。
    ExtensionHostProfile host;
    // 人工复核记录。为空表示没有任何扩展被复核过，因此所有记录保持 Unverified。
    QList<ExtensionReviewPin> reviewPins;
};

struct ExtensionInventorySnapshot {
    QList<ExtensionRegistryRecord> records;
    QStringList sourceIssueCodes;
    QString registryIdentity;
    bool registryValid = false;
};

class ExtensionInventoryCoordinator
{
public:
    static constexpr qint64 MaxCodexStderrBytes = 64 * 1024;

    static ExtensionInventorySnapshot collect(
        const ExtensionInventoryInputs &inputs);
    static QProcessEnvironment scrubbedEnvironment(
        const QProcessEnvironment &source);
};

#endif // EXTENSION_INVENTORY_COORDINATOR_H
