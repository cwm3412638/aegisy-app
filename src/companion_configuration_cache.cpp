#include "companion_configuration_cache.h"

#include "companion_config_projection.h"
#include "companion_model_projection.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QVariant>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

constexpr qint64 kMaximumJsonInteger = 9007199254740991LL;
constexpr int kMaximumEnvelopeBytes = 2 * 1024 * 1024;
constexpr int kMaximumKeys = 1000;
constexpr int kMaximumModelsPerKey = 1000;
constexpr int kMaximumModelIdBytes = 128;
constexpr int kLockStaleMs = 30000;

const char kPayloadSchema[] = "aegisy-companion-configuration-cache-payload/0.2";
const char kEnvelopeSchema[] = "aegisy-companion-configuration-cache-envelope/0.2";
const char kAuthoritySchema[] = "aegisy-companion-configuration-cache-authority/0.1";
const char kConfigurationSchema[] = "aegisy-companion-cached-configuration/0.2";
const char kMacDomain[] = "aegisy-companion-configuration-cache-hmac/0.2\0";
const char kAccountPrefix[] = "website-account-session:sha256:";
const char kWebsiteKeyPrefix[] = "website-key:sha256:";
const char kSettingsPrefix[] = "companion/revisioned_configuration_cache_v2/";
const char kLegacyPrefix[] = "companion/config_projection_v1/";
const char kAuthorityScopePrefix[] = "companion/configuration-cache-authority/v1/";

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

bool lowerHex(const QString &value)
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

bool validAccountIdentity(const QString &value)
{
    const QString prefix = QString::fromLatin1(kAccountPrefix);
    return value.startsWith(prefix) && lowerHex(value.mid(prefix.size()));
}

bool validWebsiteKeyIdentity(const QString &value)
{
    const QString prefix = QString::fromLatin1(kWebsiteKeyPrefix);
    return value.startsWith(prefix) && lowerHex(value.mid(prefix.size()));
}

QString accountSuffix(const QString &accountIdentity)
{
    return accountIdentity.mid(QString::fromLatin1(kAccountPrefix).size());
}

QString settingsBase(const QString &accountIdentity)
{
    return QString::fromLatin1(kSettingsPrefix) + accountSuffix(accountIdentity);
}

QString slotSetting(const QString &accountIdentity, const QString &slot)
{
    return settingsBase(accountIdentity) + QStringLiteral("/slot_") + slot;
}

QString legacySetting(const QString &accountIdentity)
{
    return QString::fromLatin1(kLegacyPrefix) + accountSuffix(accountIdentity);
}

QString authorityScope(const QString &accountIdentity)
{
    return QString::fromLatin1(kAuthorityScopePrefix) + accountSuffix(accountIdentity);
}

bool exactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    return QSet<QString>(keys.cbegin(), keys.cend()) == expected;
}

bool safeInteger(const QJsonValue &value, qint64 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < 0 || number > static_cast<double>(kMaximumJsonInteger)) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

bool addMilliseconds(qint64 value, qint64 amount, qint64 *result)
{
    if (value <= 0 || amount < 0 || value > kMaximumJsonInteger - amount) {
        return false;
    }
    *result = value + amount;
    return true;
}

QByteArray quoted(const QString &value)
{
    const QByteArray array = QJsonDocument(QJsonArray{value})
        .toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}

bool appendCanonical(const QJsonValue &value, QByteArray *output)
{
    switch (value.type()) {
    case QJsonValue::Null:
        output->append("null");
        return true;
    case QJsonValue::Bool:
        output->append(value.toBool() ? "true" : "false");
        return true;
    case QJsonValue::Double: {
        qint64 number = 0;
        if (!safeInteger(value, &number)) return false;
        output->append(QByteArray::number(number));
        return true;
    }
    case QJsonValue::String:
        output->append(quoted(value.toString()));
        return true;
    case QJsonValue::Array: {
        output->append('[');
        const QJsonArray array = value.toArray();
        for (int index = 0; index < array.size(); ++index) {
            if (index != 0) output->append(',');
            if (!appendCanonical(array.at(index), output)) return false;
        }
        output->append(']');
        return true;
    }
    case QJsonValue::Object: {
        output->append('{');
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end(), [](const QString &left,
                                               const QString &right) {
            return left.toUtf8() < right.toUtf8();
        });
        for (int index = 0; index < keys.size(); ++index) {
            if (index != 0) output->append(',');
            output->append(quoted(keys.at(index)));
            output->append(':');
            if (!appendCanonical(object.value(keys.at(index)), output)) return false;
        }
        output->append('}');
        return true;
    }
    case QJsonValue::Undefined:
        return false;
    }
    return false;
}

QByteArray canonical(const QJsonObject &object)
{
    QByteArray bytes;
    return appendCanonical(object, &bytes) ? bytes : QByteArray();
}

QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool variantIsByteArray(const QVariant &value)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return value.metaType().id() == QMetaType::QByteArray;
#else
    return value.type() == QVariant::ByteArray;
#endif
}

