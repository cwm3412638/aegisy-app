#include "extension_registry.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

QString kindName(ExtensionKind kind)
{
    switch (kind) {
    case ExtensionKind::CodexPlugin: return QStringLiteral("codex-plugin");
    case ExtensionKind::Skill: return QStringLiteral("skill");
    case ExtensionKind::Mcp: return QStringLiteral("mcp");
    }
    return {};
}

QString sourceName(ExtensionSourceKind source)
{
    switch (source) {
    case ExtensionSourceKind::BuiltIn: return QStringLiteral("built-in");
    case ExtensionSourceKind::LocalDirectory: return QStringLiteral("local-directory");
    case ExtensionSourceKind::CodexCli: return QStringLiteral("codex-cli");
    case ExtensionSourceKind::ToolConfiguration:
        return QStringLiteral("tool-configuration");
    }
    return {};
}

QString trustName(ExtensionTrustState trust)
{
    return trust == ExtensionTrustState::Verified
        ? QStringLiteral("verified") : QStringLiteral("unverified");
}

QString compatibilityName(ExtensionCompatibilityState compatibility)
{
    switch (compatibility) {
    case ExtensionCompatibilityState::Compatible: return QStringLiteral("compatible");
    case ExtensionCompatibilityState::Unknown: return QStringLiteral("unknown");
    case ExtensionCompatibilityState::Incompatible:
        return QStringLiteral("incompatible");
    }
    return {};
}

bool safeText(const QString &value, int maximum, bool allowEmpty = false)
{
    if ((!allowEmpty && value.isEmpty()) || value.size() > maximum) return false;
    const QString lowered = value.toLower();
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character == QChar(0x7f)) return false;
    }
    return !lowered.contains(QStringLiteral("authorization"))
        && !lowered.contains(QStringLiteral("api_key"))
        && !lowered.contains(QStringLiteral("api-key"))
        && !lowered.contains(QStringLiteral("password"))
        && !lowered.contains(QStringLiteral("secret="))
        && !lowered.contains(QStringLiteral("token="))
        && !QRegularExpression(QStringLiteral("(^|[^a-z0-9])sk-[a-z0-9_-]{8,}"),
                               QRegularExpression::CaseInsensitiveOption)
                .match(value).hasMatch();
}

bool hashIdentity(const QString &value, const QString &prefix)
{
    return QRegularExpression(QStringLiteral("^%1[0-9a-f]{64}$")
        .arg(QRegularExpression::escape(prefix))).match(value).hasMatch();
}

bool fixedCode(const QString &value, bool allowEmpty = false)
{
    if (allowEmpty && value.isEmpty()) return true;
    return QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9-]{0,95}$"))
        .match(value).hasMatch();
}

} // namespace

