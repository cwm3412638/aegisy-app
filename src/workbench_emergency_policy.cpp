#include "workbench_emergency_policy.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSettings>
#include <QSet>

#include <openssl/evp.h>

#include <cmath>
#include <limits>

namespace WorkbenchEmergencyPolicy {
namespace {

constexpr qint64 kMaximumClockSkewMs = 5LL * 60 * 1000;
constexpr qint64 kMaximumPolicyLifetimeMs = 7LL * 24 * 60 * 60 * 1000;
constexpr double kMaximumSafeJsonInteger = 9007199254740991.0;
const QString kEnvelopeKey = QStringLiteral("workbench/emergencyPolicy/envelope");
const QString kInstalledMarkerKey =
    QStringLiteral("workbench/emergencyPolicy/installedMarker");
const QString kMarkerSchema =
    QStringLiteral("aegisy-workbench-emergency-policy-marker/0.1");

struct InstalledMarker
{
    bool present = false;
    bool valid = false;
    quint64 sequence = 0;
    QString policyIdentity;
};

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &keys)
{
    if (object.size() != keys.size()) return false;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!keys.contains(it.key())) return false;
    }
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

QByteArray decodeCanonicalBase64(const QString &encoded, qsizetype expectedBytes)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9+/]+={0,2}$"));
    if (!pattern.match(encoded).hasMatch()) return {};
    const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
    if (bytes.size() != expectedBytes || bytes.toBase64() != encoded.toLatin1()) return {};
    return bytes;
}

bool verifyEd25519(const QByteArray &publicKey, const QByteArray &signature,
                   const QByteArray &payload)
{
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(publicKey.constData()),
        static_cast<size_t>(publicKey.size()));
    if (!key) return false;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    const bool valid = context
        && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestVerify(
               context,
               reinterpret_cast<const unsigned char *>(signature.constData()),
               static_cast<size_t>(signature.size()),
               reinterpret_cast<const unsigned char *>(payload.constData()),
               static_cast<size_t>(payload.size())) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return valid;
}

Decision invalid(const QString &errorCode)
{
    Decision decision;
    decision.state = State::Invalid;
    decision.blocksNewWork = true;
    decision.errorCode = errorCode;
    return decision;
}

QJsonObject parseStoredEnvelope(QSettings *settings, bool *present)
{
    *present = settings && settings->contains(kEnvelopeKey);
    if (!*present) return {};
    const QByteArray bytes = settings->value(kEnvelopeKey).toByteArray();
    if (bytes.isEmpty() || bytes.size() > 16 * 1024) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object() : QJsonObject{};
}

InstalledMarker parseInstalledMarker(QSettings *settings)
{
    InstalledMarker marker;
    marker.present = settings && settings->contains(kInstalledMarkerKey);
    if (!marker.present) return marker;
    const QByteArray bytes = settings->value(kInstalledMarkerKey).toByteArray();
    if (bytes.isEmpty() || bytes.size() > 1024) return marker;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return marker;
    const QJsonObject object = document.object();
    if (!hasExactKeys(object, {
            QStringLiteral("schema_version"), QStringLiteral("sequence"),
            QStringLiteral("policy_identity"),
        })
        || object.value(QStringLiteral("schema_version")).toString() != kMarkerSchema
        || !safePositiveInteger(object.value(QStringLiteral("sequence")), &marker.sequence)) {
        return marker;
    }
    marker.policyIdentity = object.value(QStringLiteral("policy_identity")).toString();
    static const QRegularExpression identityPattern(
        QStringLiteral("^workbench-emergency-policy:sha256:[0-9a-f]{64}$"));
    marker.valid = identityPattern.match(marker.policyIdentity).hasMatch();
    return marker;
}

QByteArray serializeInstalledMarker(const Decision &decision)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("schema_version"), kMarkerSchema},
        {QStringLiteral("sequence"), double(decision.sequence)},
        {QStringLiteral("policy_identity"), decision.policyIdentity},
    }).toJson(QJsonDocument::Compact);
}

} // namespace