QByteArray canonicalKey(const QByteArray &encoded)
{
    const QByteArray decoded = QByteArray::fromBase64(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    return decoded.size() == 32 && decoded.toBase64() == encoded
        ? decoded : QByteArray();
}

QByteArray hmac(const QByteArray &key, const QByteArray &message)
{
    QByteArray input(kMacDomain, sizeof(kMacDomain) - 1);
    input.append(message);
    unsigned char result[EVP_MAX_MD_SIZE]{};
    unsigned int length = 0;
    if (!HMAC(EVP_sha256(), key.constData(), key.size(),
              reinterpret_cast<const unsigned char *>(input.constData()),
              static_cast<size_t>(input.size()), result, &length)
            || length != 32) {
        OPENSSL_cleanse(result, sizeof(result));
        return {};
    }
    const QByteArray value(reinterpret_cast<const char *>(result), 32);
    OPENSSL_cleanse(result, sizeof(result));
    return value;
}

bool equalMac(const QByteArray &left, const QByteArray &right)
{
    return left.size() == 32 && right.size() == 32
        && CRYPTO_memcmp(left.constData(), right.constData(), 32) == 0;
}

void cleanse(QByteArray *bytes)
{
    if (!bytes || bytes->isEmpty()) return;
    OPENSSL_cleanse(bytes->data(), static_cast<size_t>(bytes->size()));
    bytes->clear();
}

bool validModelId(const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    if (value.isEmpty() || bytes.size() > kMaximumModelIdBytes) return false;
    for (const QChar character : value) {
        if (character.isNull() || character.isSpace()
                || character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Surrogate) {
            return false;
        }
    }
    const QString lower = value.toLower();
    return !lower.contains(QStringLiteral("bearer"))
        && !lower.contains(QStringLiteral("api_key"))
        && !lower.contains(QStringLiteral("credential"))
        && !lower.contains(QStringLiteral("access_token"))
        && !(value.startsWith(QStringLiteral("sk-")) && value.size() >= 12)
        && !(value.startsWith(QStringLiteral("ghp_")) && value.size() >= 20)
        && !(value.startsWith(QStringLiteral("github_pat_")) && value.size() >= 20)
        && !(value.count(QLatin1Char('.')) == 2 && value.size() >= 24);
}

bool validModelPlatform(const QString &value)
{
    return value == QStringLiteral("openai")
        || value == QStringLiteral("anthropic")
        || value == QStringLiteral("gemini");
}

bool safeDisplayText(const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    if (value.isEmpty() || bytes.size() > 128) return false;
    for (const QChar character : value) {
        if (character.isNull() || character.category() == QChar::Other_Surrogate
                || (character.category() == QChar::Other_Control
                    && character != QLatin1Char('\n')
                    && character != QLatin1Char('\t'))) {
            return false;
        }
    }
    const QString lower = value.toLower();
    if (lower.contains(QStringLiteral("bearer "))
            || lower.contains(QStringLiteral("api_key="))
            || lower.contains(QStringLiteral("api-key="))
            || lower.contains(QStringLiteral("access_token="))
            || lower.contains(QStringLiteral("credential="))) {
        return false;
    }
    for (const QString &part : value.split(
             QRegularExpression(QStringLiteral("\\s+")))) {
        const QString lowerPart = part.toLower();
        if ((lowerPart.startsWith(QStringLiteral("sk-")) && part.size() >= 12)
                || (lowerPart.startsWith(QStringLiteral("ghp_"))
                    && part.size() >= 20)
                || (lowerPart.startsWith(QStringLiteral("github_pat_"))
                    && part.size() >= 24)
                || (lowerPart.startsWith(QStringLiteral("xoxb-"))
                    && part.size() >= 20)
                || (part.count(QLatin1Char('.')) == 2 && part.size() >= 24)) {
            return false;
        }
    }
    return true;
}

struct Payload
{
    QJsonObject object;
    QString sha;
    qint64 revision = 0;
    QString previousSha;
    QString sourceObservationSha;
    QString contentSha;
    qint64 capturedAt = 0;
    qint64 validUntil = 0;
    qint64 staleUntil = 0;
    qint64 highWater = 0;
    QJsonObject configuration;
    QJsonArray models;
};

struct Envelope
{
    QByteArray bytes;
    QString sha;
    QString slot;
    qint64 revision = 0;
    Payload payload;
};

struct Anchor
{
    bool present = false;
    QString slot;
    qint64 revision = 0;
    QString payloadSha;
    QString envelopeSha;
    qint64 highWater = 0;
};

struct Prepared
{
    bool present = false;
    QString targetSlot;
    qint64 reservedRevision = 0;
    QString preimageEnvelopeSha;
    bool preimageAbsent = false;
    QString candidateEnvelopeSha;
    QString candidatePayloadSha;
    qint64 highWater = 0;
};

struct Authority
{
    QByteArray bytes;
    QByteArray keyEncoded;
    QByteArray key;
    qint64 highestReservedRevision = 0;
    Anchor committed;
    Prepared prepared;
};

QJsonValue anchorJson(const Anchor &anchor)
{
    if (!anchor.present) return QJsonValue::Null;
    return QJsonObject{
        { QStringLiteral("slot"), anchor.slot },
        { QStringLiteral("revision"), anchor.revision },
        { QStringLiteral("payload_sha256"), anchor.payloadSha },
        { QStringLiteral("envelope_sha256"), anchor.envelopeSha },
        { QStringLiteral("high_water_ms"), anchor.highWater },
    };
}

QJsonValue preparedJson(const Prepared &prepared)
{
    if (!prepared.present) return QJsonValue::Null;
    return QJsonObject{
        { QStringLiteral("target_slot"), prepared.targetSlot },
        { QStringLiteral("reserved_revision"), prepared.reservedRevision },
        { QStringLiteral("target_preimage_envelope_sha256"),
          prepared.preimageAbsent ? QJsonValue::Null
                                  : QJsonValue(prepared.preimageEnvelopeSha) },
        { QStringLiteral("candidate_envelope_sha256"),
          prepared.candidateEnvelopeSha },
        { QStringLiteral("candidate_payload_sha256"),
          prepared.candidatePayloadSha },
        { QStringLiteral("high_water_ms"), prepared.highWater },
    };
}

QByteArray authorityBytes(const QString &accountIdentity,
                          const Authority &authority)
{
    return canonical(QJsonObject{
        { QStringLiteral("schema_version"), QString::fromLatin1(kAuthoritySchema) },
        { QStringLiteral("account_identity"), accountIdentity },
        { QStringLiteral("hmac_key_base64"),
          QString::fromLatin1(authority.keyEncoded) },
        { QStringLiteral("phase"), authority.prepared.present
              ? QStringLiteral("prepared") : QStringLiteral("committed") },
        { QStringLiteral("highest_reserved_revision"),
          authority.highestReservedRevision },
        { QStringLiteral("committed"), anchorJson(authority.committed) },
        { QStringLiteral("prepared"), preparedJson(authority.prepared) },
    });
}

bool parseAnchor(const QJsonValue &value, Anchor *anchor)
{
    static const QSet<QString> keys{
        QStringLiteral("slot"), QStringLiteral("revision"),
        QStringLiteral("payload_sha256"), QStringLiteral("envelope_sha256"),
        QStringLiteral("high_water_ms"),
    };
    if (value.isNull()) {
        *anchor = Anchor{};
        return true;
    }
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    qint64 revision = 0;
    qint64 highWater = 0;
    const QString slot = object.value(QStringLiteral("slot")).toString();
    if (!exactKeys(object, keys)
            || (slot != QStringLiteral("a") && slot != QStringLiteral("b"))
            || !safeInteger(object.value(QStringLiteral("revision")), &revision)
            || revision <= 0
            || !lowerHex(object.value(QStringLiteral("payload_sha256")).toString())
            || !lowerHex(object.value(QStringLiteral("envelope_sha256")).toString())
            || !safeInteger(object.value(QStringLiteral("high_water_ms")), &highWater)
            || highWater <= 0) {
        return false;
    }
    anchor->present = true;
    anchor->slot = slot;
    anchor->revision = revision;
    anchor->payloadSha = object.value(QStringLiteral("payload_sha256")).toString();
    anchor->envelopeSha = object.value(QStringLiteral("envelope_sha256")).toString();
    anchor->highWater = highWater;
    return true;
}

bool parsePrepared(const QJsonValue &value, Prepared *prepared)
{
    static const QSet<QString> keys{
        QStringLiteral("target_slot"), QStringLiteral("reserved_revision"),
        QStringLiteral("target_preimage_envelope_sha256"),
        QStringLiteral("candidate_envelope_sha256"),
        QStringLiteral("candidate_payload_sha256"),
        QStringLiteral("high_water_ms"),
    };
    if (value.isNull()) {
        *prepared = Prepared{};
        return true;
    }
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const QString slot = object.value(QStringLiteral("target_slot")).toString();
    const QJsonValue preimage = object.value(
        QStringLiteral("target_preimage_envelope_sha256"));
    qint64 revision = 0;
    qint64 highWater = 0;
    if (!exactKeys(object, keys)
            || (slot != QStringLiteral("a") && slot != QStringLiteral("b"))
            || !safeInteger(object.value(QStringLiteral("reserved_revision")),
                            &revision)
            || revision <= 0
            || !((preimage.isNull())
                 || (preimage.isString() && lowerHex(preimage.toString())))
            || !lowerHex(object.value(
                QStringLiteral("candidate_envelope_sha256")).toString())
            || !lowerHex(object.value(
                QStringLiteral("candidate_payload_sha256")).toString())
            || !safeInteger(object.value(QStringLiteral("high_water_ms")), &highWater)
            || highWater <= 0) {
        return false;
    }
    prepared->present = true;
    prepared->targetSlot = slot;
    prepared->reservedRevision = revision;
    prepared->preimageAbsent = preimage.isNull();
    prepared->preimageEnvelopeSha = preimage.toString();
    prepared->candidateEnvelopeSha = object.value(
        QStringLiteral("candidate_envelope_sha256")).toString();
    prepared->candidatePayloadSha = object.value(
        QStringLiteral("candidate_payload_sha256")).toString();
    prepared->highWater = highWater;
    return true;
}

bool parseAuthority(const QByteArray &bytes, const QString &accountIdentity,
                    Authority *authority)
{
    static const QSet<QString> keys{
        QStringLiteral("schema_version"), QStringLiteral("account_identity"),
        QStringLiteral("hmac_key_base64"), QStringLiteral("phase"),
        QStringLiteral("highest_reserved_revision"),
        QStringLiteral("committed"), QStringLiteral("prepared"),
    };
    if (bytes.isEmpty() || bytes.size() > 16384) return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject object = document.object();
    qint64 highest = 0;
    const QByteArray keyEncoded = object.value(
        QStringLiteral("hmac_key_base64")).toString().toLatin1();
    QByteArray key = canonicalKey(keyEncoded);
    Anchor committed;
    Prepared prepared;
    const QString phase = object.value(QStringLiteral("phase")).toString();
    const bool valid = exactKeys(object, keys) && canonical(object) == bytes
        && object.value(QStringLiteral("schema_version")).toString()
            == QString::fromLatin1(kAuthoritySchema)
        && object.value(QStringLiteral("account_identity")).toString()
            == accountIdentity
        && key.size() == 32
        && safeInteger(object.value(
                           QStringLiteral("highest_reserved_revision")), &highest)
        && parseAnchor(object.value(QStringLiteral("committed")), &committed)
        && parsePrepared(object.value(QStringLiteral("prepared")), &prepared)
        && ((phase == QStringLiteral("committed") && !prepared.present)
            || (phase == QStringLiteral("prepared") && prepared.present))
        && (!committed.present || committed.revision <= highest)
        && (!prepared.present
            || (prepared.reservedRevision == highest
                && (!committed.present
                    || prepared.reservedRevision > committed.revision)
                && (committed.present
                    ? prepared.targetSlot != committed.slot
                    : (prepared.targetSlot == QStringLiteral("a")
                       && prepared.preimageAbsent))
                && prepared.highWater >= (committed.present
                    ? committed.highWater : 0)));
    if (!valid) {
        cleanse(&key);
        return false;
    }
    authority->bytes = bytes;
    authority->keyEncoded = keyEncoded;
    authority->key = key;
    authority->highestReservedRevision = highest;
    authority->committed = committed;
    authority->prepared = prepared;
    return true;
}

bool validateConfiguration(const QJsonObject &configuration,
                           const QString &accountIdentity, QString *contentSha)
{
    static const QSet<QString> configurationKeys{
        QStringLiteral("schema_version"), QStringLiteral("account_identity"),
        QStringLiteral("source_origin"), QStringLiteral("key_count"),
        QStringLiteral("keys"),
    };
    static const QSet<QString> keyKeys{
        QStringLiteral("key_identity"), QStringLiteral("display_name"),
        QStringLiteral("group_label"), QStringLiteral("platform"),
        QStringLiteral("state"),
    };
    qint64 count = 0;
    const QJsonValue keysValue = configuration.value(QStringLiteral("keys"));
    const QJsonArray keys = keysValue.toArray();
    if (!exactKeys(configuration, configurationKeys)
            || configuration.value(QStringLiteral("schema_version")).toString()
                != QString::fromLatin1(kConfigurationSchema)
            || configuration.value(QStringLiteral("account_identity")).toString()
                != accountIdentity
            || !CompanionConfigProjection::isTrustedWebsiteOrigin(
                configuration.value(QStringLiteral("source_origin")).toString())
            || !keysValue.isArray()
            || !safeInteger(configuration.value(QStringLiteral("key_count")), &count)
            || count != keys.size() || keys.size() > kMaximumKeys) {
        return false;
    }
    QSet<QString> identities;
    for (const QJsonValue &value : keys) {
        if (!value.isObject()) return false;
        const QJsonObject key = value.toObject();
        const QString identity = key.value(QStringLiteral("key_identity")).toString();
        const QString platform = key.value(QStringLiteral("platform")).toString();
        const QString state = key.value(QStringLiteral("state")).toString();
        if (!exactKeys(key, keyKeys) || !validWebsiteKeyIdentity(identity)
                || identities.contains(identity)
                || !safeDisplayText(
                    key.value(QStringLiteral("display_name")).toString())
                || !safeDisplayText(
                    key.value(QStringLiteral("group_label")).toString())
                || !QSet<QString>{QStringLiteral("openai"),
                                  QStringLiteral("anthropic"),
                                  QStringLiteral("gemini"),
                                  QStringLiteral("unknown")}.contains(platform)
                || !QSet<QString>{QStringLiteral("active"),
                                  QStringLiteral("inactive"),
                                  QStringLiteral("expired"),
                                  QStringLiteral("unknown")}.contains(state)) {
            return false;
        }
        identities.insert(identity);
    }
    const QByteArray bytes = canonical(configuration);
    if (bytes.isEmpty() || bytes.size() > kMaximumEnvelopeBytes) return false;
    *contentSha = sha256(bytes);
    return true;
}

bool validateModels(const QJsonArray &models, const Payload &payload)
{
    static const QSet<QString> keys{
        QStringLiteral("key_identity"), QStringLiteral("platform"),
        QStringLiteral("configuration_observation_sha256"),
        QStringLiteral("source_observation_sha256"),
        QStringLiteral("captured_at_ms"), QStringLiteral("valid_until_ms"),
        QStringLiteral("model_count"), QStringLiteral("models"),
    };
    QHash<QString, QString> configured;
    for (const QJsonValue &value
         : payload.configuration.value(QStringLiteral("keys")).toArray()) {
        const QJsonObject key = value.toObject();
        configured.insert(key.value(QStringLiteral("key_identity")).toString(),
                          key.value(QStringLiteral("platform")).toString());
    }
    if (models.size() > configured.size()) return false;
    QSet<QString> seenKeys;
    for (const QJsonValue &value : models) {
        if (!value.isObject()) return false;
        const QJsonObject entry = value.toObject();
        const QString keyIdentity = entry.value(QStringLiteral("key_identity")).toString();
        const QString platform = entry.value(QStringLiteral("platform")).toString();
        qint64 captured = 0;
        qint64 validUntil = 0;
        qint64 count = 0;
        qint64 maximumValid = 0;
        const QJsonValue listValue = entry.value(QStringLiteral("models"));
        const QJsonArray list = listValue.toArray();
        if (!exactKeys(entry, keys) || !configured.contains(keyIdentity)
                || !validModelPlatform(platform)
                || configured.value(keyIdentity) != platform
                || seenKeys.contains(keyIdentity)
                || !lowerHex(entry.value(QStringLiteral(
                    "configuration_observation_sha256")).toString())
                || entry.value(QStringLiteral(
                    "configuration_observation_sha256")).toString()
                    != payload.sourceObservationSha
                || !lowerHex(entry.value(QStringLiteral(
                    "source_observation_sha256")).toString())
                || !safeInteger(entry.value(QStringLiteral("captured_at_ms")), &captured)
                || !safeInteger(entry.value(QStringLiteral("valid_until_ms")), &validUntil)
                || !safeInteger(entry.value(QStringLiteral("model_count")), &count)
                || !listValue.isArray() || count != list.size()
                || list.size() > kMaximumModelsPerKey || captured <= 0
                || captured > payload.highWater
                || !addMilliseconds(captured,
                                    CompanionConfigurationCache::ModelFreshMs,
                                    &maximumValid)
                || validUntil <= captured
                || validUntil > std::min(maximumValid, payload.validUntil)) {
            return false;
        }
        QSet<QString> seenModels;
        for (const QJsonValue &modelValue : list) {
            const QString model = modelValue.toString();
            if (!modelValue.isString() || !validModelId(model)
                    || seenModels.contains(model)) {
                return false;
            }
            seenModels.insert(model);
        }
        seenKeys.insert(keyIdentity);
    }
    return true;
}

bool parsePayload(const QJsonObject &object, const QString &accountIdentity,
                  qint64 revision, Payload *payload)
{
    static const QSet<QString> keys{
        QStringLiteral("schema_version"), QStringLiteral("account_identity"),
        QStringLiteral("revision"), QStringLiteral("previous_payload_sha256"),
        QStringLiteral("source_observation_sha256"),
        QStringLiteral("content_sha256"), QStringLiteral("captured_at_ms"),
        QStringLiteral("valid_until_ms"), QStringLiteral("stale_until_ms"),
        QStringLiteral("high_water_ms"), QStringLiteral("configuration"),
        QStringLiteral("models"), QStringLiteral("configuration_authority"),
        QStringLiteral("configuration_applied"),
        QStringLiteral("model_selection_authority"),
    };
    qint64 storedRevision = 0;
    qint64 captured = 0;
    qint64 validUntil = 0;
    qint64 staleUntil = 0;
    qint64 highWater = 0;
    qint64 expectedValid = 0;
    qint64 expectedStale = 0;
    const QJsonValue previous = object.value(QStringLiteral("previous_payload_sha256"));
    const QJsonValue configuration = object.value(QStringLiteral("configuration"));
    const QJsonValue models = object.value(QStringLiteral("models"));
    if (!exactKeys(object, keys)
            || object.value(QStringLiteral("schema_version")).toString()
                != QString::fromLatin1(kPayloadSchema)
            || object.value(QStringLiteral("account_identity")).toString()
                != accountIdentity
            || !safeInteger(object.value(QStringLiteral("revision")), &storedRevision)
            || storedRevision != revision || revision <= 0
            || !((previous.isNull())
                 || (previous.isString() && lowerHex(previous.toString())))
            || !lowerHex(object.value(QStringLiteral(
                "source_observation_sha256")).toString())
            || !lowerHex(object.value(QStringLiteral("content_sha256")).toString())
            || !safeInteger(object.value(QStringLiteral("captured_at_ms")), &captured)
            || !safeInteger(object.value(QStringLiteral("valid_until_ms")), &validUntil)
            || !safeInteger(object.value(QStringLiteral("stale_until_ms")), &staleUntil)
            || !safeInteger(object.value(QStringLiteral("high_water_ms")), &highWater)
            || captured <= 0 || highWater < captured
            || !addMilliseconds(captured,
                                CompanionConfigurationCache::ConfigurationFreshMs,
                                &expectedValid)
            || !addMilliseconds(expectedValid,
                                CompanionConfigurationCache::ConfigurationStaleMs,
                                &expectedStale)
            || validUntil != expectedValid || staleUntil != expectedStale
            || !configuration.isObject() || !models.isArray()
            || object.value(QStringLiteral("configuration_authority"))
                != QJsonValue(false)
            || object.value(QStringLiteral("configuration_applied"))
                != QJsonValue(false)
            || object.value(QStringLiteral("model_selection_authority"))
                != QJsonValue(false)) {
        return false;
    }
    Payload candidate;
    candidate.object = object;
    candidate.revision = revision;
    candidate.previousSha = previous.toString();
    candidate.sourceObservationSha = object.value(
        QStringLiteral("source_observation_sha256")).toString();
    candidate.contentSha = object.value(QStringLiteral("content_sha256")).toString();
    candidate.capturedAt = captured;
    candidate.validUntil = validUntil;
    candidate.staleUntil = staleUntil;
    candidate.highWater = highWater;
    candidate.configuration = configuration.toObject();
    candidate.models = models.toArray();
    QString computedContent;
    if (!validateConfiguration(candidate.configuration,
                               accountIdentity, &computedContent)
            || computedContent != candidate.contentSha
            || !validateModels(candidate.models, candidate)) {
        return false;
    }
    const QByteArray bytes = canonical(object);
    if (bytes.isEmpty() || bytes.size() > kMaximumEnvelopeBytes) return false;
    candidate.sha = sha256(bytes);
    *payload = candidate;
    return true;
}

bool parseEnvelope(const QByteArray &bytes, const QString &expectedSlot,
                   const QString &accountIdentity, const QByteArray &key,
                   Envelope *envelope)
{
    static const QSet<QString> keys{
        QStringLiteral("schema_version"), QStringLiteral("account_identity"),
        QStringLiteral("slot"), QStringLiteral("revision"),
        QStringLiteral("payload_sha256"), QStringLiteral("payload"),
        QStringLiteral("mac"),
    };
    if (bytes.isEmpty() || bytes.size() > kMaximumEnvelopeBytes) return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject object = document.object();
    qint64 revision = 0;
    const QJsonValue payloadValue = object.value(QStringLiteral("payload"));
    const QByteArray macEncoded = object.value(QStringLiteral("mac"))
        .toString().toLatin1();
    const QByteArray storedMac = QByteArray::fromBase64(
        macEncoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!exactKeys(object, keys) || canonical(object) != bytes
            || object.value(QStringLiteral("schema_version")).toString()
                != QString::fromLatin1(kEnvelopeSchema)
            || object.value(QStringLiteral("account_identity")).toString()
                != accountIdentity
            || object.value(QStringLiteral("slot")).toString() != expectedSlot
            || !safeInteger(object.value(QStringLiteral("revision")), &revision)
            || revision <= 0 || !payloadValue.isObject()
            || !lowerHex(object.value(QStringLiteral("payload_sha256")).toString())
            || storedMac.size() != 32 || storedMac.toBase64() != macEncoded) {
        return false;
    }
    QJsonObject unsignedEnvelope = object;
    unsignedEnvelope.remove(QStringLiteral("mac"));
    if (!equalMac(storedMac, hmac(key, canonical(unsignedEnvelope)))) return false;
    Payload payload;
    if (!parsePayload(payloadValue.toObject(), accountIdentity, revision, &payload)
            || payload.sha
                != object.value(QStringLiteral("payload_sha256")).toString()) {
        return false;
    }
    envelope->bytes = bytes;
    envelope->sha = sha256(bytes);
    envelope->slot = expectedSlot;
    envelope->revision = revision;
    envelope->payload = payload;
    return true;
}

QJsonObject sanitizedConfiguration(const QJsonObject &projection)
{
    QJsonArray keys;
    for (const QJsonValue &value : projection.value(QStringLiteral("keys")).toArray()) {
        const QJsonObject key = value.toObject();
        keys.append(QJsonObject{
            { QStringLiteral("key_identity"), key.value(QStringLiteral("key_identity")) },
            { QStringLiteral("display_name"), key.value(QStringLiteral("display_name")) },
            { QStringLiteral("group_label"), key.value(QStringLiteral("group_label")) },
            { QStringLiteral("platform"), key.value(QStringLiteral("platform")) },
            { QStringLiteral("state"), key.value(QStringLiteral("state")) },
        });
    }
    return QJsonObject{
        { QStringLiteral("schema_version"), QString::fromLatin1(kConfigurationSchema) },
        { QStringLiteral("account_identity"),
          projection.value(QStringLiteral("account_identity")) },
        { QStringLiteral("source_origin"),
          projection.value(QStringLiteral("source_origin")) },
        { QStringLiteral("key_count"), keys.size() },
        { QStringLiteral("keys"), keys },
    };
}

QJsonObject makePayload(const QString &accountIdentity, qint64 revision,
                        const QString &previousSha,
                        const QString &sourceObservationSha,
                        const QString &contentSha, qint64 capturedAt,
                        qint64 validUntil, qint64 staleUntil, qint64 highWater,
                        const QJsonObject &configuration, const QJsonArray &models)
{
    return QJsonObject{
        { QStringLiteral("schema_version"), QString::fromLatin1(kPayloadSchema) },
        { QStringLiteral("account_identity"), accountIdentity },
        { QStringLiteral("revision"), revision },
        { QStringLiteral("previous_payload_sha256"),
          previousSha.isEmpty() ? QJsonValue::Null : QJsonValue(previousSha) },
        { QStringLiteral("source_observation_sha256"), sourceObservationSha },
        { QStringLiteral("content_sha256"), contentSha },
        { QStringLiteral("captured_at_ms"), capturedAt },
        { QStringLiteral("valid_until_ms"), validUntil },
        { QStringLiteral("stale_until_ms"), staleUntil },
        { QStringLiteral("high_water_ms"), highWater },
        { QStringLiteral("configuration"), configuration },
        { QStringLiteral("models"), models },
        { QStringLiteral("configuration_authority"), false },
        { QStringLiteral("configuration_applied"), false },
        { QStringLiteral("model_selection_authority"), false },
    };
}

QByteArray makeEnvelope(const QString &accountIdentity, const QString &slot,
                        qint64 revision, const QJsonObject &payload,
                        const QByteArray &key)
{
    QJsonObject envelope{
        { QStringLiteral("schema_version"), QString::fromLatin1(kEnvelopeSchema) },
        { QStringLiteral("account_identity"), accountIdentity },
        { QStringLiteral("slot"), slot },
        { QStringLiteral("revision"), revision },
        { QStringLiteral("payload_sha256"), sha256(canonical(payload)) },
        { QStringLiteral("payload"), payload },
    };
    const QByteArray mac = hmac(key, canonical(envelope));
    if (mac.size() != 32) return {};
    envelope.insert(QStringLiteral("mac"), QString::fromLatin1(mac.toBase64()));
    return canonical(envelope);
}

struct SlotBytes
{
    bool present = false;
    QByteArray bytes;
    QString sha;
};

bool readSlot(QSettings *settings, const QString &accountIdentity,
              const QString &slot, SlotBytes *result, QString *errorCode)
{
    const QString key = slotSetting(accountIdentity, slot);
    result->present = settings->contains(key);
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, QStringLiteral("cache-settings-unavailable"));
        return false;
    }
    if (!result->present) return true;
    const QVariant value = settings->value(key);
    if (settings->status() != QSettings::NoError) {
        fail(errorCode, QStringLiteral("cache-settings-unavailable"));
        return false;
    }
    if (!variantIsByteArray(value)) {
        fail(errorCode, QStringLiteral("cache-slot-type-invalid"));
        return false;
    }
    result->bytes = value.toByteArray();
    if (result->bytes.isEmpty() || result->bytes.size() > kMaximumEnvelopeBytes) {
        fail(errorCode, QStringLiteral("cache-slot-bounds-invalid"));
        return false;
    }
    result->sha = sha256(result->bytes);
    return true;
}

