#ifndef EXTENSION_REGISTRY_H
#define EXTENSION_REGISTRY_H

#include <QJsonObject>
#include <QStringList>

enum class ExtensionKind {
    CodexPlugin,
    Skill,
    Mcp,
};

enum class ExtensionSourceKind {
    BuiltIn,
    LocalDirectory,
    CodexCli,
    ToolConfiguration,
};

enum class ExtensionTrustState {
    Verified,
    Unverified,
};

enum class ExtensionCompatibilityState {
    Compatible,
    Unknown,
    Incompatible,
};

struct ExtensionRegistryRecord {
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    QString name;
    QString version;
    ExtensionSourceKind sourceKind = ExtensionSourceKind::LocalDirectory;
    QString sourceIdentity;
    QString contentIdentity;
    ExtensionTrustState trust = ExtensionTrustState::Unverified;
    ExtensionCompatibilityState compatibility = ExtensionCompatibilityState::Unknown;
    QString compatibilityReason;
    QString scope;
    QStringList requestedCapabilities;
    bool installed = false;
    bool effectiveEnabled = false;
    bool updateAvailable = false;
    bool recoveryAvailable = false;
};

struct ExtensionRegistryProjection {
    QJsonObject object;
    QString identity;
};

class ExtensionRegistry
{
public:
    static constexpr int MaxRecords = 512;
    static bool build(const QList<ExtensionRegistryRecord> &records,
                      ExtensionRegistryProjection *projection,
                      QString *errorCode = nullptr);
};

#endif // EXTENSION_REGISTRY_H
