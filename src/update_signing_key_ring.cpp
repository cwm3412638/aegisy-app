#include "update_signing_key_ring.h"

#include "aap_transport_runtime.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <openssl/evp.h>

#include <cmath>

namespace UpdateSigningKeyRing {

namespace {

constexpr double kMaximumSafeJsonInteger = 9007199254740991.0;
constexpr qsizetype kMaximumEnvelopeBytes = 128 * 1024;
constexpr int kMaximumKeys = 32;
const QString kRingSchema =
    QStringLiteral("aegisy-update-signing-key-ring/0.1");
const QString kEnvelopeSchema =
    QStringLiteral("aegisy-update-signing-key-ring-signature/0.1");
const QString kAnchorSchema =
    QStringLiteral("aegisy-update-signing-trust-anchor/0.1");
const QString kArtifactUsage = QStringLiteral("artifact-set");
const QString kRingUsage = QStringLiteral("key-ring");

bool fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
    return false;
}

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &keys)
{
    if (object.size() != keys.size()) return false;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!keys.contains(it.key())) return false;
    }
    return true;
}

bool exactString(const QJsonObject &object, const QString &key, QString *value)
{
    const QJsonValue field = object.value(key);
    if (!field.isString()) return false;
    *value = field.toString();
    return true;
}

bool safePositiveInteger(const QJsonValue &value, quint64 *output)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number <= 0 || number > kMaximumSafeJsonInteger) {
        return false;
    }
    *output = static_cast<quint64>(number);
    return true;
}

bool validNow(qint64 nowMs)
{
    return nowMs > 0
        && static_cast<quint64>(nowMs)
            <= static_cast<quint64>(kMaximumSafeJsonInteger);
}

bool validKeyId(const QString &keyId)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[a-z0-9][a-z0-9._-]{0,63}\\z"));
    return pattern.match(keyId).hasMatch();
}

bool validIdentity(const QString &identity, const QString &prefix)
{
    static const QRegularExpression hashPattern(
        QStringLiteral("\\A[0-9a-f]{64}\\z"));
    return identity.startsWith(prefix)
        && hashPattern.match(identity.mid(prefix.size())).hasMatch();
}

QByteArray decodeCanonicalBase64(const QString &encoded, qsizetype expectedBytes)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[A-Za-z0-9+/]+={0,2}\\z"));
    if (!pattern.match(encoded).hasMatch()) return {};
    const QByteArray decoded = QByteArray::fromBase64(encoded.toLatin1());
    if (decoded.size() != expectedBytes
        || decoded.toBase64() != encoded.toLatin1()) {
        return {};
    }
    return decoded;
}

enum class SignatureVerification {
    Valid,
    Invalid,
    Unavailable,
};

SignatureVerification verifyEd25519(const QByteArray &publicKey,
                                    const QByteArray &signature,
                                    const QByteArray &payload)
{
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(publicKey.constData()),
        static_cast<size_t>(publicKey.size()));
    if (!key) return SignatureVerification::Unavailable;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context
        || EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) != 1) {
        EVP_MD_CTX_free(context);
        EVP_PKEY_free(key);
        return SignatureVerification::Unavailable;
    }
    const int result = EVP_DigestVerify(
        context,
        reinterpret_cast<const unsigned char *>(signature.constData()),
        static_cast<size_t>(signature.size()),
        reinterpret_cast<const unsigned char *>(payload.constData()),
        static_cast<size_t>(payload.size()));
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (result == 1) return SignatureVerification::Valid;
    if (result == 0) return SignatureVerification::Invalid;
    return SignatureVerification::Unavailable;
}

void appendLine(QByteArray *payload, const QByteArray &key,
                const QString &value)
{
    payload->append(key);
    payload->append('=');
    payload->append(value.toUtf8());
    payload->append('\n');
}

void appendLine(QByteArray *payload, const QByteArray &key, quint64 value)
{
    payload->append(key);
    payload->append('=');
    payload->append(QByteArray::number(value));
    payload->append('\n');
}

void appendLine(QByteArray *payload, const QByteArray &key, bool value)
{
    appendLine(payload, key, static_cast<quint64>(value ? 1 : 0));
}

QString sha256Identity(const QByteArray &payload, const QString &prefix)
{
    return prefix + QString::fromLatin1(QCryptographicHash::hash(
        payload, QCryptographicHash::Sha256).toHex());
}

} // namespace

struct KeyRecord
{
    QString keyId;
    QString publicKeyBase64;
    QByteArray publicKey;
    quint64 validFromMs = 0;
    quint64 validUntilMs = 0;
    bool revoked = false;
    QString replaces;
    QStringList usages;
    QString keyIdentity;
};

struct Ring
{
    quint64 generation = 0;
    QList<KeyRecord> keys;
    QString ringIdentity;
};

struct SignedRing
{
    QString signerKeyId;
    quint64 signedAtMs = 0;
    Ring ring;
    QString payloadIdentity;
    QString signatureBase64;
    QByteArray payload;
    QString envelopeIdentity;
};

struct KeyAdmission
{
    quint64 generation = 0;
    quint64 admittedAtMs = 0;
};

struct TrustAnchorAuthority::Data
{
    QString keyId;
    QString publicKeyBase64;
    QByteArray publicKey;
    QString anchorIdentity;
};

