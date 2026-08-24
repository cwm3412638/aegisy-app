#include "codex_plugin_inventory.h"
#include "strict_json_validator.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

namespace {

QByteArray framed(const QList<QByteArray> &parts)
{
    QByteArray result;
    for (const QByteArray &part : parts) {
        const quint64 size = static_cast<quint64>(part.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            result.append(static_cast<char>((size >> shift) & 0xff));
        }
        result.append(part);
    }
    return result;
}

QString identity(const QByteArray &domain, const QList<QByteArray> &parts,
                 const QString &prefix)
{
    QByteArray input = domain;
    input.append(framed(parts));
    return prefix + QString::fromLatin1(
        QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    return object.size() == expected.size()
        && QSet<QString>(keys.cbegin(), keys.cend()) == expected;
}

bool safeText(const QString &value, int maximum, bool allowEmpty = false)
{
    if ((!allowEmpty && value.isEmpty()) || value.size() > maximum) return false;
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character == QChar(0x7f)) return false;
    }
    const QString lowered = value.toLower();
    if (lowered.contains(QStringLiteral("authorization"))
            || lowered.contains(QStringLiteral("bearer "))
            || lowered.contains(QStringLiteral("api_key"))
            || lowered.contains(QStringLiteral("api-key"))
            || lowered.contains(QStringLiteral("access_token"))
            || lowered.contains(QStringLiteral("password"))
            || lowered.contains(QStringLiteral("credential="))
            || lowered.contains(QStringLiteral("secret="))
            || lowered.contains(QStringLiteral("token="))) {
        return false;
    }
    for (const QString &part : value.split(QRegularExpression(QStringLiteral("\\s+")))) {
        const QString loweredPart = part.toLower();
        if ((loweredPart.startsWith(QStringLiteral("sk-")) && part.size() >= 12)
                || (loweredPart.startsWith(QStringLiteral("ghp_"))
                    && part.size() >= 20)
                || (loweredPart.startsWith(QStringLiteral("github_pat_"))
                    && part.size() >= 24)
                || (loweredPart.startsWith(QStringLiteral("xoxb-"))
                    && part.size() >= 20)
                || (part.count(QLatin1Char('.')) == 2 && part.size() >= 24)) {
            return false;
        }
    }
    return true;
}

bool validPluginId(const QString &value)
{
    return QRegularExpression(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}$"))
        .match(value).hasMatch();
}

CodexPluginInventoryResult invalidResult(const QString &sourceIdentity,
                                         const QString &errorCode)
{
    CodexPluginInventoryResult result;
    result.sourceIdentity = sourceIdentity;
    result.errorCode = errorCode;
    return result;
}