QByteArray signaturePayload(const QJsonObject &envelope, QString *errorCode)
{
    const auto fail = [errorCode](const QString &code) {
        if (errorCode) *errorCode = code;
        return QByteArray{};
    };
    if (!hasExactKeys(envelope, {
            QStringLiteral("schema_version"), QStringLiteral("sequence"),
            QStringLiteral("issued_at_ms"), QStringLiteral("expires_at_ms"),
            QStringLiteral("workbench_disabled"), QStringLiteral("reason_code"),
            QStringLiteral("signature"),
        })) {
        return fail(QStringLiteral("policy-fields-invalid"));
    }
    if (envelope.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("aegisy-workbench-emergency-policy/0.1")) {
        return fail(QStringLiteral("policy-schema-invalid"));
    }
    quint64 sequence = 0;
    quint64 issuedAt = 0;
    quint64 expiresAt = 0;
    if (!safePositiveInteger(envelope.value(QStringLiteral("sequence")), &sequence)
        || !safePositiveInteger(envelope.value(QStringLiteral("issued_at_ms")), &issuedAt)
        || !safePositiveInteger(envelope.value(QStringLiteral("expires_at_ms")), &expiresAt)
        || !envelope.value(QStringLiteral("workbench_disabled")).isBool()) {
        return fail(QStringLiteral("policy-value-invalid"));
    }
    const QString reason = envelope.value(QStringLiteral("reason_code")).toString();
    static const QRegularExpression reasonPattern(QStringLiteral("^[a-z0-9][a-z0-9-]{0,63}$"));
    if (!reasonPattern.match(reason).hasMatch()) {
        return fail(QStringLiteral("policy-reason-invalid"));
    }
    QByteArray payload = QByteArrayLiteral("aegisy-workbench-emergency-policy/0.1\n");
    payload += "sequence=" + QByteArray::number(sequence) + '\n';
    payload += "issued_at_ms=" + QByteArray::number(issuedAt) + '\n';
    payload += "expires_at_ms=" + QByteArray::number(expiresAt) + '\n';
    payload += QByteArrayLiteral("workbench_disabled=")
        + (envelope.value(QStringLiteral("workbench_disabled")).toBool()
               ? QByteArrayLiteral("true\n") : QByteArrayLiteral("false\n"));
    payload += "reason_code=" + reason.toLatin1() + '\n';
    if (errorCode) errorCode->clear();
    return payload;
}