struct Authority::Data
{
    QString trustAnchorKeyId;
    QString trustAnchorPublicKeyBase64;
    QString trustAnchorIdentity;
    Ring ring;
    quint64 signedAtMs = 0;
    QMap<QString, KeyAdmission> keyAdmissions;
    QString keyAdmissionsIdentity;
    QString signedEnvelopeIdentity;
    QString authorityIdentity;
};

namespace {

void appendKeyFields(QByteArray *payload, const QByteArray &prefix,
                     const KeyRecord &key, bool includeIdentity)
{
    appendLine(payload, prefix + ".key_id", key.keyId);
    appendLine(payload, prefix + ".public_key_base64", key.publicKeyBase64);
    appendLine(payload, prefix + ".valid_from_ms", key.validFromMs);
    appendLine(payload, prefix + ".valid_until_ms", key.validUntilMs);
    appendLine(payload, prefix + ".revoked", key.revoked);
    appendLine(payload, prefix + ".replaces.present", !key.replaces.isEmpty());
    if (!key.replaces.isEmpty()) {
        appendLine(payload, prefix + ".replaces", key.replaces);
    }
    appendLine(payload, prefix + ".usages.count",
               static_cast<quint64>(key.usages.size()));
    for (int index = 0; index < key.usages.size(); ++index) {
        appendLine(payload,
                   prefix + ".usages." + QByteArray::number(index),
                   key.usages.at(index));
    }
    if (includeIdentity) {
        appendLine(payload, prefix + ".key_identity", key.keyIdentity);
    }
}

QString keyIdentity(const KeyRecord &key)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key/0.1\n");
    appendKeyFields(&payload, QByteArrayLiteral("key"), key, false);
    return sha256Identity(payload,
                          QStringLiteral("update-signing-key:sha256:"));
}

QString ringIdentity(const Ring &ring)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring/0.1\n");
    appendLine(&payload, QByteArrayLiteral("generation"), ring.generation);
    appendLine(&payload, QByteArrayLiteral("keys.count"),
               static_cast<quint64>(ring.keys.size()));
    for (int index = 0; index < ring.keys.size(); ++index) {
        appendKeyFields(&payload,
                        QByteArrayLiteral("keys.") + QByteArray::number(index),
                        ring.keys.at(index), true);
    }
    return sha256Identity(payload,
                          QStringLiteral("update-signing-key-ring:sha256:"));
}

QByteArray signedRingPayload(const SignedRing &signedRing)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring-signature/0.1\n");
    appendLine(&payload, QByteArrayLiteral("signer_key_id"),
               signedRing.signerKeyId);
    appendLine(&payload, QByteArrayLiteral("signed_at_ms"),
               signedRing.signedAtMs);
    appendLine(&payload, QByteArrayLiteral("key_ring.generation"),
               signedRing.ring.generation);
    appendLine(&payload, QByteArrayLiteral("key_ring.identity"),
               signedRing.ring.ringIdentity);
    return payload;
}

QString signedEnvelopeIdentity(const SignedRing &signedRing)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring-envelope/0.1\n");
    appendLine(&payload, QByteArrayLiteral("signing_payload.identity"),
               signedRing.payloadIdentity);
    appendLine(&payload, QByteArrayLiteral("signature_base64"),
               signedRing.signatureBase64);
    return sha256Identity(
        payload, QStringLiteral("update-signing-key-ring-envelope:sha256:"));
}

QString anchorIdentity(const TrustAnchorAuthority::Data &anchor)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-trust-anchor/0.1\n");
    appendLine(&payload, QByteArrayLiteral("key_id"), anchor.keyId);
    appendLine(&payload, QByteArrayLiteral("public_key_base64"),
               anchor.publicKeyBase64);
    return sha256Identity(payload,
                          QStringLiteral("update-signing-trust-anchor:sha256:"));
}

QString keyAdmissionsIdentity(const Authority::Data &authority)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-admissions/0.1\n");
    appendLine(&payload, QByteArrayLiteral("latest_signed_at_ms"),
               authority.signedAtMs);
    appendLine(&payload, QByteArrayLiteral("keys.count"),
               static_cast<quint64>(authority.ring.keys.size()));
    for (int index = 0; index < authority.ring.keys.size(); ++index) {
        const KeyRecord &key = authority.ring.keys.at(index);
        const KeyAdmission admission = authority.keyAdmissions.value(key.keyId);
        const QByteArray prefix =
            QByteArrayLiteral("keys.") + QByteArray::number(index);
        appendLine(&payload, prefix + ".key_id", key.keyId);
        appendLine(&payload, prefix + ".admitted_generation",
                   admission.generation);
        appendLine(&payload, prefix + ".admitted_at_ms",
                   admission.admittedAtMs);
    }
    return sha256Identity(
        payload, QStringLiteral("update-signing-key-admissions:sha256:"));
}

