#include "companion_key_management_projection.h"

#include "companion_config_projection.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace {

constexpr int kMaximumKeys = 1000;
constexpr int kMaximumGroups = 1000;
constexpr int kMaximumDisplayBytes = 128;
constexpr int kMaximumDateBytes = 64;
constexpr int kMaximumProjectionBytes = 1024 * 1024;
const char kSchemaVersion[] =
    "aegisy-companion-key-management-projection/0.1";
const char kKeyUpdateHandlePrefix[] = "website-key-update:opaque:";
const char kKeyDeleteHandlePrefix[] = "website-key-delete:opaque:";
const char kKeyTestHandlePrefix[] = "website-key-test:opaque:";
const char kGroupManagementHandlePrefix[] =
    "website-group-management:opaque:";
const char kGroupCreateHandlePrefix[] = "website-group-create:opaque:";

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend()) == expected;
}

bool validLowerHex256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a')
                     && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool validPrefixedHandle(const QString &value, const QString &prefix)
{
    return value.startsWith(prefix)
        && validLowerHex256(value.mid(prefix.size()));
}

QString handleToken(const QString &value, const QString &prefix)
{
    return validPrefixedHandle(value, prefix) ? value.mid(prefix.size()) : QString();
}

bool validKeyIdentity(const QString &value)
{
    return validPrefixedHandle(value, QStringLiteral("website-key:sha256:"));
}

bool validAccountIdentity(const QString &value)
{
    return validPrefixedHandle(
        value, QStringLiteral("website-account-session:sha256:"));
}

bool safeDisplayText(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    if (value.isEmpty() || utf8.size() > kMaximumDisplayBytes) return false;
    for (const QChar character : value) {
        const QChar::Category category = character.category();
        if (character.isNull() || category == QChar::Other_Control
                || category == QChar::Other_Surrogate) {
            return false;
        }
    }
    const QString lower = value.toLower();
    if (lower.contains(QStringLiteral("bearer "))
            || lower.contains(QStringLiteral("api_key="))
            || lower.contains(QStringLiteral("api-key="))
            || lower.contains(QStringLiteral("access_token="))) {
        return false;
    }
    for (const QString &part : value.split(
         QRegularExpression(QStringLiteral("\\s+")))) {
        const QString lowerPart = part.toLower();
        if ((lowerPart.startsWith(QStringLiteral("sk-")) && part.size() >= 12)
                || (lowerPart.startsWith(QStringLiteral("ghp_")) && part.size() >= 20)
                || (part.count(QLatin1Char('.')) == 2 && part.size() >= 24)) {
            return false;
        }
    }
    return true;
}

bool validPlatform(const QString &value)
{
    return QSet<QString>{QStringLiteral("openai"), QStringLiteral("anthropic"),
                         QStringLiteral("gemini"), QStringLiteral("unknown")}
        .contains(value);
}

bool validState(const QString &value)
{
    return QSet<QString>{QStringLiteral("active"), QStringLiteral("inactive"),
                         QStringLiteral("expired"), QStringLiteral("unknown")}
        .contains(value);
}

bool validMetric(const QJsonValue &value)
{
    return value.isDouble() && std::isfinite(value.toDouble())
        && value.toDouble() >= 0.0
        && value.toDouble() <= 9007199254740991.0;
}

bool validDate(const QJsonValue &value, QDateTime *parsed = nullptr)
{
    if (value.isNull()) {
        if (parsed) *parsed = QDateTime();
        return true;
    }
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty() || text.toUtf8().size() > kMaximumDateBytes) return false;
    for (const QChar character : text) {
        if (character.isNull() || character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Surrogate) {
            return false;
        }
    }
    QDateTime date = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!date.isValid()) date = QDateTime::fromString(text, Qt::ISODate);
    if (!date.isValid()) return false;
    if (parsed) *parsed = date;
    return true;
}

bool validDateRange(const QJsonValue &createdValue,
                    const QJsonValue &expiresValue)
{
    QDateTime created;
    QDateTime expires;
    if (!validDate(createdValue, &created) || !validDate(expiresValue, &expires)) {
        return false;
    }
    return !created.isValid() || !expires.isValid()
        || expires.toMSecsSinceEpoch() >= created.toMSecsSinceEpoch();
}