bool writeSlot(QSettings *settings, const QString &accountIdentity,
               const QString &slot, const QByteArray &bytes,
               const SlotBytes &preimage, QString *errorCode)
{
    const QString key = slotSetting(accountIdentity, slot);
    settings->setValue(key, bytes);
    settings->sync();
    SlotBytes observed;
    QString readError;
    if (!readSlot(settings, accountIdentity, slot, &observed, &readError)) {
        fail(errorCode, readError);
        return false;
    }
    if (observed.present && observed.bytes == bytes) return true;
    if (observed.present == preimage.present
            && (!observed.present || observed.bytes == preimage.bytes)) {
        fail(errorCode, QStringLiteral("cache-slot-definite-failure"));
    } else {
        fail(errorCode, QStringLiteral("cache-slot-outcome-unknown"));
    }
    return false;
}

bool removeLegacyExact(QSettings *settings,
                       CompanionConfigurationCacheLegacyCleaner *cleaner,
                       const QString &key, QString *warning)
{
    QString error;
    if (cleaner) {
        if (cleaner->removeExact(settings, key, &error)) return true;
        *warning = error.isEmpty()
            ? QStringLiteral("cache-legacy-cleanup-failed") : error;
        return false;
    }
    const bool present = settings->contains(key);
    if (settings->status() != QSettings::NoError) {
        *warning = QStringLiteral("cache-legacy-cleanup-status-unavailable");
        return false;
    }
    if (!present) return true;
    settings->remove(key);
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        *warning = QStringLiteral("cache-legacy-cleanup-sync-failed");
        return false;
    }
    const bool remains = settings->contains(key);
    if (settings->status() != QSettings::NoError || remains) {
        *warning = remains
            ? QStringLiteral("cache-legacy-cleanup-readback-present")
            : QStringLiteral("cache-legacy-cleanup-readback-unavailable");
        return false;
    }
    return true;
}