QString authorityIdentity(const Authority::Data &authority)
{
    QByteArray payload = authority.ring.generation == 1
        ? QByteArrayLiteral("aegisy-update-signing-key-ring-authority/0.1\n")
        : QByteArrayLiteral("aegisy-update-signing-key-ring-authority/0.2\n");
    appendLine(&payload, QByteArrayLiteral("trust_anchor.identity"),
               authority.trustAnchorIdentity);
    appendLine(&payload, QByteArrayLiteral("key_ring.generation"),
               authority.ring.generation);
    appendLine(&payload, QByteArrayLiteral("key_ring.identity"),
               authority.ring.ringIdentity);
    appendLine(&payload, QByteArrayLiteral("signed_envelope.identity"),
               authority.signedEnvelopeIdentity);
    if (authority.ring.generation >= 2) {
        appendLine(&payload, QByteArrayLiteral("key_admissions.identity"),
                   authority.keyAdmissionsIdentity);
    }
    return sha256Identity(
        payload, QStringLiteral("update-signing-key-ring-authority:sha256:"));
}

bool parseUsageArray(const QJsonValue &value, QStringList *usages,
                     QString *errorCode)
{
    if (!value.isArray()) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-usage-invalid"));
    }
    const QJsonArray array = value.toArray();
    if (array.isEmpty() || array.size() > 2) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-usage-invalid"));
    }
    QString previous;
    for (const QJsonValue &entry : array) {
        if (!entry.isString()) {
            return fail(errorCode,
                        QStringLiteral("update-signing-key-usage-invalid"));
        }
        const QString usage = entry.toString();
        if ((usage != kArtifactUsage && usage != kRingUsage)
            || (!previous.isEmpty() && usage <= previous)) {
            return fail(errorCode,
                        QStringLiteral("update-signing-key-usage-invalid"));
        }
        usages->append(usage);
        previous = usage;
    }
    return true;
}

bool parseKey(const QJsonValue &value, KeyRecord *key, QString *errorCode)
{
    if (!value.isObject()) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-fields-invalid"));
    }
    const QJsonObject object = value.toObject();
    if (!hasExactKeys(object, {
            QStringLiteral("key_id"),
            QStringLiteral("public_key_base64"),
            QStringLiteral("valid_from_ms"),
            QStringLiteral("valid_until_ms"),
            QStringLiteral("revoked"),
            QStringLiteral("replaces"),
            QStringLiteral("usages"),
            QStringLiteral("key_identity"),
        })
        || !exactString(object, QStringLiteral("key_id"), &key->keyId)
        || !validKeyId(key->keyId)
        || !exactString(object, QStringLiteral("public_key_base64"),
                        &key->publicKeyBase64)
        || !safePositiveInteger(object.value(QStringLiteral("valid_from_ms")),
                                &key->validFromMs)
        || !safePositiveInteger(object.value(QStringLiteral("valid_until_ms")),
                                &key->validUntilMs)
        || key->validUntilMs <= key->validFromMs
        || !object.value(QStringLiteral("revoked")).isBool()
        || !parseUsageArray(object.value(QStringLiteral("usages")),
                            &key->usages, errorCode)
        || !exactString(object, QStringLiteral("key_identity"),
                        &key->keyIdentity)
        || !validIdentity(key->keyIdentity,
                          QStringLiteral("update-signing-key:sha256:"))) {
        if (errorCode && errorCode->isEmpty()) {
            *errorCode = QStringLiteral("update-signing-key-fields-invalid");
        }
        return false;
    }
    key->revoked = object.value(QStringLiteral("revoked")).toBool();
    const QJsonValue replaces = object.value(QStringLiteral("replaces"));
    if (replaces.isNull()) {
        key->replaces.clear();
    } else if (replaces.isString() && validKeyId(replaces.toString())) {
        key->replaces = replaces.toString();
    } else {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-lineage-invalid"));
    }
    if (key->replaces == key->keyId) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-lineage-invalid"));
    }
    key->publicKey = decodeCanonicalBase64(key->publicKeyBase64, 32);
    if (key->publicKey.isEmpty()) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-encoding-invalid"));
    }
    if (key->keyIdentity != keyIdentity(*key)) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-identity-mismatch"));
    }
    return true;
}

bool validateLineage(const Ring &ring, QString *errorCode)
{
    QMap<QString, const KeyRecord *> byId;
    QMap<QString, int> childCounts;
    for (const KeyRecord &key : ring.keys) byId.insert(key.keyId, &key);
    for (const KeyRecord &key : ring.keys) {
        if (key.replaces.isEmpty()) continue;
        if (!byId.contains(key.replaces)) {
            return fail(errorCode,
                        QStringLiteral("update-signing-key-lineage-unknown"));
        }
        const int children = childCounts.value(key.replaces) + 1;
        childCounts.insert(key.replaces, children);
        if (children > 1) {
            return fail(errorCode,
                        QStringLiteral("update-signing-key-lineage-branch"));
        }
        QSet<QString> visited;
        QString current = key.keyId;
        while (!current.isEmpty()) {
            if (visited.contains(current)) {
                return fail(errorCode,
                            QStringLiteral("update-signing-key-lineage-cycle"));
            }
            visited.insert(current);
            const KeyRecord *currentKey = byId.value(current, nullptr);
            if (!currentKey) {
                return fail(errorCode,
                            QStringLiteral("update-signing-key-lineage-unknown"));
            }
            current = currentKey->replaces;
        }
    }
    return true;
}