QString digest(QJsonObject projection)
{
    projection.remove(QStringLiteral("projection_sha256"));
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(projection).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

} // namespace

QJsonObject CompanionKeyManagementProjection::fromConfiguration(
    const QJsonObject &configurationProjection,
    const QHash<QString, QJsonObject> &metadataByKeyIdentity,
    const QJsonArray &groups,
    QString *errorCode)
{
    if (!CompanionConfigProjection::validate(configurationProjection)) {
        fail(errorCode, QStringLiteral("key-management-configuration-invalid"));
        return {};
    }
    if (groups.size() > kMaximumGroups) {
        fail(errorCode, QStringLiteral("key-management-group-limit-exceeded"));
        return {};
    }

    static const QSet<QString> metadataKeys{
        QStringLiteral("update_handle"), QStringLiteral("delete_handle"),
        QStringLiteral("test_handle"), QStringLiteral("group_handle"),
        QStringLiteral("quota"), QStringLiteral("quota_used"),
        QStringLiteral("created_at"), QStringLiteral("expires_at"),
    };
    static const QSet<QString> groupKeys{
        QStringLiteral("group_handle"), QStringLiteral("display_name"),
        QStringLiteral("platform"), QStringLiteral("create_handle"),
    };

    QJsonArray projectedGroups;
    QHash<QString, QJsonObject> groupsByHandle;
    QSet<QString> handleTokens;
    for (const QJsonValue &value : groups) {
        if (!value.isObject()) {
            fail(errorCode, QStringLiteral("key-management-group-invalid"));
            return {};
        }
        const QJsonObject group = value.toObject();
        const QString handle = group.value(QStringLiteral("group_handle")).toString();
        const QString createHandle = group.value(
            QStringLiteral("create_handle")).toString();
        const QString groupToken = handleToken(
            handle, QString::fromLatin1(kGroupManagementHandlePrefix));
        const QString createToken = handleToken(
            createHandle, QString::fromLatin1(kGroupCreateHandlePrefix));
        if (!exactKeys(group, groupKeys)
                || groupToken.isEmpty() || createToken.isEmpty()
                || groupsByHandle.contains(handle)
                || handleTokens.contains(groupToken)
                || handleTokens.contains(createToken)
                || groupToken == createToken
                || !safeDisplayText(group.value(
                    QStringLiteral("display_name")).toString())
                || !validPlatform(group.value(
                    QStringLiteral("platform")).toString())) {
            fail(errorCode, QStringLiteral("key-management-group-invalid"));
            return {};
        }
        groupsByHandle.insert(handle, group);
        handleTokens.insert(groupToken);
        handleTokens.insert(createToken);
        projectedGroups.append(group);
    }

    QJsonArray projectedKeys;
    QSet<QString> expectedIdentities;
    const QJsonArray candidates = configurationProjection.value(
        QStringLiteral("keys")).toArray();
    if (candidates.size() > kMaximumKeys
            || metadataByKeyIdentity.size() != candidates.size()) {
        fail(errorCode, QStringLiteral("key-management-key-count-invalid"));
        return {};
    }
    for (const QJsonValue &value : candidates) {
        const QJsonObject candidate = value.toObject();
        const QString keyIdentity = candidate.value(
            QStringLiteral("key_identity")).toString();
        expectedIdentities.insert(keyIdentity);
        const auto metadataIt = metadataByKeyIdentity.constFind(keyIdentity);
        if (metadataIt == metadataByKeyIdentity.cend()
                || !exactKeys(*metadataIt, metadataKeys)) {
            fail(errorCode, QStringLiteral("key-management-metadata-invalid"));
            return {};
        }
        const QJsonObject metadata = *metadataIt;
        const QString updateHandle = metadata.value(
            QStringLiteral("update_handle")).toString();
        const QString deleteHandle = metadata.value(
            QStringLiteral("delete_handle")).toString();
        const QString testHandle = metadata.value(
            QStringLiteral("test_handle")).toString();
        const QString updateToken = handleToken(
            updateHandle, QString::fromLatin1(kKeyUpdateHandlePrefix));
        const QString deleteToken = handleToken(
            deleteHandle, QString::fromLatin1(kKeyDeleteHandlePrefix));
        const QString testToken = handleToken(
            testHandle, QString::fromLatin1(kKeyTestHandlePrefix));
        const QJsonValue groupHandleValue = metadata.value(
            QStringLiteral("group_handle"));
        if (updateToken.isEmpty() || deleteToken.isEmpty() || testToken.isEmpty()
                || handleTokens.contains(updateToken)
                || handleTokens.contains(deleteToken)
                || handleTokens.contains(testToken)
                || updateToken == deleteToken || updateToken == testToken
                || deleteToken == testToken
                || !(groupHandleValue.isNull() || groupHandleValue.isString())
                || !validMetric(metadata.value(QStringLiteral("quota")))
                || !validMetric(metadata.value(QStringLiteral("quota_used")))
                || !validDateRange(metadata.value(QStringLiteral("created_at")),
                                   metadata.value(QStringLiteral("expires_at")))) {
            fail(errorCode, QStringLiteral("key-management-metadata-invalid"));
            return {};
        }
        if (groupHandleValue.isString()) {
            const QString groupHandle = groupHandleValue.toString();
            const auto groupIt = groupsByHandle.constFind(groupHandle);
            if (!validPrefixedHandle(
                    groupHandle,
                    QString::fromLatin1(kGroupManagementHandlePrefix))
                    || groupIt == groupsByHandle.cend()
                    || groupIt->value(QStringLiteral("display_name"))
                        != candidate.value(QStringLiteral("group_label"))
                    || groupIt->value(QStringLiteral("platform"))
                        != candidate.value(QStringLiteral("platform"))) {
                fail(errorCode, QStringLiteral("key-management-group-binding-invalid"));
                return {};
            }
        }
        handleTokens.insert(updateToken);
        handleTokens.insert(deleteToken);
        handleTokens.insert(testToken);
        projectedKeys.append(QJsonObject{
            { QStringLiteral("key_identity"), keyIdentity },
            { QStringLiteral("update_handle"), updateHandle },
            { QStringLiteral("delete_handle"), deleteHandle },
            { QStringLiteral("test_handle"), testHandle },
            { QStringLiteral("group_handle"), groupHandleValue },
            { QStringLiteral("display_name"), candidate.value(
                QStringLiteral("display_name")) },
            { QStringLiteral("group_label"), candidate.value(
                QStringLiteral("group_label")) },
            { QStringLiteral("platform"), candidate.value(
                QStringLiteral("platform")) },
            { QStringLiteral("state"), candidate.value(QStringLiteral("state")) },
            { QStringLiteral("quota"), metadata.value(QStringLiteral("quota")) },
            { QStringLiteral("quota_used"), metadata.value(
                QStringLiteral("quota_used")) },
            { QStringLiteral("created_at"), metadata.value(
                QStringLiteral("created_at")) },
            { QStringLiteral("expires_at"), metadata.value(
                QStringLiteral("expires_at")) },
        });
    }
    for (auto it = metadataByKeyIdentity.cbegin();
         it != metadataByKeyIdentity.cend(); ++it) {
        if (!expectedIdentities.contains(it.key())) {
            fail(errorCode, QStringLiteral("key-management-metadata-key-invalid"));
            return {};
        }
    }

    QJsonObject projection{
        { QStringLiteral("schema_version"), QString::fromLatin1(kSchemaVersion) },
        { QStringLiteral("account_identity"), configurationProjection.value(
            QStringLiteral("account_identity")) },
        { QStringLiteral("configuration_projection_sha256"),
          configurationProjection.value(QStringLiteral("projection_sha256")) },
        { QStringLiteral("key_count"), projectedKeys.size() },
        { QStringLiteral("keys"), projectedKeys },
        { QStringLiteral("group_count"), projectedGroups.size() },
        { QStringLiteral("groups"), projectedGroups },
        { QStringLiteral("raw_key_ids_included"), false },
        { QStringLiteral("raw_group_ids_included"), false },
        { QStringLiteral("credential_values_included"), false },
        { QStringLiteral("credential_fragments_included"), false },
        { QStringLiteral("configuration_authority"), false },
        { QStringLiteral("mutation_authority"), false },
    };
    projection.insert(QStringLiteral("projection_sha256"), digest(projection));
    if (!validate(projection, errorCode)) return {};
    if (errorCode) errorCode->clear();
    return projection;
}