bool validLockPath(const QString &lockFilePath)
{
    const QFileInfo info(lockFilePath);
    const QFileInfo parent(info.dir().absolutePath());
    return !lockFilePath.isEmpty() && lockFilePath.toUtf8().size() <= 4096
        && info.isAbsolute() && info.fileName() != QStringLiteral(".")
        && info.fileName() != QStringLiteral("..")
        && parent.exists() && parent.isDir() && !parent.isSymLink()
        && (!info.exists() || !info.isSymLink());
}

std::unique_ptr<QLockFile> acquireLock(const QString &lockFilePath,
                                       QString *errorCode)
{
    if (!validLockPath(lockFilePath)) {
        fail(errorCode, QStringLiteral("cache-lock-path-unavailable"));
        return {};
    }
    auto lock = std::make_unique<QLockFile>(lockFilePath);
    lock->setStaleLockTime(kLockStaleMs);
    if (!lock->tryLock(5000)) {
        fail(errorCode, QStringLiteral("cache-lock-unavailable"));
        return {};
    }
    return lock;
}

enum class AuthorityWriteResult {
    Expected,
    Previous,
    OutcomeUnknown,
    Invalid,
};

AuthorityWriteResult writeAuthority(
    CompanionConfigurationCacheSecureStore *store, const QString &scope,
    const QByteArray &expected, const QByteArray &previous, bool previousMissing,
    QString *errorCode)
{
    QString writeError;
    const auto outcome = store->write(scope, expected, &writeError);
    QByteArray observed;
    QString readError;
    const auto readState = store->readFresh(scope, &observed, &readError);
    if (readState == CompanionConfigurationCacheSecureStore::ReadState::Found
            && observed == expected) {
        return AuthorityWriteResult::Expected;
    }
    if ((readState == CompanionConfigurationCacheSecureStore::ReadState::Missing
         && previousMissing)
            || (readState
                    == CompanionConfigurationCacheSecureStore::ReadState::Found
                && !previousMissing && observed == previous)) {
        fail(errorCode, writeError.isEmpty()
             ? QStringLiteral("cache-authority-definite-failure") : writeError);
        return AuthorityWriteResult::Previous;
    }
    if (readState == CompanionConfigurationCacheSecureStore::ReadState::Invalid
            || readState == CompanionConfigurationCacheSecureStore::ReadState::Found
            || readState == CompanionConfigurationCacheSecureStore::ReadState::Missing) {
        fail(errorCode, QStringLiteral("cache-authority-write-drift"));
        return AuthorityWriteResult::Invalid;
    }
    if (readState == CompanionConfigurationCacheSecureStore::ReadState::Unavailable) {
        fail(errorCode, readError.isEmpty()
             ? (outcome
                    == CompanionConfigurationCacheSecureStore::WriteOutcome::DefiniteFailure
                    ? QStringLiteral("cache-authority-unavailable-after-failure")
                    : QStringLiteral("cache-authority-outcome-unknown"))
             : readError);
        return AuthorityWriteResult::OutcomeUnknown;
    }
    fail(errorCode, QStringLiteral("cache-authority-write-drift"));
    return AuthorityWriteResult::Invalid;
}