bool validateRing(const Ring &ring, QString *errorCode)
{
    if (ring.generation == 0
        || ring.generation
            > static_cast<quint64>(kMaximumSafeJsonInteger)) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-generation-invalid"));
    }
    if (ring.keys.isEmpty() || ring.keys.size() > kMaximumKeys) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-size-invalid"));
    }
    QString previousId;
    QSet<QByteArray> publicKeys;
    bool hasArtifactKey = false;
    bool hasRingKey = false;
    for (const KeyRecord &key : ring.keys) {
        if ((!previousId.isEmpty() && key.keyId <= previousId)
            || publicKeys.contains(key.publicKey)) {
            return fail(
                errorCode,
                publicKeys.contains(key.publicKey)
                    ? QStringLiteral("update-signing-key-duplicate-public-key")
                    : QStringLiteral("update-signing-key-ring-order-invalid"));
        }
        publicKeys.insert(key.publicKey);
        previousId = key.keyId;
        if (!key.revoked && key.usages.contains(kArtifactUsage)) {
            hasArtifactKey = true;
        }
        if (!key.revoked && key.usages.contains(kRingUsage)) hasRingKey = true;
    }
    if (!hasArtifactKey || !hasRingKey) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-no-active-usage"));
    }
    if (!validateLineage(ring, errorCode)) return false;
    if (ring.ringIdentity != ringIdentity(ring)) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-identity-mismatch"));
    }
    return true;
}

bool parseRing(const QJsonValue &value, Ring *ring, QString *errorCode)
{
    if (!value.isObject()) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-fields-invalid"));
    }
    const QJsonObject object = value.toObject();
    QString schema;
    if (!hasExactKeys(object, {
            QStringLiteral("schema_version"),
            QStringLiteral("generation"),
            QStringLiteral("keys"),
            QStringLiteral("ring_identity"),
        })
        || !exactString(object, QStringLiteral("schema_version"), &schema)
        || schema != kRingSchema
        || !safePositiveInteger(object.value(QStringLiteral("generation")),
                                &ring->generation)
        || !object.value(QStringLiteral("keys")).isArray()
        || !exactString(object, QStringLiteral("ring_identity"),
                        &ring->ringIdentity)
        || !validIdentity(ring->ringIdentity,
                          QStringLiteral("update-signing-key-ring:sha256:"))) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-fields-invalid"));
    }
    const QJsonArray keys = object.value(QStringLiteral("keys")).toArray();
    if (keys.isEmpty() || keys.size() > kMaximumKeys) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-size-invalid"));
    }
    for (const QJsonValue &keyValue : keys) {
        KeyRecord key;
        if (!parseKey(keyValue, &key, errorCode)) return false;
        ring->keys.append(key);
    }
    return validateRing(*ring, errorCode);
}

bool parseSignedRing(const QByteArray &bytes, qint64 nowMs,
                     SignedRing *signedRing, QString *errorCode)
{
    if (!validNow(nowMs)) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-clock-invalid"));
    }
    if (bytes.isEmpty() || bytes.size() > kMaximumEnvelopeBytes) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-json-size-invalid"));
    }
    using namespace aegisy::aap::transport_runtime;
    TransportJsonValue parsed;
    QString parseError;
    if (!parseTransportJsonRaw(bytes, &parsed, &parseError)) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-json-invalid"));
    }
    QJsonValue projected;
    TransportProjectionError projectionError = TransportProjectionError::None;
    if (!projectJsonSafeTransportValue(parsed, &projected, &projectionError)
        || !projected.isObject()) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-json-invalid"));
    }
    const QJsonObject object = projected.toObject();
    QString schema;
    if (!hasExactKeys(object, {
            QStringLiteral("schema_version"),
            QStringLiteral("signer_key_id"),
            QStringLiteral("signed_at_ms"),
            QStringLiteral("key_ring"),
            QStringLiteral("payload_identity"),
            QStringLiteral("signature"),
        })
        || !exactString(object, QStringLiteral("schema_version"), &schema)
        || schema != kEnvelopeSchema
        || !exactString(object, QStringLiteral("signer_key_id"),
                        &signedRing->signerKeyId)
        || !validKeyId(signedRing->signerKeyId)
        || !safePositiveInteger(object.value(QStringLiteral("signed_at_ms")),
                                &signedRing->signedAtMs)
        || signedRing->signedAtMs > static_cast<quint64>(nowMs)
        || !parseRing(object.value(QStringLiteral("key_ring")),
                      &signedRing->ring, errorCode)
        || !exactString(object, QStringLiteral("payload_identity"),
                        &signedRing->payloadIdentity)
        || !validIdentity(
            signedRing->payloadIdentity,
            QStringLiteral("update-signing-key-ring-payload:sha256:"))
        || !exactString(object, QStringLiteral("signature"),
                        &signedRing->signatureBase64)
        || decodeCanonicalBase64(signedRing->signatureBase64, 64).isEmpty()) {
        if (errorCode && errorCode->isEmpty()) {
            *errorCode = QStringLiteral(
                "update-signing-key-ring-envelope-fields-invalid");
        }
        return false;
    }
    signedRing->payload = signedRingPayload(*signedRing);
    const QString expectedPayloadIdentity = sha256Identity(
        signedRing->payload,
        QStringLiteral("update-signing-key-ring-payload:sha256:"));
    if (signedRing->payloadIdentity != expectedPayloadIdentity) {
        return fail(errorCode,
                    QStringLiteral(
                        "update-signing-key-ring-payload-identity-mismatch"));
    }
    signedRing->envelopeIdentity = signedEnvelopeIdentity(*signedRing);
    return true;
}

const KeyRecord *findKey(const Ring &ring, const QString &keyId)
{
    for (const KeyRecord &key : ring.keys) {
        if (key.keyId == keyId) return &key;
    }
    return nullptr;
}