bool ExtensionRegistry::build(
    const QList<ExtensionRegistryRecord> &records,
    ExtensionRegistryProjection *projection,
    QString *errorCode)
{
    if (projection) *projection = ExtensionRegistryProjection();
    if (!projection || records.size() > MaxRecords) {
        if (errorCode) *errorCode = QStringLiteral("extension-registry-limit-invalid");
        return false;
    }
    QList<ExtensionRegistryRecord> sorted = records;
    std::sort(sorted.begin(), sorted.end(), [](const auto &left, const auto &right) {
        const QString leftKey = kindName(left.kind) + QLatin1Char(':') + left.id;
        const QString rightKey = kindName(right.kind) + QLatin1Char(':') + right.id;
        return leftKey < rightKey;
    });
    QSet<QString> identities;
    QJsonArray output;
    const QSet<QString> allowedCapabilities{
        QStringLiteral("filesystem-read"), QStringLiteral("network"),
        QStringLiteral("process"), QStringLiteral("mcp-tools"),
        QStringLiteral("skill-content")};
    for (const ExtensionRegistryRecord &record : sorted) {
        const QString kind = kindName(record.kind);
        const QString source = sourceName(record.sourceKind);
        const QString compatibility = compatibilityName(record.compatibility);
        const bool trustValid = record.trust == ExtensionTrustState::Verified
            || record.trust == ExtensionTrustState::Unverified;
        const QString recordKey = kind + QLatin1Char(':') + record.id;
        const bool enabledAllowed = record.trust == ExtensionTrustState::Verified
            && record.compatibility == ExtensionCompatibilityState::Compatible;
        if (kind.isEmpty() || source.isEmpty() || compatibility.isEmpty() || !trustValid
                || !QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
                    .match(record.id).hasMatch()
                || identities.contains(recordKey)
                || !safeText(record.name, 128)
                || !safeText(record.version, 64, true)
                || !hashIdentity(record.sourceIdentity,
                                 QStringLiteral("extension-source:sha256:"))
                || !hashIdentity(record.contentIdentity,
                                 QStringLiteral("extension-content:sha256:"))
                || !fixedCode(record.compatibilityReason, true)
                || (record.compatibility == ExtensionCompatibilityState::Compatible
                    && !record.compatibilityReason.isEmpty())
                || (record.compatibility != ExtensionCompatibilityState::Compatible
                    && record.compatibilityReason.isEmpty())
                || (record.scope != QStringLiteral("user")
                    && record.scope != QStringLiteral("built-in"))
                || (record.effectiveEnabled && !enabledAllowed)
                || record.requestedCapabilities.size() > 16) {
            if (errorCode) *errorCode = QStringLiteral("extension-registry-record-invalid");
            return false;
        }
        QStringList sortedCapabilities = record.requestedCapabilities;
        std::sort(sortedCapabilities.begin(), sortedCapabilities.end());
        QSet<QString> capabilities;
        QJsonArray capabilityArray;
        for (const QString &capability : sortedCapabilities) {
            if (!allowedCapabilities.contains(capability)
                    || capabilities.contains(capability)) {
                if (errorCode) {
                    *errorCode = QStringLiteral("extension-registry-capability-invalid");
                }
                return false;
            }
            capabilities.insert(capability);
            capabilityArray.append(capability);
        }
        identities.insert(recordKey);
        output.append(QJsonObject{
            {QStringLiteral("kind"), kind},
            {QStringLiteral("id"), record.id},
            {QStringLiteral("name"), record.name},
            {QStringLiteral("version"), record.version},
            {QStringLiteral("source_kind"), source},
            {QStringLiteral("source_identity"), record.sourceIdentity},
            {QStringLiteral("content_identity"), record.contentIdentity},
            {QStringLiteral("trust"), trustName(record.trust)},
            {QStringLiteral("compatibility"), compatibility},
            {QStringLiteral("compatibility_reason"), record.compatibilityReason},
            {QStringLiteral("scope"), record.scope},
            {QStringLiteral("requested_capabilities"), capabilityArray},
            {QStringLiteral("installed"), record.installed},
            {QStringLiteral("effective_enabled"), record.effectiveEnabled},
            {QStringLiteral("update_available"), record.updateAvailable},
            {QStringLiteral("recovery_available"), record.recoveryAvailable},
            {QStringLiteral("install_authority"), false},
            {QStringLiteral("enable_authority"), false},
            {QStringLiteral("update_authority"), false},
            {QStringLiteral("remove_authority"), false},
            {QStringLiteral("execution_authority"), false},
        });
    }
    QJsonObject object{
        {QStringLiteral("schema"), QStringLiteral("extension-registry/0.1")},
        {QStringLiteral("records"), output},
        {QStringLiteral("install_authority"), false},
        {QStringLiteral("enable_authority"), false},
        {QStringLiteral("update_authority"), false},
        {QStringLiteral("remove_authority"), false},
        {QStringLiteral("execution_authority"), false},
    };
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > 1024 * 1024) {
        if (errorCode) *errorCode = QStringLiteral("extension-registry-bytes-invalid");
        return false;
    }
    const QString identity = QStringLiteral("extension-registry:sha256:%1").arg(
        QString::fromLatin1(QCryptographicHash::hash(
            bytes, QCryptographicHash::Sha256).toHex()));
    object.insert(QStringLiteral("identity"), identity);
    projection->object = object;
    projection->identity = identity;
    return true;
}