enum class LoadState {
    Empty,
    Ready,
    Legacy,
    Invalid,
    Unavailable,
    OutcomeUnknown,
    RecoveryRequired,
};

struct Loaded
{
    LoadState state = LoadState::Unavailable;
    QString error;
    Authority authority;
    Envelope current;
    SlotBytes slotA;
    SlotBytes slotB;
};

bool validateCommitted(const QString &accountIdentity, Loaded *loaded)
{
    const Anchor &anchor = loaded->authority.committed;
    if (!anchor.present) {
        if (loaded->slotA.present || loaded->slotB.present) {
            loaded->state = LoadState::Invalid;
            loaded->error = QStringLiteral("cache-unanchored-slot-present");
            return false;
        }
        loaded->state = LoadState::Empty;
        return true;
    }
    const SlotBytes &active = anchor.slot == QStringLiteral("a")
        ? loaded->slotA : loaded->slotB;
    const SlotBytes &other = anchor.slot == QStringLiteral("a")
        ? loaded->slotB : loaded->slotA;
    Envelope current;
    if (!active.present || active.sha != anchor.envelopeSha
            || !parseEnvelope(active.bytes, anchor.slot, accountIdentity,
                              loaded->authority.key, &current)
            || current.revision != anchor.revision
            || current.payload.sha != anchor.payloadSha
            || anchor.highWater < current.payload.highWater) {
        loaded->state = LoadState::Invalid;
        loaded->error = QStringLiteral("cache-committed-anchor-invalid");
        return false;
    }
    if (current.payload.previousSha.isEmpty()) {
        if (other.present) {
            loaded->state = LoadState::Invalid;
            loaded->error = QStringLiteral("cache-unbound-secondary-slot");
            return false;
        }
    } else {
        if (!other.present) {
            loaded->state = LoadState::Invalid;
            loaded->error = QStringLiteral("cache-previous-slot-missing");
            return false;
        }
        Envelope previous;
        const QString otherSlot = anchor.slot == QStringLiteral("a")
            ? QStringLiteral("b") : QStringLiteral("a");
        if (!parseEnvelope(other.bytes, otherSlot, accountIdentity,
                           loaded->authority.key, &previous)
                || previous.revision >= current.revision
                || previous.payload.sha != current.payload.previousSha) {
            loaded->state = LoadState::Invalid;
            loaded->error = previous.revision == current.revision
                ? QStringLiteral("cache-same-revision-drift")
                : QStringLiteral("cache-revision-rollback-or-drift");
            return false;
        }
    }
    loaded->current = current;
    loaded->state = LoadState::Ready;
    return true;
}