bool keyActiveAt(const KeyRecord &key, quint64 atMs)
{
    return !key.revoked && atMs >= key.validFromMs && atMs < key.validUntilMs;
}

bool hasActiveUsageAt(const Ring &ring, const QString &usage, quint64 atMs)
{
    for (const KeyRecord &key : ring.keys) {
        if (key.usages.contains(usage) && keyActiveAt(key, atMs)) return true;
    }
    return false;
}

bool verifySignature(const QByteArray &publicKey, const QByteArray &payload,
                     const QString &encodedSignature, QString *errorCode,
                     const QString &invalidCode,
                     const QString &unavailableCode)
{
    const QByteArray signature = decodeCanonicalBase64(encodedSignature, 64);
    if (signature.isEmpty()) {
        return fail(errorCode,
                    QStringLiteral("update-signing-signature-encoding-invalid"));
    }
    const SignatureVerification result = verifyEd25519(
        publicKey, signature, payload);
    if (result == SignatureVerification::Unavailable) {
        return fail(errorCode, unavailableCode);
    }
    if (result == SignatureVerification::Invalid) {
        return fail(errorCode, invalidCode);
    }
    return true;
}

bool validAnchor(const TrustAnchorAuthority::Data &anchor, QString *errorCode)
{
    if (!validKeyId(anchor.keyId)
        || anchor.publicKey.size() != 32
        || anchor.publicKey.toBase64() != anchor.publicKeyBase64
        || anchor.anchorIdentity != anchorIdentity(anchor)) {
        return fail(errorCode,
                    QStringLiteral("update-signing-trust-anchor-invalid"));
    }
    return true;
}

bool validKeyAdmissions(const Authority::Data &authority, QString *errorCode)
{
    if (authority.signedAtMs == 0
        || authority.signedAtMs
            > static_cast<quint64>(kMaximumSafeJsonInteger)
        || authority.keyAdmissions.size() != authority.ring.keys.size()) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-admissions-invalid"));
    }
    for (const KeyRecord &key : authority.ring.keys) {
        const auto admissionIt = authority.keyAdmissions.constFind(key.keyId);
        if (admissionIt == authority.keyAdmissions.constEnd()) {
            return fail(
                errorCode,
                QStringLiteral("update-signing-key-admissions-invalid"));
        }
        const KeyAdmission &admission = admissionIt.value();
        if (admission.generation == 0
            || admission.generation > authority.ring.generation
            || admission.admittedAtMs == 0
            || admission.admittedAtMs > authority.signedAtMs
            || (admission.generation == authority.ring.generation
                && admission.admittedAtMs != authority.signedAtMs)) {
            return fail(
                errorCode,
                QStringLiteral("update-signing-key-admissions-invalid"));
        }
        if (key.replaces.isEmpty()) {
            if (key.keyId != authority.trustAnchorKeyId
                || admission.generation != 1) {
                return fail(
                    errorCode,
                    QStringLiteral("update-signing-key-admissions-invalid"));
            }
        } else {
            const auto parentIt = authority.keyAdmissions.constFind(
                key.replaces);
            if (parentIt == authority.keyAdmissions.constEnd()
                || parentIt->generation >= admission.generation
                || parentIt->admittedAtMs > admission.admittedAtMs) {
                return fail(
                    errorCode,
                    QStringLiteral("update-signing-key-admissions-invalid"));
            }
        }
    }
    if (!validIdentity(
            authority.keyAdmissionsIdentity,
            QStringLiteral("update-signing-key-admissions:sha256:"))
        || authority.keyAdmissionsIdentity
            != keyAdmissionsIdentity(authority)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-admissions-identity-mismatch"));
    }
    return true;
}

bool validAuthority(const Authority::Data &authority, QString *errorCode)
{
    TrustAnchorAuthority::Data anchor;
    anchor.keyId = authority.trustAnchorKeyId;
    anchor.publicKeyBase64 = authority.trustAnchorPublicKeyBase64;
    anchor.publicKey = decodeCanonicalBase64(anchor.publicKeyBase64, 32);
    anchor.anchorIdentity = authority.trustAnchorIdentity;
    if (!validAnchor(anchor, errorCode)
        || !validateRing(authority.ring, errorCode)
        || !validKeyAdmissions(authority, errorCode)) {
        return false;
    }
    const KeyRecord *root = findKey(authority.ring, anchor.keyId);
    if (!root || root->publicKey != anchor.publicKey
        || !root->replaces.isEmpty()) {
        return fail(errorCode,
                    QStringLiteral("update-signing-trust-anchor-history-invalid"));
    }
    if (!validIdentity(
            authority.signedEnvelopeIdentity,
            QStringLiteral("update-signing-key-ring-envelope:sha256:"))
        || authority.authorityIdentity != authorityIdentity(authority)) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-authority-invalid"));
    }
    return true;
}

