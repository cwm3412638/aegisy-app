#include "companion_usage_projection.h"

#include "companion_config_projection.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <cmath>

namespace {

constexpr int kMaximumProjectionBytes = 1024 * 1024;

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend()) == expected;
}

bool validLowerSha256(const QString &value)
{
    if (value.size() != 64) return false;
    for (const QChar character : value) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a') && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool validKeyIdentity(const QString &value)
{
    const QString prefix = QStringLiteral("website-key:sha256:");
    return value.startsWith(prefix) && validLowerSha256(value.mid(prefix.size()));
}

bool validAccountIdentity(const QString &value)
{
    const QString prefix = QStringLiteral("website-account-session:sha256:");
    return value.startsWith(prefix) && validLowerSha256(value.mid(prefix.size()));
}

bool safeDisplayText(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    if (value.isEmpty() || utf8.size() > 128) return false;
    const QString lower = value.toLower();
    if (lower.contains(QStringLiteral("bearer "))
            || lower.contains(QStringLiteral("api_key="))
            || lower.contains(QStringLiteral("api-key="))
            || lower.contains(QStringLiteral("access_token="))
            || (value.startsWith(QStringLiteral("sk-")) && value.size() >= 12)) {
        return false;
    }
    for (const QChar character : value) {
        if (character.isNull() || character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Surrogate) {
            return false;
        }
    }
    return true;
}

bool validMetric(const QJsonValue &value)
{
    return value.isDouble() && std::isfinite(value.toDouble())
        && value.toDouble() >= 0.0 && value.toDouble() <= 9007199254740991.0;
}

QString digest(QJsonObject projection)
{
    projection.remove(QStringLiteral("projection_sha256"));
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(projection).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

} // namespace

QJsonObject CompanionUsageProjection::fromConfiguration(
    const QJsonObject &configurationProjection,
    const QHash<QString, QJsonObject> &usageByKeyIdentity,
    QString *errorCode)
{
    if (!CompanionConfigProjection::validate(configurationProjection)) {
        fail(errorCode, QStringLiteral("usage-projection-configuration-invalid"));
        return {};
    }
    QJsonArray keys;
    QSet<QString> expectedIdentities;
    for (const QJsonValue &value : configurationProjection.value(
         QStringLiteral("keys")).toArray()) {
        const QJsonObject candidate = value.toObject();
        const QString keyIdentity = candidate.value(
            QStringLiteral("key_identity")).toString();
        expectedIdentities.insert(keyIdentity);
        const QJsonObject metrics = usageByKeyIdentity.value(keyIdentity);
        const QJsonValue today = metrics.value(QStringLiteral("today_actual_cost"));
        const QJsonValue total = metrics.value(QStringLiteral("total_actual_cost"));
        const QJsonValue used = metrics.value(QStringLiteral("quota_used"));
        const QJsonValue quota = metrics.value(QStringLiteral("quota"));
        if (!validMetric(today) || !validMetric(total)
                || !validMetric(used) || !validMetric(quota)) {
            fail(errorCode, QStringLiteral("usage-projection-metric-invalid"));
            return {};
        }
        keys.append(QJsonObject{
            { QStringLiteral("key_identity"), keyIdentity },
            { QStringLiteral("display_name"),
              candidate.value(QStringLiteral("display_name")) },
            { QStringLiteral("group_label"),
              candidate.value(QStringLiteral("group_label")) },
            { QStringLiteral("state"), candidate.value(QStringLiteral("state")) },
            { QStringLiteral("today_actual_cost"), today },
            { QStringLiteral("total_actual_cost"), total },
            { QStringLiteral("quota_used"), used },
            { QStringLiteral("quota"), quota },
        });
    }
    for (auto it = usageByKeyIdentity.cbegin(); it != usageByKeyIdentity.cend(); ++it) {
        if (!expectedIdentities.contains(it.key())) {
            fail(errorCode, QStringLiteral("usage-projection-key-invalid"));
            return {};
        }
    }

    QJsonObject projection{
        { QStringLiteral("schema_version"),
          QStringLiteral("aegisy-companion-usage-projection/0.1") },
        { QStringLiteral("account_identity"), configurationProjection.value(
            QStringLiteral("account_identity")) },
        { QStringLiteral("configuration_projection_sha256"),
          configurationProjection.value(QStringLiteral("projection_sha256")) },
        { QStringLiteral("key_count"), keys.size() },
        { QStringLiteral("keys"), keys },
        { QStringLiteral("raw_key_ids_included"), false },
        { QStringLiteral("credential_values_included"), false },
        { QStringLiteral("configuration_authority"), false },
    };
    projection.insert(QStringLiteral("projection_sha256"), digest(projection));
    if (!validate(projection, errorCode)) return {};
    if (errorCode) errorCode->clear();
    return projection;
}

bool CompanionUsageProjection::validate(
    const QJsonObject &projection, QString *errorCode)
{
    static const QSet<QString> projectionKeys{
        QStringLiteral("schema_version"), QStringLiteral("account_identity"),
        QStringLiteral("configuration_projection_sha256"),
        QStringLiteral("key_count"), QStringLiteral("keys"),
        QStringLiteral("raw_key_ids_included"),
        QStringLiteral("credential_values_included"),
        QStringLiteral("configuration_authority"),
        QStringLiteral("projection_sha256"),
    };
    static const QSet<QString> keyKeys{
        QStringLiteral("key_identity"), QStringLiteral("display_name"),
        QStringLiteral("group_label"), QStringLiteral("state"),
        QStringLiteral("today_actual_cost"),
        QStringLiteral("total_actual_cost"), QStringLiteral("quota_used"),
        QStringLiteral("quota"),
    };
    const QJsonValue keysValue = projection.value(QStringLiteral("keys"));
    const QJsonArray keys = keysValue.toArray();
    const QByteArray encoded = QJsonDocument(projection).toJson(QJsonDocument::Compact);
    if (!exactKeys(projection, projectionKeys)
            || encoded.isEmpty() || encoded.size() > kMaximumProjectionBytes
            || projection.value(QStringLiteral("schema_version")).toString()
                != QStringLiteral("aegisy-companion-usage-projection/0.1")
            || !validAccountIdentity(projection.value(
                QStringLiteral("account_identity")).toString())
            || !keysValue.isArray()
            || projection.value(QStringLiteral("key_count")).toInt(-1) != keys.size()
            || !validLowerSha256(projection.value(
                QStringLiteral("configuration_projection_sha256")).toString())
            || !validLowerSha256(projection.value(
                QStringLiteral("projection_sha256")).toString())
            || projection.value(QStringLiteral("raw_key_ids_included"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("credential_values_included"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("configuration_authority"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("projection_sha256")).toString()
                != digest(projection)) {
        fail(errorCode, QStringLiteral("usage-projection-invalid"));
        return false;
    }
    QSet<QString> identities;
    for (const QJsonValue &value : keys) {
        const QJsonObject key = value.toObject();
        const QString identity = key.value(QStringLiteral("key_identity")).toString();
        if (!value.isObject() || !exactKeys(key, keyKeys)
                || !validKeyIdentity(identity) || identities.contains(identity)
                || !safeDisplayText(key.value(
                    QStringLiteral("display_name")).toString())
                || !safeDisplayText(key.value(
                    QStringLiteral("group_label")).toString())
                || !QSet<QString>{QStringLiteral("active"), QStringLiteral("inactive"),
                                  QStringLiteral("expired"), QStringLiteral("unknown")}
                    .contains(key.value(QStringLiteral("state")).toString())
                || !validMetric(key.value(QStringLiteral("today_actual_cost")))
                || !validMetric(key.value(QStringLiteral("total_actual_cost")))
                || !validMetric(key.value(QStringLiteral("quota_used")))
                || !validMetric(key.value(QStringLiteral("quota")))) {
            fail(errorCode, QStringLiteral("usage-projection-key-invalid"));
            return false;
        }
        identities.insert(identity);
    }
    if (errorCode) errorCode->clear();
    return true;
}