Loaded loadAndRecover(CompanionConfigurationCacheSecureStore *store,
                      QSettings *settings, const QString &accountIdentity)
{
    Loaded loaded;
    if (!store || !settings) {
        loaded.error = QStringLiteral("cache-dependencies-unavailable");
        return loaded;
    }
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        loaded.state = LoadState::Unavailable;
        loaded.error = QStringLiteral("cache-settings-unavailable");
        return loaded;
    }
    const QString accountPrefix = settingsBase(accountIdentity);
    const QString nestedPrefix = accountPrefix + QLatin1Char('/');
    const QString slotAName = slotSetting(accountIdentity, QStringLiteral("a"));
    const QString slotBName = slotSetting(accountIdentity, QStringLiteral("b"));
    for (const QString &key : settings->allKeys()) {
        if ((key == accountPrefix || key.startsWith(nestedPrefix))
                && key != slotAName && key != slotBName) {
            loaded.state = LoadState::Invalid;
            loaded.error = QStringLiteral("cache-settings-namespace-invalid");
            return loaded;
        }
    }
    if (settings->status() != QSettings::NoError) {
        loaded.state = LoadState::Unavailable;
        loaded.error = QStringLiteral("cache-settings-unavailable");
        return loaded;
    }
    QString slotError;
    if (!readSlot(settings, accountIdentity, QStringLiteral("a"),
                  &loaded.slotA, &slotError)
            || !readSlot(settings, accountIdentity, QStringLiteral("b"),
                         &loaded.slotB, &slotError)) {
        loaded.state = slotError == QStringLiteral("cache-settings-unavailable")
            ? LoadState::Unavailable : LoadState::Invalid;
        loaded.error = slotError;
        return loaded;
    }
    QByteArray authorityData;
    QString secureError;
    const auto readState = store->readFresh(
        authorityScope(accountIdentity), &authorityData, &secureError);
    if (readState == CompanionConfigurationCacheSecureStore::ReadState::Unavailable) {
        loaded.state = LoadState::Unavailable;
        loaded.error = secureError.isEmpty()
            ? QStringLiteral("cache-authority-unavailable") : secureError;
        return loaded;
    }
    if (readState == CompanionConfigurationCacheSecureStore::ReadState::Invalid) {
        loaded.state = LoadState::Invalid;
        loaded.error = secureError.isEmpty()
            ? QStringLiteral("cache-authority-backend-invalid") : secureError;
        return loaded;
    }
    if (readState == CompanionConfigurationCacheSecureStore::ReadState::Missing) {
        if (loaded.slotA.present || loaded.slotB.present) {
            loaded.state = LoadState::Invalid;
            loaded.error = QStringLiteral("cache-slots-without-authority");
        } else {
            const QString legacy = legacySetting(accountIdentity);
            const bool legacyPresent = settings->contains(legacy);
            if (settings->status() != QSettings::NoError) {
                loaded.state = LoadState::Unavailable;
                loaded.error = QStringLiteral("cache-settings-unavailable");
            } else if (!legacyPresent) {
                loaded.state = LoadState::Empty;
            } else {
                const QVariant legacyValue = settings->value(legacy);
                if (settings->status() != QSettings::NoError) {
                    loaded.state = LoadState::Unavailable;
                    loaded.error = QStringLiteral("cache-settings-unavailable");
                } else if (!variantIsByteArray(legacyValue)
                           || legacyValue.toByteArray().isEmpty()
                           || legacyValue.toByteArray().size() > 1024 * 1024) {
                    loaded.state = LoadState::Invalid;
                    loaded.error = QStringLiteral("cache-legacy-evidence-invalid");
                } else {
                    loaded.state = LoadState::Legacy;
                    loaded.error = QStringLiteral("cache-legacy-unverified");
                }
            }
        }
        return loaded;
    }
    if (!parseAuthority(authorityData, accountIdentity, &loaded.authority)) {
        loaded.state = LoadState::Invalid;
        loaded.error = QStringLiteral("cache-authority-invalid");
        return loaded;
    }
    if (!loaded.authority.prepared.present) {
        validateCommitted(accountIdentity, &loaded);
        return loaded;
    }

    const Prepared prepared = loaded.authority.prepared;
    const SlotBytes &target = prepared.targetSlot == QStringLiteral("a")
        ? loaded.slotA : loaded.slotB;
    const bool isPreimage = target.present != prepared.preimageAbsent
        && (prepared.preimageAbsent || target.sha == prepared.preimageEnvelopeSha);
    const bool isCandidate = target.present
        && target.sha == prepared.candidateEnvelopeSha;
    Authority recovered = loaded.authority;
    if (isPreimage) {
        recovered.prepared = Prepared{};
    } else if (isCandidate) {
        Envelope candidate;
        if (!parseEnvelope(target.bytes, prepared.targetSlot, accountIdentity,
                           loaded.authority.key, &candidate)
                || candidate.revision != prepared.reservedRevision
                || candidate.payload.sha != prepared.candidatePayloadSha
                || candidate.payload.previousSha
                    != (loaded.authority.committed.present
                        ? loaded.authority.committed.payloadSha : QString())) {
            loaded.state = LoadState::Invalid;
            loaded.error = QStringLiteral("cache-prepared-candidate-invalid");
            return loaded;
        }
        recovered.committed.present = true;
        recovered.committed.slot = prepared.targetSlot;
        recovered.committed.revision = candidate.revision;
        recovered.committed.payloadSha = candidate.payload.sha;
        recovered.committed.envelopeSha = candidate.sha;
        recovered.committed.highWater = prepared.highWater;
        recovered.prepared = Prepared{};
    } else {
        loaded.state = LoadState::Invalid;
        loaded.error = QStringLiteral("cache-prepared-target-third-state");
        return loaded;
    }
    recovered.bytes = authorityBytes(accountIdentity, recovered);
    const AuthorityWriteResult resolution = writeAuthority(
        store, authorityScope(accountIdentity), recovered.bytes,
        loaded.authority.bytes, false, &loaded.error);
    if (resolution == AuthorityWriteResult::OutcomeUnknown) {
        loaded.state = LoadState::OutcomeUnknown;
        return loaded;
    }
    if (resolution == AuthorityWriteResult::Previous) {
        loaded.state = LoadState::RecoveryRequired;
        loaded.error = QStringLiteral("cache-prepared-recovery-required");
        return loaded;
    }
    if (resolution == AuthorityWriteResult::Invalid) {
        loaded.state = LoadState::Invalid;
        return loaded;
    }
    cleanse(&loaded.authority.key);
    loaded.authority = recovered;
    validateCommitted(accountIdentity, &loaded);
    return loaded;
}

bool initializeAuthority(CompanionConfigurationCacheSecureStore *store,
                         const QString &accountIdentity, Authority *authority,
                         LoadState *failureState, QString *errorCode)
{
    unsigned char raw[32]{};
    if (RAND_bytes(raw, sizeof(raw)) != 1) {
        OPENSSL_cleanse(raw, sizeof(raw));
        fail(errorCode, QStringLiteral("cache-key-generation-failed"));
        *failureState = LoadState::Unavailable;
        return false;
    }
    authority->key = QByteArray(reinterpret_cast<const char *>(raw), sizeof(raw));
    OPENSSL_cleanse(raw, sizeof(raw));
    authority->keyEncoded = authority->key.toBase64();
    authority->bytes = authorityBytes(accountIdentity, *authority);
    const AuthorityWriteResult result = writeAuthority(
        store, authorityScope(accountIdentity), authority->bytes,
        QByteArray(), true, errorCode);
    if (result != AuthorityWriteResult::Expected) {
        *failureState = result == AuthorityWriteResult::OutcomeUnknown
            ? LoadState::OutcomeUnknown
            : (result == AuthorityWriteResult::Invalid
                ? LoadState::Invalid : LoadState::Unavailable);
        cleanse(&authority->key);
        return false;
    }
    return true;
}

bool publish(CompanionConfigurationCacheSecureStore *store, QSettings *settings,
             const QString &accountIdentity, Loaded *loaded,
             const QJsonObject &payloadObject, qint64 revision, qint64 highWater,
             LoadState *failureState, QString *errorCode)
{
    const QString targetSlot = loaded->authority.committed.present
        ? (loaded->authority.committed.slot == QStringLiteral("a")
               ? QStringLiteral("b") : QStringLiteral("a"))
        : QStringLiteral("a");
    const SlotBytes preimage = targetSlot == QStringLiteral("a")
        ? loaded->slotA : loaded->slotB;
    const QByteArray candidateBytes = makeEnvelope(
        accountIdentity, targetSlot, revision,
        payloadObject, loaded->authority.key);
    Envelope candidate;
    if (candidateBytes.isEmpty()
            || !parseEnvelope(candidateBytes, targetSlot, accountIdentity,
                              loaded->authority.key, &candidate)) {
        fail(errorCode, QStringLiteral("cache-candidate-build-invalid"));
        *failureState = LoadState::Invalid;
        return false;
    }

    Authority preparedAuthority = loaded->authority;
    preparedAuthority.highestReservedRevision = revision;
    preparedAuthority.prepared.present = true;
    preparedAuthority.prepared.targetSlot = targetSlot;
    preparedAuthority.prepared.reservedRevision = revision;
    preparedAuthority.prepared.preimageAbsent = !preimage.present;
    preparedAuthority.prepared.preimageEnvelopeSha = preimage.sha;
    preparedAuthority.prepared.candidateEnvelopeSha = candidate.sha;
    preparedAuthority.prepared.candidatePayloadSha = candidate.payload.sha;
    preparedAuthority.prepared.highWater = highWater;
    preparedAuthority.bytes = authorityBytes(accountIdentity, preparedAuthority);
    AuthorityWriteResult resolution = writeAuthority(
        store, authorityScope(accountIdentity), preparedAuthority.bytes,
        loaded->authority.bytes, false, errorCode);
    if (resolution != AuthorityWriteResult::Expected) {
        *failureState = resolution == AuthorityWriteResult::OutcomeUnknown
            ? LoadState::OutcomeUnknown
            : (resolution == AuthorityWriteResult::Invalid
                ? LoadState::Invalid : LoadState::Unavailable);
        return false;
    }
    cleanse(&loaded->authority.key);
    loaded->authority = preparedAuthority;

    if (!writeSlot(settings, accountIdentity, targetSlot,
                   candidateBytes, preimage, errorCode)) {
        const QString slotFailure = errorCode ? *errorCode : QString();
        SlotBytes observed;
        QString readError;
        if (!readSlot(settings, accountIdentity, targetSlot, &observed, &readError)) {
            *failureState = LoadState::OutcomeUnknown;
            fail(errorCode, readError);
            return false;
        }
        const bool stillPreimage = observed.present == preimage.present
            && (!observed.present || observed.bytes == preimage.bytes);
        if (!stillPreimage) {
            *failureState = observed.present && observed.bytes == candidateBytes
                ? LoadState::RecoveryRequired : LoadState::Invalid;
            fail(errorCode, observed.present && observed.bytes == candidateBytes
                 ? QStringLiteral("cache-candidate-awaits-recovery")
                 : QStringLiteral("cache-slot-third-state"));
            return false;
        }
        Authority aborted = loaded->authority;
        aborted.prepared = Prepared{};
        aborted.bytes = authorityBytes(accountIdentity, aborted);
        resolution = writeAuthority(
            store, authorityScope(accountIdentity), aborted.bytes,
            loaded->authority.bytes, false, errorCode);
        if (resolution == AuthorityWriteResult::Expected) {
            *failureState = LoadState::Unavailable;
            fail(errorCode, slotFailure.isEmpty()
                 ? QStringLiteral("cache-slot-definite-failure") : slotFailure);
        } else {
            *failureState = resolution == AuthorityWriteResult::OutcomeUnknown
                ? LoadState::OutcomeUnknown
                : LoadState::RecoveryRequired;
        }
        return false;
    }

    Authority committed = loaded->authority;
    committed.committed.present = true;
    committed.committed.slot = targetSlot;
    committed.committed.revision = revision;
    committed.committed.payloadSha = candidate.payload.sha;
    committed.committed.envelopeSha = candidate.sha;
    committed.committed.highWater = highWater;
    committed.prepared = Prepared{};
    committed.bytes = authorityBytes(accountIdentity, committed);
    resolution = writeAuthority(
        store, authorityScope(accountIdentity), committed.bytes,
        loaded->authority.bytes, false, errorCode);
    if (resolution != AuthorityWriteResult::Expected) {
        *failureState = resolution == AuthorityWriteResult::OutcomeUnknown
            ? LoadState::OutcomeUnknown
            : (resolution == AuthorityWriteResult::Invalid
                ? LoadState::Invalid : LoadState::RecoveryRequired);
        if (resolution == AuthorityWriteResult::Previous) {
            fail(errorCode, QStringLiteral("cache-candidate-awaits-recovery"));
        }
        return false;
    }
    return true;
}