bool validateRotation(const Ring &previous, const Ring &next,
                      QString *errorCode)
{
    if (next.generation < previous.generation) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-generation-rollback"));
    }
    if (next.generation != previous.generation + 1) {
        return fail(errorCode,
                    QStringLiteral("update-signing-key-ring-generation-gap"));
    }
    QMap<QString, const KeyRecord *> previousById;
    QMap<QString, const KeyRecord *> nextById;
    for (const KeyRecord &key : previous.keys) {
        previousById.insert(key.keyId, &key);
    }
    for (const KeyRecord &key : next.keys) nextById.insert(key.keyId, &key);
    for (auto it = previousById.constBegin(); it != previousById.constEnd(); ++it) {
        const KeyRecord *replacement = nextById.value(it.key(), nullptr);
        if (!replacement) {
            return fail(errorCode,
                        QStringLiteral("update-signing-key-removed"));
        }
        const KeyRecord &old = *it.value();
        if (replacement->publicKey != old.publicKey
            || replacement->validFromMs != old.validFromMs
            || replacement->replaces != old.replaces
            || replacement->usages != old.usages) {
            return fail(errorCode,
                        QStringLiteral("update-signing-key-rewritten"));
        }
        if (replacement->validUntilMs > old.validUntilMs) {
            return fail(errorCode,
                        QStringLiteral("update-signing-key-validity-widened"));
        }
        if (old.revoked && !replacement->revoked) {
            return fail(errorCode,
                        QStringLiteral("update-signing-key-revocation-reversed"));
        }
    }
    for (const KeyRecord &key : next.keys) {
        if (previousById.contains(key.keyId)) continue;
        if (key.replaces.isEmpty()) {
            return fail(errorCode,
                        QStringLiteral("update-signing-key-lineage-missing"));
        }
        const KeyRecord *parent = previousById.value(key.replaces, nullptr);
        if (!parent) {
            return fail(errorCode,
                        QStringLiteral("update-signing-key-lineage-unknown"));
        }
        for (const QString &usage : key.usages) {
            if (!parent->usages.contains(usage)) {
                return fail(errorCode,
                            QStringLiteral("update-signing-key-usage-widened"));
            }
        }
    }
    return true;
}

QSharedPointer<const Authority::Data> makeAuthorityData(
    const TrustAnchorAuthority::Data &anchor, const SignedRing &signedRing,
    const Authority::Data *previous)
{
    QSharedPointer<Authority::Data> data(new Authority::Data);
    data->trustAnchorKeyId = anchor.keyId;
    data->trustAnchorPublicKeyBase64 = anchor.publicKeyBase64;
    data->trustAnchorIdentity = anchor.anchorIdentity;
    data->ring = signedRing.ring;
    data->signedAtMs = signedRing.signedAtMs;
    if (previous) data->keyAdmissions = previous->keyAdmissions;
    for (const KeyRecord &key : data->ring.keys) {
        if (!data->keyAdmissions.contains(key.keyId)) {
            data->keyAdmissions.insert(
                key.keyId,
                KeyAdmission{data->ring.generation, signedRing.signedAtMs});
        }
    }
    data->keyAdmissionsIdentity = keyAdmissionsIdentity(*data);
    data->signedEnvelopeIdentity = signedRing.envelopeIdentity;
    data->authorityIdentity = authorityIdentity(*data);
    return data;
}

} // namespace

class Verifier
{
public:
    static TrustAnchorAuthority makeAnchor(const QString &keyId,
                                           const QByteArray &publicKeyBase64,
                                           QString *errorCode)
    {
        TrustAnchorAuthority authority;
        if (!validKeyId(keyId) || publicKeyBase64.size() != 44) {
            fail(errorCode,
                 QStringLiteral("update-signing-trust-anchor-invalid"));
            return authority;
        }
        const QString encoded = QString::fromLatin1(publicKeyBase64);
        const QByteArray publicKey = decodeCanonicalBase64(encoded, 32);
        if (publicKey.isEmpty()) {
            fail(errorCode,
                 QStringLiteral("update-signing-trust-anchor-invalid"));
            return authority;
        }
        QSharedPointer<TrustAnchorAuthority::Data> data(
            new TrustAnchorAuthority::Data);
        data->keyId = keyId;
        data->publicKeyBase64 = encoded;
        data->publicKey = publicKey;
        data->anchorIdentity = anchorIdentity(*data);
        authority.m_data = data;
        if (errorCode) errorCode->clear();
        return authority;
    }

    static AuthorityResult bootstrap(const QByteArray &bytes,
                                     const TrustAnchorAuthority &trustAnchor,
                                     qint64 nowMs)
    {
        AuthorityResult result;
        if (!trustAnchor.m_data
            || !validAnchor(*trustAnchor.m_data, &result.errorCode)) {
            if (result.errorCode.isEmpty()) {
                result.errorCode = QStringLiteral(
                    "update-signing-trust-anchor-invalid");
            }
            return result;
        }
        SignedRing signedRing;
        if (!parseSignedRing(bytes, nowMs, &signedRing, &result.errorCode)) {
            return result;
        }
        if (signedRing.signerKeyId != trustAnchor.m_data->keyId) {
            result.errorCode = QStringLiteral(
                "update-signing-key-ring-bootstrap-signer-mismatch");
            return result;
        }
        if (!verifySignature(
                trustAnchor.m_data->publicKey, signedRing.payload,
                signedRing.signatureBase64, &result.errorCode,
                QStringLiteral("update-signing-key-ring-signature-invalid"),
                QStringLiteral(
                    "update-signing-key-ring-signature-verifier-unavailable"))) {
            return result;
        }
        if (signedRing.ring.generation != 1
            || signedRing.ring.keys.size() != 1) {
            result.errorCode = QStringLiteral(
                "update-signing-key-ring-bootstrap-generation-invalid");
            return result;
        }
        const KeyRecord &root = signedRing.ring.keys.constFirst();
        const quint64 now = static_cast<quint64>(nowMs);
        if (root.keyId != trustAnchor.m_data->keyId
            || root.publicKey != trustAnchor.m_data->publicKey
            || root.revoked || !root.replaces.isEmpty()
            || !root.usages.contains(kArtifactUsage)
            || !root.usages.contains(kRingUsage)
            || !keyActiveAt(root, signedRing.signedAtMs)
            || !keyActiveAt(root, now)) {
            result.errorCode = QStringLiteral(
                "update-signing-key-ring-bootstrap-root-invalid");
            return result;
        }
        result.authority.m_data = makeAuthorityData(
            *trustAnchor.m_data, signedRing, nullptr);
        result.ok = true;
        return result;
    }