Decision verify(const QJsonObject &envelope, const QByteArray &publicKeyBase64,
                qint64 nowMs, bool requireFresh)
{
    QString errorCode;
    const QByteArray payload = signaturePayload(envelope, &errorCode);
    if (payload.isEmpty()) return invalid(errorCode);
    const QByteArray publicKey = decodeCanonicalBase64(
        QString::fromLatin1(publicKeyBase64), 32);
    const QByteArray signature = decodeCanonicalBase64(
        envelope.value(QStringLiteral("signature")).toString(), 64);
    if (publicKey.isEmpty() || signature.isEmpty()) {
        return invalid(QStringLiteral("policy-signature-encoding-invalid"));
    }
    if (!verifyEd25519(publicKey, signature, payload)) {
        return invalid(QStringLiteral("policy-signature-invalid"));
    }

    quint64 sequence = 0;
    quint64 issuedAt = 0;
    quint64 expiresAt = 0;
    safePositiveInteger(envelope.value(QStringLiteral("sequence")), &sequence);
    safePositiveInteger(envelope.value(QStringLiteral("issued_at_ms")), &issuedAt);
    safePositiveInteger(envelope.value(QStringLiteral("expires_at_ms")), &expiresAt);
    if (expiresAt <= issuedAt || expiresAt - issuedAt > quint64(kMaximumPolicyLifetimeMs)) {
        return invalid(QStringLiteral("policy-lifetime-invalid"));
    }
    if (nowMs <= 0 || nowMs > std::numeric_limits<qint64>::max() - kMaximumClockSkewMs
        || issuedAt > quint64(nowMs + kMaximumClockSkewMs)) {
        return invalid(QStringLiteral("policy-clock-invalid"));
    }

    Decision decision;
    decision.sequence = sequence;
    decision.expiresAtMs = static_cast<qint64>(expiresAt);
    decision.reasonCode = envelope.value(QStringLiteral("reason_code")).toString();
    decision.policyIdentity = QStringLiteral("workbench-emergency-policy:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()));
    if (requireFresh && quint64(nowMs) >= expiresAt) {
        decision.state = State::Stale;
        decision.blocksNewWork = true;
        decision.errorCode = QStringLiteral("policy-expired");
        return decision;
    }
    decision.blocksNewWork = envelope.value(QStringLiteral("workbench_disabled")).toBool();
    decision.state = decision.blocksNewWork ? State::Disabled : State::Enabled;
    return decision;
}

Decision load(QSettings *settings, const QByteArray &publicKeyBase64, qint64 nowMs)
{
    if (!settings) return invalid(QStringLiteral("policy-store-unavailable"));
    bool present = false;
    const QJsonObject envelope = parseStoredEnvelope(settings, &present);
    const InstalledMarker marker = parseInstalledMarker(settings);
    if (!present && !marker.present) {
        return {};
    }
    if (!present) return invalid(QStringLiteral("policy-cache-missing"));
    if (!marker.present) return invalid(QStringLiteral("policy-cache-marker-missing"));
    if (!marker.valid) return invalid(QStringLiteral("policy-cache-marker-invalid"));
    if (envelope.isEmpty()) return invalid(QStringLiteral("policy-cache-invalid"));
    Decision decision = verify(envelope, publicKeyBase64, nowMs, true);
    if (decision.state == State::Invalid) return decision;
    if (marker.sequence != decision.sequence
        || marker.policyIdentity != decision.policyIdentity) {
        return invalid(QStringLiteral("policy-cache-marker-mismatch"));
    }
    return decision;
}

InstallResult install(QSettings *settings, const QJsonObject &envelope,
                      const QByteArray &publicKeyBase64, qint64 nowMs)
{
    InstallResult result;
    if (!settings) {
        result.errorCode = QStringLiteral("policy-store-unavailable");
        result.decision = invalid(result.errorCode);
        return result;
    }
    const Decision candidate = verify(envelope, publicKeyBase64, nowMs, true);
    if (candidate.state == State::Invalid || candidate.state == State::Stale) {
        result.errorCode = candidate.errorCode;
        result.decision = load(settings, publicKeyBase64, nowMs);
        return result;
    }

    bool existingPresent = false;
    parseStoredEnvelope(settings, &existingPresent);
    const InstalledMarker marker = parseInstalledMarker(settings);
    if (existingPresent && !marker.present) {
        result.errorCode = QStringLiteral("policy-cache-marker-missing");
        result.decision = invalid(result.errorCode);
        return result;
    }
    if (marker.present && !marker.valid) {
        result.errorCode = QStringLiteral("policy-cache-marker-invalid");
        result.decision = invalid(result.errorCode);
        return result;
    }
    if (marker.valid && candidate.sequence < marker.sequence) {
        result.errorCode = QStringLiteral("policy-sequence-rollback");
        result.decision = load(settings, publicKeyBase64, nowMs);
        return result;
    }
    if (marker.valid && candidate.sequence == marker.sequence
        && candidate.policyIdentity != marker.policyIdentity) {
        result.errorCode = QStringLiteral("policy-sequence-conflict");
        result.decision = load(settings, publicKeyBase64, nowMs);
        return result;
    }

    if (!marker.valid || candidate.sequence > marker.sequence) {
        settings->setValue(kInstalledMarkerKey, serializeInstalledMarker(candidate));
        settings->sync();
        if (settings->status() != QSettings::NoError) {
            result.errorCode = QStringLiteral("policy-marker-write-failed");
            result.decision = invalid(result.errorCode);
            return result;
        }
    }
    const QByteArray compact = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (compact.isEmpty() || compact.size() > 16 * 1024) {
        result.errorCode = QStringLiteral("policy-cache-size-invalid");
        result.decision = load(settings, publicKeyBase64, nowMs);
        return result;
    }
    settings->setValue(kEnvelopeKey, compact);
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        result.errorCode = QStringLiteral("policy-cache-write-failed");
        result.decision = invalid(result.errorCode);
        return result;
    }
    result.accepted = true;
    result.decision = load(settings, publicKeyBase64, nowMs);
    return result;
}

} // namespace WorkbenchEmergencyPolicy
