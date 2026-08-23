#ifndef EXTENSION_INVENTORY_COORDINATOR_H
#define EXTENSION_INVENTORY_COORDINATOR_H

#include "extension_registry.h"

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