bool appendGroup(const QJsonArray &items, bool expectedInstalled,
                 const QString &group, const QString &captureIdentity,
                 QSet<QString> *seenIds, QList<ExtensionRegistryRecord> *records,
                 QString *errorCode)
{
    static const QSet<QString> itemKeys{
        QStringLiteral("pluginId"), QStringLiteral("name"),
        QStringLiteral("marketplaceName"), QStringLiteral("version"),
        QStringLiteral("installed"), QStringLiteral("enabled"),
        QStringLiteral("source")};
    static const QSet<QString> sourceKeys{QStringLiteral("path")};

    for (const QJsonValue &value : items) {
        if (!value.isObject()) {
            *errorCode = QStringLiteral("codex-plugin-item-invalid");
            return false;
        }
        const QJsonObject item = value.toObject();
        if (!exactKeys(item, itemKeys)
                || !item.value(QStringLiteral("pluginId")).isString()
                || !item.value(QStringLiteral("name")).isString()
                || !item.value(QStringLiteral("marketplaceName")).isString()
                || !item.value(QStringLiteral("version")).isString()
                || !item.value(QStringLiteral("installed")).isBool()
                || !item.value(QStringLiteral("enabled")).isBool()
                || !item.value(QStringLiteral("source")).isObject()) {
            *errorCode = QStringLiteral("codex-plugin-item-invalid");
            return false;
        }

        const QString pluginId = item.value(QStringLiteral("pluginId")).toString();
        const QString name = item.value(QStringLiteral("name")).toString();
        const QString marketplace = item.value(QStringLiteral("marketplaceName")).toString();
        const QString version = item.value(QStringLiteral("version")).toString();
        const bool installed = item.value(QStringLiteral("installed")).toBool();
        const bool enabled = item.value(QStringLiteral("enabled")).toBool();
        const QJsonObject source = item.value(QStringLiteral("source")).toObject();
        if (!exactKeys(source, sourceKeys)
                || !source.value(QStringLiteral("path")).isString()) {
            *errorCode = QStringLiteral("codex-plugin-source-invalid");
            return false;
        }
        const QString sourcePath = source.value(QStringLiteral("path")).toString();
        if (!validPluginId(pluginId) || seenIds->contains(pluginId)
                || !safeText(name, 128) || !safeText(marketplace, 128, true)
                || !safeText(version, 64, true) || !safeText(sourcePath, 4096)
                || installed != expectedInstalled || (enabled && !installed)) {
            *errorCode = seenIds->contains(pluginId)
                ? QStringLiteral("codex-plugin-duplicate")
                : QStringLiteral("codex-plugin-metadata-invalid");
            return false;
        }
        seenIds->insert(pluginId);

        QJsonObject canonical{
            {QStringLiteral("group"), group},
            {QStringLiteral("plugin_id"), pluginId},
            {QStringLiteral("name"), name},
            {QStringLiteral("marketplace_name"), marketplace},
            {QStringLiteral("version"), version},
            {QStringLiteral("installed"), installed},
            {QStringLiteral("enabled"), enabled},
            {QStringLiteral("source_path"), sourcePath},
        };
        const QByteArray canonicalBytes =
            QJsonDocument(canonical).toJson(QJsonDocument::Compact);

        ExtensionRegistryRecord record;
        record.kind = ExtensionKind::CodexPlugin;
        record.id = pluginId;
        record.name = name;
        record.version = version;
        record.sourceKind = ExtensionSourceKind::CodexCli;
        record.sourceIdentity = identity(
            QByteArrayLiteral("aegisy-codex-plugin-extension-source/0.1\0"),
            {captureIdentity.toUtf8(), group.toUtf8(), pluginId.toUtf8()},
            QStringLiteral("extension-source:sha256:"));
        record.contentIdentity = identity(
            QByteArrayLiteral("aegisy-codex-plugin-extension-content/0.1\0"),
            {canonicalBytes}, QStringLiteral("extension-content:sha256:"));
        record.trust = ExtensionTrustState::Unverified;
        // 来源只报告事实。兼容性由 ExtensionCompatibilityPolicy 依据宿主证据判定，
        // 因此这里保持"未判定"，而不是让来源自己声明一个结论。
        record.compatibility = ExtensionCompatibilityState::Unknown;
        record.compatibilityReason =
            QStringLiteral("codex-plugin-compatibility-unevaluated");
        record.scope = QStringLiteral("user");
        record.installed = installed;
        record.effectiveEnabled = false;
        record.updateAvailable = false;
        record.recoveryAvailable = false;
        records->append(record);
    }
    return true;
}

} // namespace

CodexPluginInventoryResult CodexPluginInventory::inspectCapturedOutput(
    const QByteArray &bytes)
{
    if (bytes.isEmpty() || bytes.size() > MaxCapturedBytes
            || bytes.startsWith("\xef\xbb\xbf")) {
        return invalidResult({},
                             QStringLiteral("codex-plugin-output-invalid"));
    }
    const QString sourceIdentity = identity(
        QByteArrayLiteral("aegisy-codex-plugin-list-source/0.1\0"), {bytes},
        QStringLiteral("codex-plugin-list-source:sha256:"));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !StrictJsonValidator::accepts(bytes)) {
        return invalidResult(sourceIdentity,
                             QStringLiteral("codex-plugin-json-invalid"));
    }

    const QJsonObject root = document.object();
    static const QSet<QString> rootKeys{
        QStringLiteral("installed"), QStringLiteral("available")};
    if (!exactKeys(root, rootKeys)
            || !root.value(QStringLiteral("installed")).isArray()
            || !root.value(QStringLiteral("available")).isArray()) {
        return invalidResult(sourceIdentity,
                             QStringLiteral("codex-plugin-root-invalid"));
    }

    const QJsonArray installed = root.value(QStringLiteral("installed")).toArray();
    const QJsonArray available = root.value(QStringLiteral("available")).toArray();
    if (installed.size() > MaxPlugins || available.size() > MaxPlugins
            || installed.size() + available.size() > MaxPlugins) {
        return invalidResult(sourceIdentity,
                             QStringLiteral("codex-plugin-count-invalid"));
    }

    QList<ExtensionRegistryRecord> records;
    records.reserve(installed.size() + available.size());
    QSet<QString> seenIds;
    QString errorCode;
    if (!appendGroup(installed, true, QStringLiteral("installed"), sourceIdentity,
                     &seenIds, &records, &errorCode)
            || !appendGroup(available, false, QStringLiteral("available"),
                            sourceIdentity, &seenIds, &records, &errorCode)) {
        return invalidResult(sourceIdentity, errorCode);
    }

    CodexPluginInventoryResult result;
    result.state = CodexPluginInventoryState::Ready;
    result.records = records;
    result.sourceIdentity = sourceIdentity;
    return result;
}