    static AuthorityResult rotate(const QByteArray &bytes,
                                  const Authority &previous,
                                  qint64 nowMs)
    {
        AuthorityResult result;
        if (!previous.m_data
            || !validAuthority(*previous.m_data, &result.errorCode)) {
            if (result.errorCode.isEmpty()) {
                result.errorCode = QStringLiteral(
                    "update-signing-key-ring-authority-invalid");
            }
            return result;
        }
        SignedRing signedRing;
        if (!parseSignedRing(bytes, nowMs, &signedRing, &result.errorCode)) {
            return result;
        }
        if (signedRing.ring.generation == previous.m_data->ring.generation
            && signedRing.envelopeIdentity
                == previous.m_data->signedEnvelopeIdentity) {
            result.ok = true;
            result.idempotent = true;
            result.authority = previous;
            return result;
        }
        const KeyRecord *signer = findKey(
            previous.m_data->ring, signedRing.signerKeyId);
        const quint64 now = static_cast<quint64>(nowMs);
        if (!signer) {
            result.errorCode = QStringLiteral(
                "update-signing-key-ring-signer-unknown");
            return result;
        }
        if (!signer->usages.contains(kRingUsage)) {
            result.errorCode = QStringLiteral(
                "update-signing-key-ring-signer-usage-invalid");
            return result;
        }
        // signed_at_ms is signer-controlled metadata, not a trusted timestamp.
        // First admission must therefore also observe a currently active signer.
        if (!keyActiveAt(*signer, signedRing.signedAtMs)
            || !keyActiveAt(*signer, now)) {
            result.errorCode = signer->revoked
                ? QStringLiteral("update-signing-key-ring-signer-revoked")
                : QStringLiteral("update-signing-key-ring-signer-inactive");
            return result;
        }
        const KeyAdmission signerAdmission =
            previous.m_data->keyAdmissions.value(signer->keyId);
        if (signedRing.signedAtMs < signerAdmission.admittedAtMs) {
            result.errorCode = QStringLiteral(
                "update-signing-key-ring-signer-not-yet-admitted");
            return result;
        }
        if (!verifySignature(
                signer->publicKey, signedRing.payload,
                signedRing.signatureBase64, &result.errorCode,
                QStringLiteral("update-signing-key-ring-signature-invalid"),
                QStringLiteral(
                    "update-signing-key-ring-signature-verifier-unavailable"))) {
            return result;
        }
        if (signedRing.ring.generation == previous.m_data->ring.generation) {
            result.errorCode = QStringLiteral(
                "update-signing-key-ring-generation-conflict");
            return result;
        }
        if (signedRing.signedAtMs < previous.m_data->signedAtMs) {
            result.errorCode = QStringLiteral(
                "update-signing-key-ring-signed-time-rollback");
            return result;
        }
        if (!validateRotation(previous.m_data->ring, signedRing.ring,
                              &result.errorCode)) {
            return result;
        }
        if (!hasActiveUsageAt(
                signedRing.ring, kArtifactUsage, signedRing.signedAtMs)
            || !hasActiveUsageAt(
                signedRing.ring, kRingUsage, signedRing.signedAtMs)
            || !hasActiveUsageAt(signedRing.ring, kArtifactUsage, now)
            || !hasActiveUsageAt(signedRing.ring, kRingUsage, now)) {
            result.errorCode = QStringLiteral(
                "update-signing-key-ring-no-current-active-usage");
            return result;
        }
        TrustAnchorAuthority::Data anchor;
        anchor.keyId = previous.m_data->trustAnchorKeyId;
        anchor.publicKeyBase64 = previous.m_data->trustAnchorPublicKeyBase64;
        anchor.publicKey = decodeCanonicalBase64(anchor.publicKeyBase64, 32);
        anchor.anchorIdentity = previous.m_data->trustAnchorIdentity;
        result.authority.m_data = makeAuthorityData(
            anchor, signedRing, previous.m_data.data());
        result.ok = true;
        return result;
    }

