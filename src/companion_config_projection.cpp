#include "companion_config_projection.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QDateTime>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QUrl>

#include <cmath>

namespace {

constexpr int kMaximumKeys = 1000;
constexpr int kMaximumDisplayBytes = 128;
constexpr int kMaximumProjectionBytes = 1024 * 1024;
const char kSchemaVersion[] = "aegisy-companion-config-projection/0.1";
const char kSource[] = "aegisy-website-observation";
const char kCachePrefix[] = "companion/config_projection_v1/";
const char kAccountIdentityPrefix[] = "website-account-session:sha256:";

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

bool safeDisplayText(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    if (value.isEmpty() || utf8.size() > kMaximumDisplayBytes) return false;
    for (const QChar character : value) {
        const QChar::Category category = character.category();
        if (character.isNull() || category == QChar::Other_Surrogate
                || (category == QChar::Other_Control
                    && character != QLatin1Char('\t'))) {
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
    for (const QString &part : value.split(QRegularExpression(QStringLiteral("\\s+")))) {
        if ((part.startsWith(QStringLiteral("sk-")) && part.size() >= 12)
                || (part.startsWith(QStringLiteral("ghp_")) && part.size() >= 20)
                || (part.count(QLatin1Char('.')) == 2 && part.size() >= 24)) {
            return false;
        }
    }
    return true;
}

QString rawIdentifier(const QJsonValue &value)
{
    QString result;
    if (value.isString()) {
        result = value.toString().trimmed();
    } else if (value.isDouble()) {
        const double number = value.toDouble();
        if (!std::isfinite(number) || number < 0 || std::floor(number) != number
                || number > 9007199254740991.0) {
            return {};
        }
        result = QString::number(static_cast<qulonglong>(number));
    }
    if (result.isEmpty() || result.toUtf8().size() > kMaximumDisplayBytes) return {};
    for (const QChar character : result) {
        if (character.isNull() || character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Surrogate) {
            return {};
        }
    }
    return result;
}

void appendLengthFramed(QByteArray *output, const QByteArray &value)
{
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        output->append(static_cast<char>((size >> shift) & 0xff));
    }
    output->append(value);
}

QString keyIdentity(const QString &rawId)
{
    QByteArray input = QByteArrayLiteral("aegisy-companion-website-key/0.1\0");
    appendLengthFramed(&input, rawId.toUtf8());
    return QStringLiteral("website-key:sha256:%1").arg(
        QString::fromLatin1(QCryptographicHash::hash(
            input, QCryptographicHash::Sha256).toHex()));
}

QString normalizedState(const QJsonValue &value)
{
    const QString state = value.toString().trimmed().toLower();
    if (state == QStringLiteral("active") || state == QStringLiteral("enabled")) {
        return QStringLiteral("active");
    }
    if (state == QStringLiteral("inactive") || state == QStringLiteral("disabled")) {
        return QStringLiteral("inactive");
    }
    if (state == QStringLiteral("expired")) return QStringLiteral("expired");
    return QStringLiteral("unknown");
}

QString projectionDigest(QJsonObject projection)
{
    projection.remove(QStringLiteral("projection_sha256"));
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(projection).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}

bool validSha256Identity(const QString &value, const QString &prefix)
{
    if (!value.startsWith(prefix) || value.size() != prefix.size() + 64) return false;
    for (const QChar character : value.mid(prefix.size())) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a') && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

QString cacheKey(const QString &accountIdentity)
{
    return QString::fromLatin1(kCachePrefix)
        + accountIdentity.mid(QString::fromLatin1(kAccountIdentityPrefix).size());
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    const QSet<QString> actual(keys.cbegin(), keys.cend());
    return actual == expected;
}

} // namespace

QJsonObject CompanionConfigProjection::fromWebsiteApiKeys(
    const QJsonArray &apiKeys, const QString &accountIdentity,
    const QString &sourceOrigin, qint64 receivedAtMs, QString *errorCode)
{
    if (apiKeys.size() > kMaximumKeys) {
        fail(errorCode, QStringLiteral("projection-key-limit-exceeded"));
        return {};
    }
    if (!validSha256Identity(accountIdentity,
                             QString::fromLatin1(kAccountIdentityPrefix))
            || !isTrustedWebsiteOrigin(sourceOrigin)
            || receivedAtMs <= 0 || receivedAtMs > 9007199254740991LL) {
        fail(errorCode, QStringLiteral("projection-binding-invalid"));
        return {};
    }

    QJsonArray keys;
    QSet<QString> identities;
    for (const QJsonValue &value : apiKeys) {
        if (!value.isObject()) {
            fail(errorCode, QStringLiteral("projection-key-invalid"));
            return {};
        }
        const QJsonObject raw = value.toObject();
        const QString rawId = rawIdentifier(raw.value(QStringLiteral("id")));
        const QString identity = rawId.isEmpty() ? QString() : keyIdentity(rawId);
        QString name = raw.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) name = QStringLiteral("未命名 Key");
        const QJsonObject group = raw.value(QStringLiteral("group")).toObject();
        QString groupLabel = group.value(QStringLiteral("name")).toString().trimmed();
        if (groupLabel.isEmpty()) {
            groupLabel = raw.value(QStringLiteral("group_name")).toString().trimmed();
        }
        if (groupLabel.isEmpty()) groupLabel = QStringLiteral("未分组");
        if (identity.isEmpty() || identities.contains(identity)
                || !safeDisplayText(name) || !safeDisplayText(groupLabel)) {
            fail(errorCode, QStringLiteral("projection-key-invalid"));
            return {};
        }
        identities.insert(identity);

        QJsonObject projected;
        projected.insert(QStringLiteral("key_identity"), identity);
        projected.insert(QStringLiteral("display_name"), name);
        projected.insert(QStringLiteral("group_label"), groupLabel);
        projected.insert(QStringLiteral("state"), normalizedState(
            raw.value(QStringLiteral("status"))));
        projected.insert(QStringLiteral("website_credential_available"),
                         !raw.value(QStringLiteral("key")).toString().isEmpty());
        keys.append(projected);
    }

    QJsonObject projection;
    projection.insert(QStringLiteral("schema_version"),
                      QString::fromLatin1(kSchemaVersion));
    projection.insert(QStringLiteral("source"), QString::fromLatin1(kSource));
    projection.insert(QStringLiteral("account_identity"), accountIdentity);
    projection.insert(QStringLiteral("source_origin"), sourceOrigin);
    projection.insert(QStringLiteral("received_at_ms"), receivedAtMs);
    projection.insert(QStringLiteral("authenticated_at_capture"), true);
    projection.insert(QStringLiteral("key_count"), keys.size());
    projection.insert(QStringLiteral("keys"), keys);
    projection.insert(QStringLiteral("credential_values_included"), false);
    projection.insert(QStringLiteral("configuration_authority"), false);
    projection.insert(QStringLiteral("configuration_applied"), false);
    projection.insert(QStringLiteral("projection_sha256"), projectionDigest(projection));

    if (!validate(projection, errorCode)) return {};
    if (errorCode) errorCode->clear();
    return projection;
}

bool CompanionConfigProjection::validate(
    const QJsonObject &projection, QString *errorCode)
{
    static const QSet<QString> projectionKeys{
        QStringLiteral("schema_version"), QStringLiteral("source"),
        QStringLiteral("account_identity"), QStringLiteral("source_origin"),
        QStringLiteral("received_at_ms"),
        QStringLiteral("authenticated_at_capture"), QStringLiteral("key_count"),
        QStringLiteral("keys"), QStringLiteral("credential_values_included"),
        QStringLiteral("configuration_authority"),
        QStringLiteral("configuration_applied"),
        QStringLiteral("projection_sha256"),
    };
    static const QSet<QString> keyKeys{
        QStringLiteral("key_identity"), QStringLiteral("display_name"),
        QStringLiteral("group_label"), QStringLiteral("state"),
        QStringLiteral("website_credential_available"),
    };

    const QByteArray encoded = QJsonDocument(projection).toJson(QJsonDocument::Compact);
    const QJsonValue keysValue = projection.value(QStringLiteral("keys"));
    const QJsonValue countValue = projection.value(QStringLiteral("key_count"));
    const QJsonValue receivedAtValue = projection.value(QStringLiteral("received_at_ms"));
    const QJsonArray keys = keysValue.toArray();
    if (!exactKeys(projection, projectionKeys)
            || encoded.isEmpty() || encoded.size() > kMaximumProjectionBytes
            || !keysValue.isArray() || !countValue.isDouble()
            || !receivedAtValue.isDouble()
            || projection.value(QStringLiteral("schema_version")).toString()
                != QString::fromLatin1(kSchemaVersion)
            || projection.value(QStringLiteral("source")).toString()
                != QString::fromLatin1(kSource)
            || !validSha256Identity(
                projection.value(QStringLiteral("account_identity")).toString(),
                QString::fromLatin1(kAccountIdentityPrefix))
            || !isTrustedWebsiteOrigin(
                projection.value(QStringLiteral("source_origin")).toString())
            || receivedAtValue.toDouble() <= 0
            || std::floor(receivedAtValue.toDouble()) != receivedAtValue.toDouble()
            || receivedAtValue.toDouble() > 9007199254740991.0
            || projection.value(QStringLiteral("authenticated_at_capture"))
                != QJsonValue(true)
            || projection.value(QStringLiteral("credential_values_included"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("configuration_authority"))
                != QJsonValue(false)
            || projection.value(QStringLiteral("configuration_applied"))
                != QJsonValue(false)
            || keys.size() > kMaximumKeys
            || countValue.toInt(-1) != keys.size()
            || !validSha256Identity(
                QStringLiteral("projection:sha256:")
                    + projection.value(QStringLiteral("projection_sha256")).toString(),
                QStringLiteral("projection:sha256:"))
            || projection.value(QStringLiteral("projection_sha256")).toString()
                != projectionDigest(projection)) {
        fail(errorCode, QStringLiteral("projection-invalid"));
        return false;
    }

    QSet<QString> identities;
    for (const QJsonValue &value : keys) {
        const QJsonObject key = value.toObject();
        const QString identity = key.value(QStringLiteral("key_identity")).toString();
        const QString state = key.value(QStringLiteral("state")).toString();
        if (!value.isObject() || !exactKeys(key, keyKeys)
                || !identity.startsWith(QStringLiteral("website-key:sha256:"))
                || identity.size() != 83
                || identities.contains(identity)
                || !safeDisplayText(key.value(QStringLiteral("display_name")).toString())
                || !safeDisplayText(key.value(QStringLiteral("group_label")).toString())
                || !QSet<QString>{QStringLiteral("active"), QStringLiteral("inactive"),
                                  QStringLiteral("expired"), QStringLiteral("unknown")}
                        .contains(state)
                || !key.value(QStringLiteral("website_credential_available")).isBool()) {
            fail(errorCode, QStringLiteral("projection-key-invalid"));
            return false;
        }
        for (const QChar character : identity.mid(19)) {
            if (!character.isDigit()
                    && !(character >= QLatin1Char('a') && character <= QLatin1Char('f'))) {
                fail(errorCode, QStringLiteral("projection-key-invalid"));
                return false;
            }
        }
        identities.insert(identity);
    }
    if (errorCode) errorCode->clear();
    return true;
}

bool CompanionConfigProjection::saveLastValid(
    QSettings *settings, const QJsonObject &projection, QString *errorCode)
{
    if (!settings || !validate(projection, errorCode)) return false;
    const QByteArray encoded = QJsonDocument(projection).toJson(QJsonDocument::Compact);
    settings->setValue(cacheKey(
        projection.value(QStringLiteral("account_identity")).toString()), encoded);
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, QStringLiteral("projection-cache-write-failed"));
        return false;
    }
    if (errorCode) errorCode->clear();
    return true;
}