CompanionConfigurationCacheState publicState(LoadState state)
{
    switch (state) {
    case LoadState::Empty: return CompanionConfigurationCacheState::Empty;
    case LoadState::Ready: return CompanionConfigurationCacheState::Fresh;
    case LoadState::Legacy: return CompanionConfigurationCacheState::LegacyUnverified;
    case LoadState::Invalid: return CompanionConfigurationCacheState::Invalid;
    case LoadState::Unavailable: return CompanionConfigurationCacheState::Unavailable;
    case LoadState::OutcomeUnknown:
        return CompanionConfigurationCacheState::OutcomeUnknown;
    case LoadState::RecoveryRequired:
        return CompanionConfigurationCacheState::RecoveryRequired;
    }
    return CompanionConfigurationCacheState::Invalid;
}

} // namespace

CompanionConfigurationCache::CompanionConfigurationCache(
    CompanionConfigurationCacheSecureStore *secureStore, QSettings *settings,
    const QString &lockFilePath,
    CompanionConfigurationCacheLegacyCleaner *legacyCleaner)
    : m_secureStore(secureStore), m_settings(settings),
      m_lockFilePath(lockFilePath), m_legacyCleaner(legacyCleaner)
{
}

bool CompanionConfigurationCache::commitLiveConfiguration(
    const QString &accountIdentity, const QJsonObject &configurationProjection,
    qint64 nowMs, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    m_lastWarning.clear();
    QString lockError;
    std::unique_ptr<QLockFile> lock = acquireLock(m_lockFilePath, &lockError);
    if (!lock) {
        fail(errorCode, lockError);
        return false;
    }
    QString projectionError;
    qint64 capturedAt = 0;
    if (!validAccountIdentity(accountIdentity)
            || nowMs <= 0 || nowMs > kMaximumJsonInteger
            || !CompanionConfigProjection::validate(
                configurationProjection, &projectionError)
            || configurationProjection.value(QStringLiteral("account_identity"))
                .toString() != accountIdentity
            || !safeInteger(configurationProjection.value(
                QStringLiteral("received_at_ms")), &capturedAt)
            || capturedAt <= 0 || capturedAt > nowMs) {
        fail(errorCode, projectionError.isEmpty()
             ? QStringLiteral("cache-live-configuration-invalid") : projectionError);
        return false;
    }
    Loaded loaded = loadAndRecover(
        m_secureStore, m_settings, accountIdentity);
    if (loaded.state == LoadState::Invalid
            || loaded.state == LoadState::Unavailable
            || loaded.state == LoadState::OutcomeUnknown
            || loaded.state == LoadState::RecoveryRequired) {
        fail(errorCode, loaded.error);
        cleanse(&loaded.authority.key);
        return false;
    }
    if (loaded.state == LoadState::Ready
            && nowMs < loaded.authority.committed.highWater) {
        fail(errorCode, QStringLiteral("cache-clock-rollback"));
        cleanse(&loaded.authority.key);
        return false;
    }
    const QJsonObject configuration = sanitizedConfiguration(configurationProjection);
    QString contentSha;
    if (!validateConfiguration(configuration, accountIdentity, &contentSha)) {
        fail(errorCode, QStringLiteral("cache-configuration-sanitization-failed"));
        cleanse(&loaded.authority.key);
        return false;
    }
    const QString sourceObservationSha = configurationProjection.value(
        QStringLiteral("projection_sha256")).toString();
    if (loaded.state == LoadState::Ready
            && loaded.current.payload.contentSha == contentSha
            && loaded.current.payload.sourceObservationSha == sourceObservationSha) {
        if (nowMs > loaded.authority.committed.highWater) {
            Authority advanced = loaded.authority;
            advanced.committed.highWater = nowMs;
            advanced.bytes = authorityBytes(accountIdentity, advanced);
            const AuthorityWriteResult result = writeAuthority(
                m_secureStore, authorityScope(accountIdentity), advanced.bytes,
                loaded.authority.bytes, false, errorCode);
            if (result != AuthorityWriteResult::Expected) {
                if (result == AuthorityWriteResult::OutcomeUnknown) {
                    m_outcomeUnknownAccounts.insert(accountIdentity);
                }
                cleanse(&loaded.authority.key);
                return false;
            }
        }
        removeLegacyExact(m_settings, m_legacyCleaner,
                          legacySetting(accountIdentity), &m_lastWarning);
        m_outcomeUnknownAccounts.remove(accountIdentity);
        cleanse(&loaded.authority.key);
        if (errorCode) errorCode->clear();
        return true;
    }
    if (loaded.state == LoadState::Ready
            && capturedAt < loaded.authority.committed.highWater) {
        fail(errorCode, QStringLiteral("cache-live-observation-before-high-water"));
        cleanse(&loaded.authority.key);
        return false;
    }
    qint64 validUntil = 0;
    qint64 staleUntil = 0;
    if (!addMilliseconds(capturedAt, ConfigurationFreshMs, &validUntil)
            || !addMilliseconds(validUntil, ConfigurationStaleMs, &staleUntil)
            || loaded.authority.highestReservedRevision >= kMaximumJsonInteger) {
        fail(errorCode, QStringLiteral("cache-time-or-revision-exhausted"));
        cleanse(&loaded.authority.key);
        return false;
    }
    const qint64 revision = loaded.authority.highestReservedRevision + 1;
    QJsonArray models;
    const QJsonObject payload = makePayload(
        accountIdentity, revision,
        loaded.state == LoadState::Ready ? loaded.current.payload.sha : QString(),
        sourceObservationSha,
        contentSha, capturedAt, validUntil, staleUntil, nowMs,
        configuration, models);
    Payload checked;
    if (!parsePayload(payload, accountIdentity, revision, &checked)) {
        fail(errorCode, QStringLiteral("cache-payload-invalid"));
        cleanse(&loaded.authority.key);
        return false;
    }
    if (loaded.authority.bytes.isEmpty()) {
        LoadState initializationFailure = LoadState::Unavailable;
        if (!initializeAuthority(m_secureStore, accountIdentity,
                                 &loaded.authority, &initializationFailure,
                                 errorCode)) {
            if (initializationFailure == LoadState::OutcomeUnknown) {
                m_outcomeUnknownAccounts.insert(accountIdentity);
            }
            return false;
        }
    }
    LoadState failure = LoadState::Unavailable;
    const bool success = publish(
        m_secureStore, m_settings, accountIdentity, &loaded,
        payload, revision, nowMs, &failure, errorCode);
    cleanse(&loaded.authority.key);
    if (!success && failure == LoadState::OutcomeUnknown) {
        m_outcomeUnknownAccounts.insert(accountIdentity);
    } else if (success) {
        removeLegacyExact(m_settings, m_legacyCleaner,
                          legacySetting(accountIdentity), &m_lastWarning);
        m_outcomeUnknownAccounts.remove(accountIdentity);
        if (errorCode) errorCode->clear();
    }
    return success;
}