    static ArtifactSignatureResult artifactSignature(
        const Authority &authority, const QString &signerKeyId,
        quint64 signedAtMs, quint64 nowMs, bool requireCurrentlyActive,
        const QByteArray &payload, const QString &signatureBase64)
    {
        ArtifactSignatureResult result;
        QString authorityError;
        if (!authority.m_data
            || !validAuthority(*authority.m_data, &authorityError)
            || signedAtMs == 0 || nowMs == 0
            || signedAtMs
                > static_cast<quint64>(kMaximumSafeJsonInteger)
            || nowMs > static_cast<quint64>(kMaximumSafeJsonInteger)
            || !validKeyId(signerKeyId)) {
            result.errorCode = QStringLiteral(
                "update-signing-key-ring-authority-invalid");
            return result;
        }
        const KeyRecord *key = findKey(authority.m_data->ring, signerKeyId);
        if (!key) {
            result.errorCode = QStringLiteral(
                "artifact-set-signing-key-unknown");
            return result;
        }
        if (!key->usages.contains(kArtifactUsage)) {
            result.errorCode = QStringLiteral(
                "artifact-set-signing-key-usage-invalid");
            return result;
        }
        if (key->revoked) {
            result.errorCode = QStringLiteral(
                "artifact-set-signing-key-revoked");
            return result;
        }
        if (signedAtMs < key->validFromMs
            || (requireCurrentlyActive && nowMs < key->validFromMs)) {
            result.errorCode = QStringLiteral(
                "artifact-set-signing-key-not-yet-valid");
            return result;
        }
        const KeyAdmission admission =
            authority.m_data->keyAdmissions.value(key->keyId);
        if (signedAtMs < admission.admittedAtMs) {
            result.errorCode = QStringLiteral(
                "artifact-set-signing-key-not-yet-admitted");
            return result;
        }
        if (signedAtMs >= key->validUntilMs
            || (requireCurrentlyActive && nowMs >= key->validUntilMs)) {
            result.errorCode = QStringLiteral(
                "artifact-set-signing-key-expired");
            return result;
        }
        QString signatureError;
        if (!verifySignature(
                key->publicKey, payload, signatureBase64, &signatureError,
                QStringLiteral("artifact-set-signature-invalid"),
                QStringLiteral(
                    "artifact-set-signature-verifier-unavailable"))) {
            result.errorCode = signatureError.startsWith(
                                   QStringLiteral("update-signing-signature-"))
                ? QStringLiteral("artifact-set-signature-encoding-invalid")
                : signatureError;
            return result;
        }
        result.ok = true;
        result.signerKeyId = key->keyId;
        result.signerKeyIdentity = key->keyIdentity;
        result.ringIdentity = authority.m_data->ring.ringIdentity;
        result.ringGeneration = authority.m_data->ring.generation;
        result.trustAnchorIdentity = authority.m_data->trustAnchorIdentity;
        result.ringAuthorityIdentity = authority.m_data->authorityIdentity;
        return result;
    }
};

bool TrustAnchorAuthority::isValid() const
{
    QString errorCode;
    return m_data && validAnchor(*m_data, &errorCode);
}

QString TrustAnchorAuthority::keyId() const
{
    return m_data ? m_data->keyId : QString();
}

QString TrustAnchorAuthority::anchorIdentity() const
{
    return m_data ? m_data->anchorIdentity : QString();
}

bool Authority::isValid() const
{
    QString errorCode;
    return m_data && validAuthority(*m_data, &errorCode);
}

quint64 Authority::generation() const
{
    return m_data ? m_data->ring.generation : 0;
}

QString Authority::ringIdentity() const
{
    return m_data ? m_data->ring.ringIdentity : QString();
}

QString Authority::trustAnchorIdentity() const
{
    return m_data ? m_data->trustAnchorIdentity : QString();
}

QString Authority::authorityIdentity() const
{
    return m_data ? m_data->authorityIdentity : QString();
}

TrustAnchorAuthority embeddedTrustAnchor()
{
#if defined(AEGISY_UPDATE_ARTIFACT_ROOT_KEY_ID) \
    && defined(AEGISY_UPDATE_ARTIFACT_ROOT_PUBLIC_KEY)
    return Verifier::makeAnchor(
        QString::fromLatin1(AEGISY_UPDATE_ARTIFACT_ROOT_KEY_ID),
        QByteArrayLiteral(AEGISY_UPDATE_ARTIFACT_ROOT_PUBLIC_KEY), nullptr);
#else
    return {};
#endif
}

AuthorityResult verifyBootstrap(const QByteArray &signedRingJson,
                                const TrustAnchorAuthority &trustAnchor,
                                qint64 nowMs)
{
    return Verifier::bootstrap(signedRingJson, trustAnchor, nowMs);
}

AuthorityResult verifyRotation(const QByteArray &signedRingJson,
                               const Authority &previous,
                               qint64 nowMs)
{
    return Verifier::rotate(signedRingJson, previous, nowMs);
}

ArtifactSignatureResult verifyArtifactSetSignature(
    const Authority &authority, const QString &signerKeyId,
    quint64 signedAtMs, quint64 nowMs, bool requireCurrentlyActive,
    const QByteArray &payload, const QString &signatureBase64)
{
    return Verifier::artifactSignature(
        authority, signerKeyId, signedAtMs, nowMs, requireCurrentlyActive,
        payload, signatureBase64);
}

#ifdef AEGISY_UPDATE_SIGNING_KEY_RING_TESTING
TrustAnchorAuthority testingTrustAnchor(const QString &keyId,
                                        const QByteArray &publicKeyBase64,
                                        QString *errorCode)
{
    return Verifier::makeAnchor(keyId, publicKeyBase64, errorCode);
}
#endif

} // namespace UpdateSigningKeyRing
