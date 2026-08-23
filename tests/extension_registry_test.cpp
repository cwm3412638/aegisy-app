#include "extension_registry.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) QTextStream(stderr) << message << Qt::endl;
    return condition;
}

QString identity(const QString &prefix, QChar fill)
{
    return prefix + QString(64, fill);
}

ExtensionRegistryRecord record(ExtensionKind kind, const QString &id, QChar fill)
{
    ExtensionRegistryRecord value;
    value.kind = kind;
    value.id = id;
    value.name = id;
    value.version = QStringLiteral("1.0.0");
    value.sourceKind = kind == ExtensionKind::CodexPlugin
        ? ExtensionSourceKind::CodexCli
        : (kind == ExtensionKind::Mcp
            ? ExtensionSourceKind::ToolConfiguration
            : ExtensionSourceKind::LocalDirectory);
    value.sourceIdentity = identity(QStringLiteral("extension-source:sha256:"), fill);
    value.contentIdentity = identity(
        QStringLiteral("extension-content:sha256:"), fill);
    value.compatibilityReason = QStringLiteral("compatibility-unverified");
    value.scope = QStringLiteral("user");
    value.installed = true;
    return value;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QList<ExtensionRegistryRecord> records{
        record(ExtensionKind::CodexPlugin, QStringLiteral("plugin.one"), QLatin1Char('a')),
        record(ExtensionKind::Skill, QStringLiteral("skill.one"), QLatin1Char('b')),
        record(ExtensionKind::Mcp, QStringLiteral("mcp.one"), QLatin1Char('c')),
    };
    records[0].requestedCapabilities = {QStringLiteral("network")};
    records[1].requestedCapabilities = {QStringLiteral("skill-content")};
    records[2].requestedCapabilities = {QStringLiteral("mcp-tools")};
    ExtensionRegistryProjection projection;
    QString error;
    if (!expect(ExtensionRegistry::build(records, &projection, &error),
                "valid registry build failed")) return 1;
    const QJsonArray output = projection.object.value(QStringLiteral("records")).toArray();
    if (!expect(output.size() == 3
                    && projection.identity.startsWith(
                        QStringLiteral("extension-registry:sha256:")),
                "registry projection is incomplete")) return 1;
    for (const QJsonValue &value : output) {
        const QJsonObject item = value.toObject();
        if (!expect(!item.value(QStringLiteral("install_authority")).toBool(true)
                        && !item.value(QStringLiteral("enable_authority")).toBool(true)
                        && !item.value(QStringLiteral("update_authority")).toBool(true)
                        && !item.value(QStringLiteral("remove_authority")).toBool(true)
                        && !item.value(QStringLiteral("execution_authority")).toBool(true)
                        && !item.value(QStringLiteral("effective_enabled")).toBool(true),
                    "registry granted unavailable authority")) return 1;
    }

    QList<ExtensionRegistryRecord> duplicate = records;
    duplicate.append(records.first());
    if (!expect(!ExtensionRegistry::build(duplicate, &projection, &error),
                "duplicate identity was accepted")) return 1;
    ExtensionRegistryRecord secret = records.first();
    secret.name = QStringLiteral("token=private-value");
    if (!expect(!ExtensionRegistry::build({secret}, &projection, &error),
                "secret-shaped metadata was accepted")) return 1;
    ExtensionRegistryRecord forgedEnabled = records.first();
    forgedEnabled.effectiveEnabled = true;
    if (!expect(!ExtensionRegistry::build({forgedEnabled}, &projection, &error),
                "unverified extension was effectively enabled")) return 1;
    ExtensionRegistryRecord badCapability = records.first();
    badCapability.requestedCapabilities = {QStringLiteral("shell-execute")};
    if (!expect(!ExtensionRegistry::build({badCapability}, &projection, &error),
                "unknown capability was accepted")) return 1;
    ExtensionRegistryRecord duplicateCapability = records.first();
    duplicateCapability.requestedCapabilities = {
        QStringLiteral("network"), QStringLiteral("network")};
    if (!expect(!ExtensionRegistry::build(
                    {duplicateCapability}, &projection, &error),
                "duplicate capability was accepted")) return 1;
    QList<ExtensionRegistryRecord> oversized;
    for (int i = 0; i <= ExtensionRegistry::MaxRecords; ++i) {
        ExtensionRegistryRecord item = record(
            ExtensionKind::Skill, QStringLiteral("skill.%1").arg(i), QLatin1Char('d'));
        item.contentIdentity = identity(
            QStringLiteral("extension-content:sha256:"),
            QChar(QLatin1Char('a').unicode() + (i % 6)));
        oversized.append(item);
    }
    if (!expect(!ExtensionRegistry::build(oversized, &projection, &error),
                "registry record limit was not enforced")) return 1;
    ExtensionRegistryRecord compatible = records.first();
    compatible.trust = ExtensionTrustState::Verified;
    compatible.compatibility = ExtensionCompatibilityState::Compatible;
    compatible.compatibilityReason.clear();
    compatible.effectiveEnabled = true;
    if (!expect(ExtensionRegistry::build({compatible}, &projection, &error),
                "verified compatible enabled metadata was rejected")) return 1;
    return 0;
}