bool CompanionConfigurationCache::mergeWebsiteModels(
    const QString &accountIdentity,
    const QString &configurationObservationSha256,
    const QString &platform,
    const QJsonObject &modelProjection, qint64 capturedAt,
    qint64 nowMs, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    QString lockError;
    std::unique_ptr<QLockFile> lock = acquireLock(m_lockFilePath, &lockError);
    if (!lock) {
        fail(errorCode, lockError);
        return false;
    }
    QString projectionError;
    if (!validAccountIdentity(accountIdentity)
            || !lowerHex(configurationObservationSha256)
            || !validModelPlatform(platform)
            || capturedAt <= 0 || capturedAt > nowMs
            || nowMs <= 0 || nowMs > kMaximumJsonInteger
            || !CompanionModelProjection::validate(
                modelProjection, &projectionError)) {
        fail(errorCode, projectionError.isEmpty()
             ? QStringLiteral("cache-model-observation-invalid") : projectionError);
        return false;
    }
    const QString keyIdentity = modelProjection.value(
        QStringLiteral("key_identity")).toString();
    if (!validWebsiteKeyIdentity(keyIdentity)) {
        fail(errorCode, QStringLiteral("cache-model-source-not-website"));
        return false;
    }
    Loaded loaded = loadAndRecover(m_secureStore, m_settings, accountIdentity);
    if (loaded.state != LoadState::Ready) {
        fail(errorCode, loaded.error.isEmpty()
             ? QStringLiteral("cache-model-configuration-unavailable") : loaded.error);
        cleanse(&loaded.authority.key);
        return false;
    }
    if (nowMs < loaded.authority.committed.highWater) {
        fail(errorCode, QStringLiteral("cache-clock-rollback"));
        cleanse(&loaded.authority.key);
        return false;
    }
    if (capturedAt < loaded.authority.committed.highWater) {
        fail(errorCode, QStringLiteral("cache-model-observation-before-high-water"));
        cleanse(&loaded.authority.key);
        return false;
    }
    if (nowMs >= loaded.current.payload.validUntil
            || capturedAt < loaded.current.payload.capturedAt
            || configurationObservationSha256
                != loaded.current.payload.sourceObservationSha) {
        fail(errorCode, QStringLiteral("cache-model-configuration-binding-invalid"));
        cleanse(&loaded.authority.key);
        return false;
    }
    bool configured = false;
    QString configuredPlatform;
    for (const QJsonValue &value : loaded.current.payload.configuration
             .value(QStringLiteral("keys")).toArray()) {
        if (value.toObject().value(QStringLiteral("key_identity")).toString()
                == keyIdentity) {
            configured = true;
            configuredPlatform = value.toObject()
                .value(QStringLiteral("platform")).toString();
            break;
        }
    }
    if (!configured) {
        fail(errorCode, QStringLiteral("cache-model-key-not-configured"));
        cleanse(&loaded.authority.key);
        return false;
    }
    if (configuredPlatform != platform) {
        fail(errorCode, QStringLiteral("cache-model-platform-binding-invalid"));
        cleanse(&loaded.authority.key);
        return false;
    }
    qint64 modelValidUntil = 0;
    if (!addMilliseconds(capturedAt, ModelFreshMs, &modelValidUntil)
            || loaded.authority.highestReservedRevision >= kMaximumJsonInteger) {
        fail(errorCode, QStringLiteral("cache-model-time-or-revision-exhausted"));
        cleanse(&loaded.authority.key);
        return false;
    }
    modelValidUntil = std::min(modelValidUntil, loaded.current.payload.validUntil);
    if (modelValidUntil <= capturedAt) {
        fail(errorCode, QStringLiteral("cache-model-validity-empty"));
        cleanse(&loaded.authority.key);
        return false;
    }
    QJsonArray models;
    for (const QJsonValue &value : loaded.current.payload.models) {
        const QJsonObject existing = value.toObject();
        qint64 existingValidUntil = 0;
        if (existing.value(QStringLiteral("key_identity")).toString() != keyIdentity
                && safeInteger(existing.value(QStringLiteral("valid_until_ms")),
                               &existingValidUntil)
                && nowMs < existingValidUntil) {
            models.append(existing);
        }
    }
    models.append(QJsonObject{
        { QStringLiteral("key_identity"), keyIdentity },
        { QStringLiteral("platform"), platform },
        { QStringLiteral("configuration_observation_sha256"),
          configurationObservationSha256 },
        { QStringLiteral("source_observation_sha256"),
          modelProjection.value(QStringLiteral("projection_sha256")) },
        { QStringLiteral("captured_at_ms"), capturedAt },
        { QStringLiteral("valid_until_ms"), modelValidUntil },
        { QStringLiteral("model_count"),
          modelProjection.value(QStringLiteral("model_count")) },
        { QStringLiteral("models"), modelProjection.value(QStringLiteral("models")) },
    });
    const qint64 revision = loaded.authority.highestReservedRevision + 1;
    const QJsonObject payload = makePayload(
        accountIdentity, revision, loaded.current.payload.sha,
        loaded.current.payload.sourceObservationSha,
        loaded.current.payload.contentSha, loaded.current.payload.capturedAt,
        loaded.current.payload.validUntil, loaded.current.payload.staleUntil,
        nowMs, loaded.current.payload.configuration, models);
    Payload checked;
    if (!parsePayload(payload, accountIdentity, revision, &checked)) {
        fail(errorCode, QStringLiteral("cache-model-payload-invalid"));
        cleanse(&loaded.authority.key);
        return false;
    }
    LoadState failure = LoadState::Unavailable;
    const bool success = publish(
        m_secureStore, m_settings, accountIdentity, &loaded,
        payload, revision, nowMs, &failure, errorCode);
    cleanse(&loaded.authority.key);
    if (!success && failure == LoadState::OutcomeUnknown) {
        m_outcomeUnknownAccounts.insert(accountIdentity);
    } else if (success) {
        m_outcomeUnknownAccounts.remove(accountIdentity);
        if (errorCode) errorCode->clear();
    }
    return success;
}

CompanionConfigurationCacheView CompanionConfigurationCache::view(
    const QString &accountIdentity, qint64 nowMs)
{
    CompanionConfigurationCacheView view;
    if (!validAccountIdentity(accountIdentity)
            || nowMs <= 0 || nowMs > kMaximumJsonInteger) {
        view.state = CompanionConfigurationCacheState::Invalid;
        view.errorCode = QStringLiteral("cache-view-arguments-invalid");
        return view;
    }
    QString lockError;
    std::unique_ptr<QLockFile> lock = acquireLock(m_lockFilePath, &lockError);
    if (!lock) {
        view.state = CompanionConfigurationCacheState::Unavailable;
        view.errorCode = lockError;
        return view;
    }
    Loaded loaded = loadAndRecover(m_secureStore, m_settings, accountIdentity);
    if (loaded.state != LoadState::Ready) {
        view.state = publicState(loaded.state);
        if (loaded.state == LoadState::Unavailable
                && m_outcomeUnknownAccounts.contains(accountIdentity)) {
            view.state = CompanionConfigurationCacheState::OutcomeUnknown;
        }
        view.errorCode = loaded.error;
        cleanse(&loaded.authority.key);
        return view;
    }
    m_outcomeUnknownAccounts.remove(accountIdentity);
    if (nowMs < loaded.authority.committed.highWater) {
        view.state = CompanionConfigurationCacheState::Invalid;
        view.errorCode = QStringLiteral("cache-clock-rollback");
        cleanse(&loaded.authority.key);
        return view;
    }
    if (nowMs > loaded.authority.committed.highWater) {
        Authority advanced = loaded.authority;
        advanced.committed.highWater = nowMs;
        advanced.bytes = authorityBytes(accountIdentity, advanced);
        QString error;
        const AuthorityWriteResult result = writeAuthority(
            m_secureStore, authorityScope(accountIdentity), advanced.bytes,
            loaded.authority.bytes, false, &error);
        if (result != AuthorityWriteResult::Expected) {
            view.state = result == AuthorityWriteResult::OutcomeUnknown
                ? CompanionConfigurationCacheState::OutcomeUnknown
                : (result == AuthorityWriteResult::Previous
                    ? CompanionConfigurationCacheState::Unavailable
                    : CompanionConfigurationCacheState::Invalid);
            view.errorCode = error;
            if (result == AuthorityWriteResult::OutcomeUnknown) {
                m_outcomeUnknownAccounts.insert(accountIdentity);
            }
            cleanse(&loaded.authority.key);
            return view;
        }
    }
    view.revision = loaded.current.revision;
    view.capturedAtMs = loaded.current.payload.capturedAt;
    view.validUntilMs = loaded.current.payload.validUntil;
    view.staleUntilMs = loaded.current.payload.staleUntil;
    view.sourceObservationSha256 = loaded.current.payload.sourceObservationSha;
    view.contentSha256 = loaded.current.payload.contentSha;
    if (nowMs < view.validUntilMs) {
        view.state = CompanionConfigurationCacheState::Fresh;
        view.configuration = loaded.current.payload.configuration;
        for (const QJsonValue &value : loaded.current.payload.models) {
            qint64 validUntil = 0;
            const QJsonObject model = value.toObject();
            if (safeInteger(model.value(QStringLiteral("valid_until_ms")), &validUntil)
                    && nowMs < validUntil) {
                view.models.append(model);
            }
        }
    } else if (nowMs < view.staleUntilMs) {
        view.state = CompanionConfigurationCacheState::Stale;
        view.configuration = loaded.current.payload.configuration;
    } else {
        view.state = CompanionConfigurationCacheState::Expired;
    }
    cleanse(&loaded.authority.key);
    return view;
}

QString CompanionConfigurationCache::lastWarning() const
{
    return m_lastWarning;
}