QJsonObject CompanionConfigProjection::loadLastValid(
    QSettings *settings, const QString &accountIdentity, QString *errorCode)
{
    if (!settings || !validSha256Identity(
            accountIdentity, QString::fromLatin1(kAccountIdentityPrefix))) {
        fail(errorCode, QStringLiteral("projection-cache-unavailable"));
        return {};
    }
    const QByteArray encoded = settings->value(
        cacheKey(accountIdentity)).toByteArray();
    if (encoded.isEmpty()) {
        fail(errorCode, QStringLiteral("projection-cache-empty"));
        return {};
    }
    if (encoded.size() > kMaximumProjectionBytes) {
        fail(errorCode, QStringLiteral("projection-cache-invalid"));
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !validate(document.object(), errorCode)
            || document.object().value(QStringLiteral("account_identity")).toString()
                != accountIdentity) {
        fail(errorCode, QStringLiteral("projection-cache-invalid"));
        return {};
    }
    if (errorCode) errorCode->clear();
    return document.object();
}

bool CompanionConfigProjection::isTrustedWebsiteOrigin(const QString &baseUrl)
{
    const QUrl url(baseUrl);
    const QString host = url.host().toLower();
    return url.isValid()
        && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0
        && (host == QStringLiteral("aegisy.cc")
            || host == QStringLiteral("www.aegisy.cc"))
        && (url.port(-1) == -1 || url.port(-1) == 443)
        && url.userInfo().isEmpty()
        && (url.path().isEmpty() || url.path() == QStringLiteral("/"))
        && !url.hasQuery() && url.fragment().isEmpty();
}

QString CompanionConfigProjection::accountIdentityForWebsiteId(const QJsonValue &accountId)
{
    const QString value = rawIdentifier(accountId);
    if (value.isEmpty() || value.toUtf8().size() > kMaximumDisplayBytes) return {};
    QByteArray input = QByteArrayLiteral("aegisy-companion-account/0.1\0");
    appendLengthFramed(&input, value.toUtf8());
    return QString::fromLatin1(kAccountIdentityPrefix)
        + QString::fromLatin1(QCryptographicHash::hash(
            input, QCryptographicHash::Sha256).toHex());
}