bool CompanionKeyManagementProjection::validate(
    const QJsonObject &projection, QString *errorCode)
{
    static const QSet<QString> projectionKeys{
        QStringLiteral("schema_version"), QStringLiteral("account_identity"),
        QStringLiteral("configuration_projection_sha256"),
        QStringLiteral("key_count"), QStringLiteral("keys"),
        QStringLiteral("group_count"), QStringLiteral("groups"),
        QStringLiteral("raw_key_ids_included"),
        QStringLiteral("raw_group_ids_included"),
        QStringLiteral("credential_values_included"),
        QStringLiteral("credential_fragments_included"),
        QStringLiteral("configuration_authority"),
        QStringLiteral("mutation_authority"),
        QStringLiteral("projection_sha256"),
    };
    static const QSet<QString> keyKeys{
        QStringLiteral("key_identity"), QStringLiteral("update_handle"),
        QStringLiteral("delete_handle"), QStringLiteral("test_handle"),
        QStringLiteral("group_handle"), QStringLiteral("display_name"),
        QStringLiteral("group_label"), QStringLiteral("platform"),
        QStringLiteral("state"), QStringLiteral("quota"),
        QStringLiteral("quota_used"), QStringLiteral("created_at"),
        QStringLiteral("expires_at"),
    };
    static const QSet<QString> groupKeys{
        QStringLiteral("group_handle"), QStringLiteral("display_name"),
        QStringLiteral("platform"), QStringLiteral("create_handle"),
    };

    const QJsonValue keysValue = projection.value(QStringLiteral("keys"));
    const QJsonValue groupsValue = projection.value(QStringLiteral("groups"));
    const QJsonArray keys = keysValue.toArray();
    const QJsonArray groups = groupsValue.toArray();
    const QByteArray encoded = QJsonDocument(projection).toJson(
        QJsonDocument::Compact);
    if (!exactKeys(projection, projectionKeys)
            || encoded.isEmpty() || encoded.size() > kMaximumProjectionBytes
            || projection.value(QStringLiteral("schema_version")).toString()
                != QString::fromLatin1(kSchemaVersion)
            || !validAccountIdentity(projection.value(
                QStringLiteral("account_identity")).toString())
            || !validLowerHex256(projection.value(
                QStringLiteral("configuration_projection_sha256")).toString())
            || !keysValue.isArray() || keys.size() > kMaximumKeys
            || !projection.value(QStringLiteral("key_count")).isDouble()
            || std::floor(projection.value(QStringLiteral("key_count")).toDouble())
                != projection.value(QStringLiteral("key_count")).toDouble()
            || projection.value(QStringLiteral("key_count")).toInt(-1)
                != keys.size()
            || !groupsValue.isArray() || groups.size() > kMaximumGroups
            || !projection.value(QStringLiteral("group_count")).isDouble()
            || std::floor(projection.value(QStringLiteral("group_count")).toDouble())
                != projection.value(QStringLiteral("group_count")).toDouble()
            || projection.value(QStringLiteral("group_count")).toInt(-1)
                != groups.size()
            || projection.value(QStringLiteral("raw_key_ids_included"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("raw_group_ids_included"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("credential_values_included"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("credential_fragments_included"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("configuration_authority"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("mutation_authority"))
                != QJsonValue(false)
            || !validLowerHex256(projection.value(
                QStringLiteral("projection_sha256")).toString())
            || projection.value(QStringLiteral("projection_sha256")).toString()
                != digest(projection)) {
        fail(errorCode, QStringLiteral("key-management-projection-invalid"));
        return false;
    }

    QHash<QString, QJsonObject> groupsByHandle;
    QSet<QString> handleTokens;
    for (const QJsonValue &value : groups) {
        const QJsonObject group = value.toObject();
        const QString handle = group.value(QStringLiteral("group_handle")).toString();
        const QString createHandle = group.value(
            QStringLiteral("create_handle")).toString();
        const QString groupToken = handleToken(
            handle, QString::fromLatin1(kGroupManagementHandlePrefix));
        const QString createToken = handleToken(
            createHandle, QString::fromLatin1(kGroupCreateHandlePrefix));
        if (!value.isObject() || !exactKeys(group, groupKeys)
                || groupToken.isEmpty() || createToken.isEmpty()
                || groupsByHandle.contains(handle)
                || handleTokens.contains(groupToken)
                || handleTokens.contains(createToken)
                || groupToken == createToken
                || !safeDisplayText(group.value(
                    QStringLiteral("display_name")).toString())
                || !validPlatform(group.value(
                    QStringLiteral("platform")).toString())) {
            fail(errorCode, QStringLiteral("key-management-group-invalid"));
            return false;
        }
        groupsByHandle.insert(handle, group);
        handleTokens.insert(groupToken);
        handleTokens.insert(createToken);
    }

    QSet<QString> keyIdentities;
    for (const QJsonValue &value : keys) {
        const QJsonObject key = value.toObject();
        const QString keyIdentity = key.value(
            QStringLiteral("key_identity")).toString();
        const QString updateHandle = key.value(
            QStringLiteral("update_handle")).toString();
        const QString deleteHandle = key.value(
            QStringLiteral("delete_handle")).toString();
        const QString testHandle = key.value(
            QStringLiteral("test_handle")).toString();
        const QString updateToken = handleToken(
            updateHandle, QString::fromLatin1(kKeyUpdateHandlePrefix));
        const QString deleteToken = handleToken(
            deleteHandle, QString::fromLatin1(kKeyDeleteHandlePrefix));
        const QString testToken = handleToken(
            testHandle, QString::fromLatin1(kKeyTestHandlePrefix));
        const QJsonValue groupHandleValue = key.value(
            QStringLiteral("group_handle"));
        if (!value.isObject() || !exactKeys(key, keyKeys)
                || !validKeyIdentity(keyIdentity)
                || keyIdentities.contains(keyIdentity)
                || updateToken.isEmpty() || deleteToken.isEmpty() || testToken.isEmpty()
                || handleTokens.contains(updateToken)
                || handleTokens.contains(deleteToken)
                || handleTokens.contains(testToken)
                || updateToken == deleteToken || updateToken == testToken
                || deleteToken == testToken
                || !(groupHandleValue.isNull() || groupHandleValue.isString())
                || !safeDisplayText(key.value(
                    QStringLiteral("display_name")).toString())
                || !safeDisplayText(key.value(
                    QStringLiteral("group_label")).toString())
                || !validPlatform(key.value(
                    QStringLiteral("platform")).toString())
                || !validState(key.value(QStringLiteral("state")).toString())
                || !validMetric(key.value(QStringLiteral("quota")))
                || !validMetric(key.value(QStringLiteral("quota_used")))
                || !validDateRange(key.value(QStringLiteral("created_at")),
                                   key.value(QStringLiteral("expires_at")))) {
            fail(errorCode, QStringLiteral("key-management-key-invalid"));
            return false;
        }
        if (groupHandleValue.isString()) {
            const QString handle = groupHandleValue.toString();
            const auto groupIt = groupsByHandle.constFind(handle);
            if (!validPrefixedHandle(
                    handle, QString::fromLatin1(kGroupManagementHandlePrefix))
                    || groupIt == groupsByHandle.cend()
                    || groupIt->value(QStringLiteral("display_name"))
                        != key.value(QStringLiteral("group_label"))
                    || groupIt->value(QStringLiteral("platform"))
                        != key.value(QStringLiteral("platform"))) {
                fail(errorCode, QStringLiteral("key-management-group-binding-invalid"));
                return false;
            }
        }
        keyIdentities.insert(keyIdentity);
        handleTokens.insert(updateToken);
        handleTokens.insert(deleteToken);
        handleTokens.insert(testToken);
    }
    if (errorCode) errorCode->clear();
    return true;
}
