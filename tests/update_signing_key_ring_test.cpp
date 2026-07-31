#include "update_signing_key_ring.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <openssl/evp.h>

#include <cstdio>

namespace {

constexpr qint64 kNowMs = 2000;
constexpr qint64 kRotationNowMs = 2500;

bool expect(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
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

QString identity(const QByteArray &payload, const QString &prefix)
{
    return prefix + QString::fromLatin1(QCryptographicHash::hash(
        payload, QCryptographicHash::Sha256).toHex());
}

class SigningKey
{
public:
    explicit SigningKey(char seed)
    {
        const QByteArray privateKey(32, seed);
        m_key = EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr,
            reinterpret_cast<const unsigned char *>(privateKey.constData()),
            static_cast<size_t>(privateKey.size()));
        if (!m_key) return;
        QByteArray publicKey(32, '\0');
        size_t size = static_cast<size_t>(publicKey.size());
        if (EVP_PKEY_get_raw_public_key(
                m_key,
                reinterpret_cast<unsigned char *>(publicKey.data()),
                &size) != 1
            || size != 32) {
            EVP_PKEY_free(m_key);
            m_key = nullptr;
            return;
        }
        m_publicKeyBase64 = publicKey.toBase64();
    }

    ~SigningKey() { EVP_PKEY_free(m_key); }

    bool isValid() const { return m_key && m_publicKeyBase64.size() == 44; }
    QByteArray publicKeyBase64() const { return m_publicKeyBase64; }

    QString sign(const QByteArray &payload) const
    {
        if (!m_key) return {};
        EVP_MD_CTX *context = EVP_MD_CTX_new();
        size_t size = 0;
        const bool measured = context
            && EVP_DigestSignInit(context, nullptr, nullptr, nullptr, m_key) == 1
            && EVP_DigestSign(
                   context, nullptr, &size,
                   reinterpret_cast<const unsigned char *>(payload.constData()),
                   static_cast<size_t>(payload.size())) == 1;
        if (!measured || size != 64) {
            EVP_MD_CTX_free(context);
            return {};
        }
        QByteArray signature(static_cast<int>(size), '\0');
        const bool signedPayload = EVP_DigestSign(
            context,
            reinterpret_cast<unsigned char *>(signature.data()), &size,
            reinterpret_cast<const unsigned char *>(payload.constData()),
            static_cast<size_t>(payload.size())) == 1;
        EVP_MD_CTX_free(context);
        return signedPayload && size == 64
            ? QString::fromLatin1(signature.toBase64()) : QString();
    }

private:
    EVP_PKEY *m_key = nullptr;
    QByteArray m_publicKeyBase64;
};

QStringList bothUsages()
{
    return {QStringLiteral("artifact-set"), QStringLiteral("key-ring")};
}

void appendKey(QByteArray *payload, const QByteArray &prefix,
               const QJsonObject &key, bool includeIdentity)
{
    appendLine(payload, prefix + ".key_id",
               key.value(QStringLiteral("key_id")).toString());
    appendLine(payload, prefix + ".public_key_base64",
               key.value(QStringLiteral("public_key_base64")).toString());
    appendLine(payload, prefix + ".valid_from_ms",
               static_cast<quint64>(
                   key.value(QStringLiteral("valid_from_ms")).toDouble()));
    appendLine(payload, prefix + ".valid_until_ms",
               static_cast<quint64>(
                   key.value(QStringLiteral("valid_until_ms")).toDouble()));
    appendLine(payload, prefix + ".revoked",
               key.value(QStringLiteral("revoked")).toBool());
    const QJsonValue replaces = key.value(QStringLiteral("replaces"));
    appendLine(payload, prefix + ".replaces.present", !replaces.isNull());
    if (!replaces.isNull()) {
        appendLine(payload, prefix + ".replaces", replaces.toString());
    }
    const QJsonArray usages = key.value(QStringLiteral("usages")).toArray();
    appendLine(payload, prefix + ".usages.count",
               static_cast<quint64>(usages.size()));
    for (int index = 0; index < usages.size(); ++index) {
        appendLine(payload,
                   prefix + ".usages." + QByteArray::number(index),
                   usages.at(index).toString());
    }
    if (includeIdentity) {
        appendLine(payload, prefix + ".key_identity",
                   key.value(QStringLiteral("key_identity")).toString());
    }
}

QJsonObject keyObject(const QString &keyId, const SigningKey &key,
                      quint64 validFromMs, quint64 validUntilMs,
                      bool revoked, const QJsonValue &replaces,
                      const QStringList &usages = bothUsages())
{
    QJsonArray usageArray;
    for (const QString &usage : usages) usageArray.append(usage);
    QJsonObject result{
        {QStringLiteral("key_id"), keyId},
        {QStringLiteral("public_key_base64"),
         QString::fromLatin1(key.publicKeyBase64())},
        {QStringLiteral("valid_from_ms"), static_cast<double>(validFromMs)},
        {QStringLiteral("valid_until_ms"), static_cast<double>(validUntilMs)},
        {QStringLiteral("revoked"), revoked},
        {QStringLiteral("replaces"), replaces},
        {QStringLiteral("usages"), usageArray},
        {QStringLiteral("key_identity"), QString()},
    };
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key/0.1\n");
    appendKey(&payload, QByteArrayLiteral("key"), result, false);
    result.insert(
        QStringLiteral("key_identity"),
        identity(payload, QStringLiteral("update-signing-key:sha256:")));
    return result;
}

QJsonObject reidentifyKey(QJsonObject key)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key/0.1\n");
    appendKey(&payload, QByteArrayLiteral("key"), key, false);
    key.insert(
        QStringLiteral("key_identity"),
        identity(payload, QStringLiteral("update-signing-key:sha256:")));
    return key;
}

QJsonObject ringObject(quint64 generation, const QJsonArray &keys)
{
    QJsonObject ring{
        {QStringLiteral("schema_version"),
         QStringLiteral("aegisy-update-signing-key-ring/0.1")},
        {QStringLiteral("generation"), static_cast<double>(generation)},
        {QStringLiteral("keys"), keys},
        {QStringLiteral("ring_identity"), QString()},
    };
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring/0.1\n");
    appendLine(&payload, QByteArrayLiteral("generation"), generation);
    appendLine(&payload, QByteArrayLiteral("keys.count"),
               static_cast<quint64>(keys.size()));
    for (int index = 0; index < keys.size(); ++index) {
        appendKey(&payload,
                  QByteArrayLiteral("keys.") + QByteArray::number(index),
                  keys.at(index).toObject(), true);
    }
    ring.insert(
        QStringLiteral("ring_identity"),
        identity(payload,
                 QStringLiteral("update-signing-key-ring:sha256:")));
    return ring;
}

QByteArray ringSigningPayload(const QString &signerKeyId, quint64 signedAtMs,
                              const QJsonObject &ring)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring-signature/0.1\n");
    appendLine(&payload, QByteArrayLiteral("signer_key_id"), signerKeyId);
    appendLine(&payload, QByteArrayLiteral("signed_at_ms"), signedAtMs);
    appendLine(&payload, QByteArrayLiteral("key_ring.generation"),
               static_cast<quint64>(
                   ring.value(QStringLiteral("generation")).toDouble()));
    appendLine(&payload, QByteArrayLiteral("key_ring.identity"),
               ring.value(QStringLiteral("ring_identity")).toString());
    return payload;
}

QJsonObject signedRingObject(const QString &signerKeyId, quint64 signedAtMs,
                             const QJsonObject &ring, const SigningKey &key)
{
    const QByteArray payload = ringSigningPayload(
        signerKeyId, signedAtMs, ring);
    return {
        {QStringLiteral("schema_version"),
         QStringLiteral("aegisy-update-signing-key-ring-signature/0.1")},
        {QStringLiteral("signer_key_id"), signerKeyId},
        {QStringLiteral("signed_at_ms"), static_cast<double>(signedAtMs)},
        {QStringLiteral("key_ring"), ring},
        {QStringLiteral("payload_identity"),
         identity(payload,
                  QStringLiteral("update-signing-key-ring-payload:sha256:"))},
        {QStringLiteral("signature"), key.sign(payload)},
    };
}

QByteArray encoded(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QString signedEnvelopeIdentity(const QJsonObject &envelope)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring-envelope/0.1\n");
    appendLine(&payload, QByteArrayLiteral("signing_payload.identity"),
               envelope.value(QStringLiteral("payload_identity")).toString());
    appendLine(&payload, QByteArrayLiteral("signature_base64"),
               envelope.value(QStringLiteral("signature")).toString());
    return identity(
        payload,
        QStringLiteral("update-signing-key-ring-envelope:sha256:"));
}

UpdateSigningKeyRing::AuthorityResult expectRotationError(
    const QJsonObject &envelope,
    const UpdateSigningKeyRing::Authority &previous,
    const QString &expectedCode,
    bool *ok,
    const char *message)
{
    const UpdateSigningKeyRing::AuthorityResult result =
        UpdateSigningKeyRing::verifyRotation(
            encoded(envelope), previous, kRotationNowMs);
    *ok = expect(!result.ok && !result.authority.isValid()
                     && result.errorCode == expectedCode,
                 message) && *ok;
    return result;
}

bool bootstrapAndParserTests(const SigningKey &root)
{
    bool ok = true;
    QString errorCode;
    const UpdateSigningKeyRing::TrustAnchorAuthority anchor =
        UpdateSigningKeyRing::testingTrustAnchor(
            QStringLiteral("artifact-2026-01"), root.publicKeyBase64(),
            &errorCode);
    ok = expect(errorCode.isEmpty() && anchor.isValid()
                    && anchor.keyId() == QStringLiteral("artifact-2026-01")
                    && anchor.anchorIdentity().startsWith(
                        QStringLiteral(
                            "update-signing-trust-anchor:sha256:")),
                "valid update trust anchor was rejected") && ok;
    const QJsonObject rootRecord = keyObject(
        QStringLiteral("artifact-2026-01"), root, 1000, 3000, false,
        QJsonValue(QJsonValue::Null));
    const QJsonObject ring = ringObject(1, QJsonArray{rootRecord});
    const QJsonObject envelope = signedRingObject(
        QStringLiteral("artifact-2026-01"), 1500, ring, root);
    const UpdateSigningKeyRing::AuthorityResult bootstrap =
        UpdateSigningKeyRing::verifyBootstrap(encoded(envelope), anchor, kNowMs);
    ok = expect(bootstrap.ok && !bootstrap.idempotent
                    && bootstrap.errorCode.isEmpty()
                    && bootstrap.authority.isValid()
                    && bootstrap.authority.generation() == 1
                    && bootstrap.authority.ringIdentity()
                        == ring.value(QStringLiteral("ring_identity")).toString()
                    && bootstrap.authority.trustAnchorIdentity()
                        == anchor.anchorIdentity()
                    && bootstrap.authority.authorityIdentity().startsWith(
                        QStringLiteral(
                            "update-signing-key-ring-authority:sha256:")),
                "valid update key-ring bootstrap was rejected") && ok;
    ok = expect(
        anchor.anchorIdentity()
                == QStringLiteral(
                    "update-signing-trust-anchor:sha256:dc421a82e06b57c959440e65b041a7bd65037503708d20849e322042cd6e0600")
            && rootRecord.value(QStringLiteral("key_identity")).toString()
                == QStringLiteral(
                    "update-signing-key:sha256:3036f1bee3b5b0b0f00d5eac81bf9977490bdc4e0c65416e458acd73275f701d")
            && ring.value(QStringLiteral("ring_identity")).toString()
                == QStringLiteral(
                    "update-signing-key-ring:sha256:9ea00e6c8a611d9ee356a52127a98e01a1745d3e3f73be6881eb591c0c3b392f")
            && envelope.value(QStringLiteral("payload_identity")).toString()
                == QStringLiteral(
                    "update-signing-key-ring-payload:sha256:4418e8e61388e34e1aaa8fdcf13978ec1ffaa1359bc77250189f060398d7c64f")
            && signedEnvelopeIdentity(envelope)
                == QStringLiteral(
                    "update-signing-key-ring-envelope:sha256:1a1d79cfd4d3410fcb408ccf946df4d07d246252b428658273563de227aba889")
            && bootstrap.authority.authorityIdentity()
                == QStringLiteral(
                    "update-signing-key-ring-authority:sha256:99acc5ac4f9a04eb7176a6cad502a6c5a872815690ddd8e6bc467f5bc7cb71a6"),
        "update signing fixed identity vectors drifted") && ok;

    const UpdateSigningKeyRing::AuthorityResult delayedBootstrap =
        UpdateSigningKeyRing::verifyBootstrap(
            encoded(envelope), anchor, 3500);
    ok = expect(!delayedBootstrap.ok
                    && delayedBootstrap.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-bootstrap-root-invalid"),
                "expired root bootstrap trusted self-reported signing time")
        && ok;

    const UpdateSigningKeyRing::AuthorityResult duplicate =
        UpdateSigningKeyRing::verifyRotation(encoded(envelope),
                                             bootstrap.authority, kNowMs);
    ok = expect(duplicate.ok && duplicate.idempotent
                    && duplicate.authority.authorityIdentity()
                        == bootstrap.authority.authorityIdentity(),
                "exact signed key-ring retry was not idempotent") && ok;

    QByteArray bom("\xEF\xBB\xBF", 3);
    bom += encoded(envelope);
    const auto invalidBom = UpdateSigningKeyRing::verifyBootstrap(
        bom, anchor, kNowMs);
    ok = expect(!invalidBom.ok
                    && invalidBom.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-json-invalid"),
                "BOM key-ring envelope was accepted") && ok;

    QByteArray duplicateKey = encoded(envelope);
    duplicateKey.replace(
        QByteArrayLiteral("\"schema_version\":"),
        QByteArrayLiteral(
            "\"schema_version\":\"aegisy-update-signing-key-ring-signature/0.1\",\"schema\\u005fversion\":"));
    const auto invalidDuplicate = UpdateSigningKeyRing::verifyBootstrap(
        duplicateKey, anchor, kNowMs);
    ok = expect(!invalidDuplicate.ok
                    && invalidDuplicate.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-json-invalid"),
                "decoded duplicate key-ring field was accepted") && ok;

    const auto oversized = UpdateSigningKeyRing::verifyBootstrap(
        QByteArray(128 * 1024 + 1, ' '), anchor, kNowMs);
    ok = expect(!oversized.ok
                    && oversized.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-json-size-invalid"),
                "oversized key-ring envelope was accepted") && ok;

    QByteArray invalidUtf8 = encoded(envelope);
    const qsizetype keyIdOffset = invalidUtf8.indexOf("artifact-2026-01");
    if (keyIdOffset >= 0) invalidUtf8[keyIdOffset] = static_cast<char>(0xff);
    const auto invalidUtf8Result = UpdateSigningKeyRing::verifyBootstrap(
        invalidUtf8, anchor, kNowMs);
    ok = expect(!invalidUtf8Result.ok
                    && invalidUtf8Result.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-json-invalid"),
                "invalid UTF-8 key-ring envelope was accepted") && ok;

    QByteArray loneSurrogate = encoded(envelope);
    loneSurrogate.replace(
        QByteArrayLiteral("artifact-2026-01"),
        QByteArrayLiteral("artifact-\\uD800"));
    const auto loneSurrogateResult = UpdateSigningKeyRing::verifyBootstrap(
        loneSurrogate, anchor, kNowMs);
    ok = expect(!loneSurrogateResult.ok
                    && loneSurrogateResult.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-json-invalid"),
                "lone-surrogate key-ring envelope was accepted") && ok;

    QJsonObject unknownEnvelopeField = envelope;
    unknownEnvelopeField.insert(QStringLiteral("unknown"), false);
    const auto unknownEnvelope = UpdateSigningKeyRing::verifyBootstrap(
        encoded(unknownEnvelopeField), anchor, kNowMs);
    ok = expect(!unknownEnvelope.ok
                    && unknownEnvelope.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-envelope-fields-invalid"),
                "unknown key-ring envelope field was accepted") && ok;

    QJsonObject mismatchedPayload = envelope;
    mismatchedPayload.insert(
        QStringLiteral("payload_identity"),
        QStringLiteral(
            "update-signing-key-ring-payload:sha256:0000000000000000000000000000000000000000000000000000000000000000"));
    const auto payloadMismatch = UpdateSigningKeyRing::verifyBootstrap(
        encoded(mismatchedPayload), anchor, kNowMs);
    ok = expect(!payloadMismatch.ok
                    && payloadMismatch.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-payload-identity-mismatch"),
                "mismatched key-ring payload identity was accepted") && ok;

    QJsonObject badSignature = envelope;
    QString changedSignature =
        badSignature.value(QStringLiteral("signature")).toString();
    changedSignature[0] = changedSignature.at(0) == QLatin1Char('A')
        ? QLatin1Char('B') : QLatin1Char('A');
    badSignature.insert(QStringLiteral("signature"), changedSignature);
    const auto invalidSignature = UpdateSigningKeyRing::verifyBootstrap(
        encoded(badSignature), anchor, kNowMs);
    ok = expect(!invalidSignature.ok
                    && invalidSignature.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-signature-invalid"),
                "invalid key-ring signature was accepted") && ok;

    QJsonObject invalidKeyEncoding = rootRecord;
    QString nonCanonicalKey = invalidKeyEncoding.value(
        QStringLiteral("public_key_base64")).toString();
    nonCanonicalKey.chop(1);
    nonCanonicalKey.append(QLatin1Char('A'));
    invalidKeyEncoding.insert(QStringLiteral("public_key_base64"),
                              nonCanonicalKey);
    invalidKeyEncoding = reidentifyKey(invalidKeyEncoding);
    const QJsonObject invalidKeyRing = ringObject(
        1, QJsonArray{invalidKeyEncoding});
    const auto invalidEncoding = UpdateSigningKeyRing::verifyBootstrap(
        encoded(signedRingObject(QStringLiteral("artifact-2026-01"), 1500,
                                 invalidKeyRing, root)),
        anchor, kNowMs);
    ok = expect(!invalidEncoding.ok
                    && invalidEncoding.errorCode
                        == QStringLiteral(
                            "update-signing-key-encoding-invalid"),
                "non-canonical update-signing public key was accepted") && ok;

    QJsonObject invalidValidity = rootRecord;
    invalidValidity.insert(QStringLiteral("valid_until_ms"), 1000.0);
    invalidValidity = reidentifyKey(invalidValidity);
    const QJsonObject invalidValidityRing = ringObject(
        1, QJsonArray{invalidValidity});
    const auto invalidValidityResult = UpdateSigningKeyRing::verifyBootstrap(
        encoded(signedRingObject(QStringLiteral("artifact-2026-01"), 1500,
                                 invalidValidityRing, root)),
        anchor, kNowMs);
    ok = expect(!invalidValidityResult.ok
                    && invalidValidityResult.errorCode
                        == QStringLiteral("update-signing-key-fields-invalid"),
                "invalid update-signing validity interval was accepted") && ok;

    const QJsonObject invalidUsageKey = keyObject(
        QStringLiteral("artifact-2026-01"), root, 1000, 3000, false,
        QJsonValue(QJsonValue::Null),
        {QStringLiteral("key-ring"), QStringLiteral("artifact-set")});
    const QJsonObject invalidUsageRing = ringObject(
        1, QJsonArray{invalidUsageKey});
    const auto invalidUsage = UpdateSigningKeyRing::verifyBootstrap(
        encoded(signedRingObject(QStringLiteral("artifact-2026-01"), 1500,
                                 invalidUsageRing, root)),
        anchor, kNowMs);
    ok = expect(!invalidUsage.ok
                    && invalidUsage.errorCode
                        == QStringLiteral("update-signing-key-usage-invalid"),
                "unsorted update-signing usages were accepted") && ok;

    QJsonArray tooManyKeys;
    for (int index = 0; index < 33; ++index) {
        tooManyKeys.append(keyObject(
            QStringLiteral("artifact-overflow-%1").arg(
                index, 2, 10, QLatin1Char('0')),
            root, 1000, 3000, false, QJsonValue(QJsonValue::Null)));
    }
    const QJsonObject oversizedRing = ringObject(1, tooManyKeys);
    const auto tooManyKeysResult = UpdateSigningKeyRing::verifyBootstrap(
        encoded(signedRingObject(QStringLiteral("artifact-2026-01"), 1500,
                                 oversizedRing, root)),
        anchor, kNowMs);
    ok = expect(!tooManyKeysResult.ok
                    && tooManyKeysResult.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-size-invalid"),
                "33-key update-signing ring was accepted") && ok;

    const QJsonObject zeroGenerationRing = ringObject(
        0, QJsonArray{rootRecord});
    const auto zeroGeneration = UpdateSigningKeyRing::verifyBootstrap(
        encoded(signedRingObject(QStringLiteral("artifact-2026-01"), 1500,
                                 zeroGenerationRing, root)),
        anchor, kNowMs);
    ok = expect(!zeroGeneration.ok
                    && zeroGeneration.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-fields-invalid"),
                "zero-generation update-signing ring was accepted") && ok;

    QJsonObject futureEnvelope = signedRingObject(
        QStringLiteral("artifact-2026-01"), 2001, ring, root);
    const auto future = UpdateSigningKeyRing::verifyBootstrap(
        encoded(futureEnvelope), anchor, kNowMs);
    ok = expect(!future.ok
                    && future.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-envelope-fields-invalid"),
                "future key-ring signature time was accepted") && ok;

    const auto wrongClock = UpdateSigningKeyRing::verifyBootstrap(
        encoded(envelope), anchor, 0);
    ok = expect(!wrongClock.ok
                    && wrongClock.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-clock-invalid"),
                "invalid key-ring clock was accepted") && ok;

    const auto invalidAnchor = UpdateSigningKeyRing::testingTrustAnchor(
        QStringLiteral("Uppercase"), root.publicKeyBase64(), &errorCode);
    ok = expect(!invalidAnchor.isValid()
                    && errorCode
                        == QStringLiteral("update-signing-trust-anchor-invalid"),
                "invalid trust-anchor Key ID was accepted") && ok;
    return ok;
}

bool rotationAndArtifactTests(const SigningKey &root, const SigningKey &next)
{
    bool ok = true;
    const auto anchor = UpdateSigningKeyRing::testingTrustAnchor(
        QStringLiteral("artifact-2026-01"), root.publicKeyBase64());
    QJsonObject rootRecord = keyObject(
        QStringLiteral("artifact-2026-01"), root, 1000, 3000, false,
        QJsonValue(QJsonValue::Null));
    const QJsonObject ring1 = ringObject(1, QJsonArray{rootRecord});
    const QJsonObject envelope1 = signedRingObject(
        QStringLiteral("artifact-2026-01"), 1500, ring1, root);
    const auto generation1 = UpdateSigningKeyRing::verifyBootstrap(
        encoded(envelope1), anchor, kNowMs);
    if (!expect(generation1.ok, "rotation fixture bootstrap failed")) return false;

    const QJsonObject nextRecord = keyObject(
        QStringLiteral("artifact-2026-02"), next, 1800, 5000, false,
        QJsonValue(QStringLiteral("artifact-2026-01")));
    const QJsonObject ring2 = ringObject(
        2, QJsonArray{rootRecord, nextRecord});
    const QJsonObject envelope2 = signedRingObject(
        QStringLiteral("artifact-2026-01"), 1900, ring2, root);
    const auto generation2 = UpdateSigningKeyRing::verifyRotation(
        encoded(envelope2), generation1.authority, kNowMs);
    ok = expect(generation2.ok && !generation2.idempotent
                    && generation2.authority.isValid()
                    && generation2.authority.generation() == 2
                    && generation2.authority.ringIdentity()
                        != generation1.authority.ringIdentity()
                    && generation2.authority.authorityIdentity()
                        != generation1.authority.authorityIdentity(),
                "valid update key rotation was rejected") && ok;

    const auto delayedGeneration2 = UpdateSigningKeyRing::verifyRotation(
        encoded(envelope2), generation1.authority, 3500);
    ok = expect(!delayedGeneration2.ok
                    && delayedGeneration2.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-signer-inactive"),
                "expired signer introduced a key through backdated rotation")
        && ok;

    const QJsonObject delayedRing3 = ringObject(
        3, QJsonArray{rootRecord, nextRecord});
    const QJsonObject delayedEnvelope3 = signedRingObject(
        QStringLiteral("artifact-2026-02"), 4500, delayedRing3, next);
    const auto delayedGeneration3 = UpdateSigningKeyRing::verifyRotation(
        encoded(delayedEnvelope3), generation2.authority, 6000);
    ok = expect(!delayedGeneration3.ok
                    && delayedGeneration3.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-signer-inactive"),
                "expired rotated signer extended authority through backdating")
        && ok;

    const auto expiredSignerReplay = UpdateSigningKeyRing::verifyRotation(
        encoded(envelope2), generation2.authority, 3500);
    ok = expect(expiredSignerReplay.ok && expiredSignerReplay.idempotent
                    && expiredSignerReplay.authority.authorityIdentity()
                        == generation2.authority.authorityIdentity(),
                "exact key-ring replay failed after its rotation signer expired")
        && ok;

    const QByteArray historicalPayload = QByteArrayLiteral("historical receipt");
    const auto historical = UpdateSigningKeyRing::verifyArtifactSetSignature(
        generation2.authority, QStringLiteral("artifact-2026-01"),
        1500, 3500, false, historicalPayload, root.sign(historicalPayload));
    ok = expect(historical.ok
                    && historical.signerKeyId
                        == QStringLiteral("artifact-2026-01")
                    && historical.signerKeyIdentity
                        == rootRecord.value(
                            QStringLiteral("key_identity")).toString()
                    && historical.ringGeneration == 2
                    && historical.ringIdentity
                        == generation2.authority.ringIdentity(),
                "naturally expired historical receipt key was rejected") && ok;
    const auto expiredCandidate =
        UpdateSigningKeyRing::verifyArtifactSetSignature(
            generation2.authority, QStringLiteral("artifact-2026-01"),
            1500, 3500, true, historicalPayload, root.sign(historicalPayload));
    ok = expect(!expiredCandidate.ok
                    && expiredCandidate.errorCode
                        == QStringLiteral("artifact-set-signing-key-expired"),
                "expired key was accepted for a new candidate") && ok;

    const QByteArray candidatePayload = QByteArrayLiteral("new candidate");
    const auto beforeAdmission =
        UpdateSigningKeyRing::verifyArtifactSetSignature(
            generation2.authority, QStringLiteral("artifact-2026-02"),
            1850, 2500, false, candidatePayload,
            next.sign(candidatePayload));
    ok = expect(!beforeAdmission.ok
                    && beforeAdmission.errorCode
                        == QStringLiteral(
                            "artifact-set-signing-key-not-yet-admitted"),
                "rotated key verified an artifact predating its admission")
        && ok;
    const auto candidate = UpdateSigningKeyRing::verifyArtifactSetSignature(
        generation2.authority, QStringLiteral("artifact-2026-02"),
        2200, 2500, true, candidatePayload, next.sign(candidatePayload));
    ok = expect(candidate.ok
                    && candidate.signerKeyId
                        == QStringLiteral("artifact-2026-02")
                    && candidate.ringAuthorityIdentity
                        == generation2.authority.authorityIdentity(),
                "current rotated candidate key was rejected") && ok;

    const auto wrongKey = UpdateSigningKeyRing::verifyArtifactSetSignature(
        generation2.authority, QStringLiteral("artifact-2026-02"),
        2200, 2500, true, candidatePayload, root.sign(candidatePayload));
    ok = expect(!wrongKey.ok
                    && wrongKey.errorCode
                        == QStringLiteral("artifact-set-signature-invalid"),
                "Key ID and artifact signature substitution was accepted") && ok;

    const auto unknownKey = UpdateSigningKeyRing::verifyArtifactSetSignature(
        generation2.authority, QStringLiteral("artifact-unknown"),
        2200, 2500, true, candidatePayload, next.sign(candidatePayload));
    ok = expect(!unknownKey.ok
                    && unknownKey.errorCode
                        == QStringLiteral("artifact-set-signing-key-unknown"),
                "unknown artifact signing Key ID was accepted") && ok;
    const auto notYetValid = UpdateSigningKeyRing::verifyArtifactSetSignature(
        generation2.authority, QStringLiteral("artifact-2026-02"),
        1799, 2500, true, candidatePayload, next.sign(candidatePayload));
    ok = expect(!notYetValid.ok
                    && notYetValid.errorCode
                        == QStringLiteral(
                            "artifact-set-signing-key-not-yet-valid"),
                "artifact signature before its key validity was accepted") && ok;
    const auto signingExpiryBoundary =
        UpdateSigningKeyRing::verifyArtifactSetSignature(
            generation2.authority, QStringLiteral("artifact-2026-02"),
            5000, 5000, false, candidatePayload,
            next.sign(candidatePayload));
    ok = expect(!signingExpiryBoundary.ok
                    && signingExpiryBoundary.errorCode
                        == QStringLiteral("artifact-set-signing-key-expired"),
                "exclusive signing-key validity boundary was accepted") && ok;
    const auto currentExpiryBoundary =
        UpdateSigningKeyRing::verifyArtifactSetSignature(
            generation2.authority, QStringLiteral("artifact-2026-02"),
            2200, 5000, true, candidatePayload,
            next.sign(candidatePayload));
    ok = expect(!currentExpiryBoundary.ok
                    && currentExpiryBoundary.errorCode
                        == QStringLiteral("artifact-set-signing-key-expired"),
                "expired key retained new-candidate authority") && ok;
    const auto invalidAuthority =
        UpdateSigningKeyRing::verifyArtifactSetSignature(
            UpdateSigningKeyRing::Authority{},
            QStringLiteral("artifact-2026-02"), 2200, 2500, true,
            candidatePayload, next.sign(candidatePayload));
    ok = expect(!invalidAuthority.ok
                    && invalidAuthority.errorCode
                        == QStringLiteral(
                            "update-signing-key-ring-authority-invalid"),
                "invalid Key Ring authority verified an artifact") && ok;

    QJsonObject shortenedRoot = rootRecord;
    shortenedRoot.insert(QStringLiteral("valid_until_ms"), 1400.0);
    shortenedRoot = reidentifyKey(shortenedRoot);
    const QJsonObject shortenedRing = ringObject(
        3, QJsonArray{shortenedRoot, nextRecord});
    const QJsonObject shortenedEnvelope = signedRingObject(
        QStringLiteral("artifact-2026-02"), 2300, shortenedRing, next);
    const auto shortenedGeneration = UpdateSigningKeyRing::verifyRotation(
        encoded(shortenedEnvelope), generation2.authority, 2500);
    ok = expect(shortenedGeneration.ok,
                "valid historical-key cutoff rotation was rejected") && ok;
    const auto cutoffHistorical =
        UpdateSigningKeyRing::verifyArtifactSetSignature(
            shortenedGeneration.authority,
            QStringLiteral("artifact-2026-01"), 1500, 3500, false,
            historicalPayload, root.sign(historicalPayload));
    ok = expect(!cutoffHistorical.ok
                    && cutoffHistorical.errorCode
                        == QStringLiteral("artifact-set-signing-key-expired"),
                "receipt after a shortened historical cutoff was accepted") && ok;

    rootRecord.insert(QStringLiteral("revoked"), true);
    QByteArray rootPayload = QByteArrayLiteral(
        "aegisy-update-signing-key/0.1\n");
    appendKey(&rootPayload, QByteArrayLiteral("key"), rootRecord, false);
    rootRecord.insert(
        QStringLiteral("key_identity"),
        identity(rootPayload, QStringLiteral("update-signing-key:sha256:")));
    const QJsonObject ring3 = ringObject(
        3, QJsonArray{rootRecord, nextRecord});
    const QJsonObject envelope3 = signedRingObject(
        QStringLiteral("artifact-2026-02"), 2300, ring3, next);
    const auto generation3 = UpdateSigningKeyRing::verifyRotation(
        encoded(envelope3), generation2.authority, 2500);
    ok = expect(generation3.ok,
                "valid retroactive revocation rotation was rejected") && ok;
    const auto revokedHistorical =
        UpdateSigningKeyRing::verifyArtifactSetSignature(
            generation3.authority, QStringLiteral("artifact-2026-01"),
            1500, 3500, false, historicalPayload,
            root.sign(historicalPayload));
    ok = expect(!revokedHistorical.ok
                    && revokedHistorical.errorCode
                        == QStringLiteral("artifact-set-signing-key-revoked"),
                "revoked key retained historical receipt authority") && ok;

    QJsonObject unrevokedRoot = rootRecord;
    unrevokedRoot.insert(QStringLiteral("revoked"), false);
    QByteArray unrevokedPayload = QByteArrayLiteral(
        "aegisy-update-signing-key/0.1\n");
    appendKey(&unrevokedPayload, QByteArrayLiteral("key"), unrevokedRoot, false);
    unrevokedRoot.insert(
        QStringLiteral("key_identity"),
        identity(unrevokedPayload,
                 QStringLiteral("update-signing-key:sha256:")));
    const QJsonObject illegalRing4 = ringObject(
        4, QJsonArray{unrevokedRoot, nextRecord});
    const QJsonObject illegalEnvelope4 = signedRingObject(
        QStringLiteral("artifact-2026-02"), 2400, illegalRing4, next);
    expectRotationError(
        illegalEnvelope4, generation3.authority,
        QStringLiteral("update-signing-key-revocation-reversed"), &ok,
        "revocation reversal was accepted");
    return ok;
}

bool admissionHistoryBindingTests(const SigningKey &root,
                                  const SigningKey &next)
{
    bool ok = true;
    const auto anchor = UpdateSigningKeyRing::testingTrustAnchor(
        QStringLiteral("artifact-2026-01"), root.publicKeyBase64());
    const QJsonObject rootRecord = keyObject(
        QStringLiteral("artifact-2026-01"), root, 1000, 3000, false,
        QJsonValue(QJsonValue::Null));
    const QJsonObject nextRecord = keyObject(
        QStringLiteral("artifact-2026-02"), next, 1800, 5000, false,
        QJsonValue(QStringLiteral("artifact-2026-01")));
    const QJsonObject ring1 = ringObject(1, QJsonArray{rootRecord});
    const auto generation1 = UpdateSigningKeyRing::verifyBootstrap(
        encoded(signedRingObject(QStringLiteral("artifact-2026-01"), 1500,
                                 ring1, root)),
        anchor, kNowMs);
    if (!expect(generation1.ok,
                "admission-history fixture bootstrap failed")) {
        return false;
    }

    const QJsonObject ring2 = ringObject(
        2, QJsonArray{rootRecord, nextRecord});
    const auto earlyGeneration2 = UpdateSigningKeyRing::verifyRotation(
        encoded(signedRingObject(QStringLiteral("artifact-2026-01"), 1900,
                                 ring2, root)),
        generation1.authority, kRotationNowMs);
    const auto lateGeneration2 = UpdateSigningKeyRing::verifyRotation(
        encoded(signedRingObject(QStringLiteral("artifact-2026-01"), 2000,
                                 ring2, root)),
        generation1.authority, kRotationNowMs);
    if (!expect(earlyGeneration2.ok && lateGeneration2.ok,
                "admission-history Ring 2 fixture failed")) {
        return false;
    }
    ok = expect(
             earlyGeneration2.authority.ringIdentity()
                     == lateGeneration2.authority.ringIdentity()
                 && earlyGeneration2.authority.authorityIdentity()
                     != lateGeneration2.authority.authorityIdentity(),
             "different Ring 2 admission histories shared authority identity")
        && ok;

    const QJsonObject ring3 = ringObject(
        3, QJsonArray{rootRecord, nextRecord});
    const QJsonObject sharedEnvelope3 = signedRingObject(
        QStringLiteral("artifact-2026-02"), 2300, ring3, next);
    const auto earlyGeneration3 = UpdateSigningKeyRing::verifyRotation(
        encoded(sharedEnvelope3), earlyGeneration2.authority, kRotationNowMs);
    const auto lateGeneration3 = UpdateSigningKeyRing::verifyRotation(
        encoded(sharedEnvelope3), lateGeneration2.authority, kRotationNowMs);
    if (!expect(earlyGeneration3.ok && lateGeneration3.ok,
                "admission-history Ring 3 convergence failed")) {
        return false;
    }
    ok = expect(
             earlyGeneration3.authority.ringIdentity()
                     == lateGeneration3.authority.ringIdentity()
                 && earlyGeneration3.authority.authorityIdentity()
                     != lateGeneration3.authority.authorityIdentity(),
             "converged Ring 3 discarded prior admission history")
        && ok;

    const QByteArray payload = QByteArrayLiteral(
        "admission-history-bound artifact");
    const QString signature = next.sign(payload);
    const auto admittedEarly =
        UpdateSigningKeyRing::verifyArtifactSetSignature(
            earlyGeneration3.authority,
            QStringLiteral("artifact-2026-02"), 1950, 2500, false,
            payload, signature);
    const auto admittedLate =
        UpdateSigningKeyRing::verifyArtifactSetSignature(
            lateGeneration3.authority,
            QStringLiteral("artifact-2026-02"), 1950, 2500, false,
            payload, signature);
    ok = expect(admittedEarly.ok,
                "artifact after early key admission was rejected") && ok;
    ok = expect(!admittedLate.ok
                    && admittedLate.errorCode
                        == QStringLiteral(
                            "artifact-set-signing-key-not-yet-admitted"),
                "artifact before late key admission was accepted") && ok;
    return ok;
}

bool rotationNegativeTests(const SigningKey &root, const SigningKey &next,
                           const SigningKey &third)
{
    bool ok = true;
    const auto anchor = UpdateSigningKeyRing::testingTrustAnchor(
        QStringLiteral("artifact-2026-01"), root.publicKeyBase64());
    const QJsonObject rootRecord = keyObject(
        QStringLiteral("artifact-2026-01"), root, 1000, 3000, false,
        QJsonValue(QJsonValue::Null));
    const QJsonObject ring1 = ringObject(1, QJsonArray{rootRecord});
    const auto generation1 = UpdateSigningKeyRing::verifyBootstrap(
        encoded(signedRingObject(QStringLiteral("artifact-2026-01"), 1500,
                                 ring1, root)),
        anchor, kNowMs);
    if (!expect(generation1.ok, "negative rotation fixture bootstrap failed")) {
        return false;
    }
    const QJsonObject nextRecord = keyObject(
        QStringLiteral("artifact-2026-02"), next, 1800, 5000, false,
        QJsonValue(QStringLiteral("artifact-2026-01")));
    const QJsonObject ring2 = ringObject(2, QJsonArray{rootRecord, nextRecord});
    const QJsonObject envelope2 = signedRingObject(
        QStringLiteral("artifact-2026-01"), 1900, ring2, root);
    const auto generation2 = UpdateSigningKeyRing::verifyRotation(
        encoded(envelope2), generation1.authority, kNowMs);
    if (!expect(generation2.ok, "negative rotation fixture rotation failed")) {
        return false;
    }

    const QJsonObject resignedSameRing = signedRingObject(
        QStringLiteral("artifact-2026-01"), 1950, ring2, root);
    expectRotationError(
        resignedSameRing, generation2.authority,
        QStringLiteral("update-signing-key-ring-generation-conflict"), &ok,
        "same-generation conflicting signed envelope was accepted");

    const QJsonObject gapRing = ringObject(4, QJsonArray{rootRecord, nextRecord});
    expectRotationError(
        signedRingObject(QStringLiteral("artifact-2026-02"), 2100,
                         gapRing, next),
        generation2.authority,
        QStringLiteral("update-signing-key-ring-generation-gap"), &ok,
        "key-ring generation gap was accepted");

    expectRotationError(
        signedRingObject(QStringLiteral("artifact-2026-01"), 1900,
                         ring1, root),
        generation2.authority,
        QStringLiteral("update-signing-key-ring-generation-rollback"), &ok,
        "key-ring generation rollback was accepted");

    const QJsonObject signedTimeRollbackRing = ringObject(
        3, QJsonArray{rootRecord, nextRecord});
    expectRotationError(
        signedRingObject(QStringLiteral("artifact-2026-01"), 1850,
                         signedTimeRollbackRing, root),
        generation2.authority,
        QStringLiteral("update-signing-key-ring-signed-time-rollback"), &ok,
        "key-ring signing time rollback was accepted");

    QJsonObject orphanedNext = nextRecord;
    orphanedNext.insert(QStringLiteral("replaces"), QJsonValue::Null);
    QByteArray orphanedPayload = QByteArrayLiteral(
        "aegisy-update-signing-key/0.1\n");
    appendKey(&orphanedPayload, QByteArrayLiteral("key"), orphanedNext, false);
    orphanedNext.insert(
        QStringLiteral("key_identity"),
        identity(orphanedPayload,
                 QStringLiteral("update-signing-key:sha256:")));
    const QJsonObject removedRing = ringObject(3, QJsonArray{orphanedNext});
    expectRotationError(
        signedRingObject(QStringLiteral("artifact-2026-02"), 2100,
                         removedRing, next),
        generation2.authority,
        QStringLiteral("update-signing-key-removed"), &ok,
        "historical update signing key removal was accepted");

    QJsonObject widenedRoot = rootRecord;
    widenedRoot.insert(QStringLiteral("valid_until_ms"), 4000.0);
    QByteArray widenedPayload = QByteArrayLiteral(
        "aegisy-update-signing-key/0.1\n");
    appendKey(&widenedPayload, QByteArrayLiteral("key"), widenedRoot, false);
    widenedRoot.insert(
        QStringLiteral("key_identity"),
        identity(widenedPayload, QStringLiteral("update-signing-key:sha256:")));
    const QJsonObject widenedRing = ringObject(
        3, QJsonArray{widenedRoot, nextRecord});
    expectRotationError(
        signedRingObject(QStringLiteral("artifact-2026-02"), 2100,
                         widenedRing, next),
        generation2.authority,
        QStringLiteral("update-signing-key-validity-widened"), &ok,
        "signing-key validity widening was accepted");

    const QJsonObject branchRecord = keyObject(
        QStringLiteral("artifact-2026-03"), third, 1900, 5000, false,
        QJsonValue(QStringLiteral("artifact-2026-01")));
    const QJsonObject branchRing = ringObject(
        3, QJsonArray{rootRecord, nextRecord, branchRecord});
    expectRotationError(
        signedRingObject(QStringLiteral("artifact-2026-02"), 2100,
                         branchRing, next),
        generation2.authority,
        QStringLiteral("update-signing-key-lineage-branch"), &ok,
        "signing-key lineage branch was accepted");

    const QJsonObject selfSignedNext = signedRingObject(
        QStringLiteral("artifact-2026-03"), 2100,
        ringObject(3, QJsonArray{
            rootRecord, nextRecord,
            keyObject(QStringLiteral("artifact-2026-03"), third,
                      1900, 5000, false,
                      QJsonValue(QStringLiteral("artifact-2026-02")))
        }),
        third);
    expectRotationError(
        selfSignedNext, generation2.authority,
        QStringLiteral("update-signing-key-ring-signer-unknown"), &ok,
        "new signing key authorized its own admission");

    QJsonObject reversedRing = ring2;
    const QJsonArray originalKeys =
        reversedRing.value(QStringLiteral("keys")).toArray();
    const QJsonArray reversedKeys{
        originalKeys.at(1),
        originalKeys.at(0),
    };
    reversedRing = ringObject(3, reversedKeys);
    expectRotationError(
        signedRingObject(QStringLiteral("artifact-2026-02"), 2100,
                         reversedRing, next),
        generation2.authority,
        QStringLiteral("update-signing-key-ring-order-invalid"), &ok,
        "unsorted update signing Key IDs were accepted");

    QJsonObject duplicatePublic = keyObject(
        QStringLiteral("artifact-2026-03"), next, 1900, 5000, false,
        QJsonValue(QStringLiteral("artifact-2026-02")));
    const QJsonObject duplicatePublicRing = ringObject(
        3, QJsonArray{rootRecord, nextRecord, duplicatePublic});
    expectRotationError(
        signedRingObject(QStringLiteral("artifact-2026-02"), 2100,
                         duplicatePublicRing, next),
        generation2.authority,
        QStringLiteral("update-signing-key-duplicate-public-key"), &ok,
        "duplicate public key under a new Key ID was accepted");
    return ok;
}

bool envelopeChainTests(const SigningKey &root, const SigningKey &next,
                        const SigningKey &third)
{
    using UpdateSigningKeyRing::EnvelopeChainStatus;

    bool ok = true;
    const auto anchor = UpdateSigningKeyRing::testingTrustAnchor(
        QStringLiteral("chain-root"), root.publicKeyBase64());
    const QJsonObject rootRecord =
        keyObject(QStringLiteral("chain-root"), root, 1000, 10000, false,
                  QJsonValue(QJsonValue::Null));
    const QJsonObject nextRecord =
        keyObject(QStringLiteral("chain-second"), next, 1800, 10000, false,
                  QJsonValue(QStringLiteral("chain-root")));
    const QJsonObject thirdRecord =
        keyObject(QStringLiteral("chain-third"), third, 2200, 10000, false,
                  QJsonValue(QStringLiteral("chain-second")));
    const QJsonObject ring1 = ringObject(1, QJsonArray{rootRecord});
    const QJsonObject ring2 = ringObject(2, QJsonArray{rootRecord, nextRecord});
    const QJsonObject ring3 =
        ringObject(3, QJsonArray{rootRecord, nextRecord, thirdRecord});
    const QByteArray envelope1 = encoded(
        signedRingObject(QStringLiteral("chain-root"), 1500, ring1, root));
    const QByteArray envelope2 = encoded(
        signedRingObject(QStringLiteral("chain-root"), 1900, ring2, root));
    const QByteArray envelope3 = encoded(
        signedRingObject(QStringLiteral("chain-second"), 2300, ring3, next));
    const QVector<QByteArray> activeChain{envelope1, envelope2, envelope3};

    const auto authoritative =
        UpdateSigningKeyRing::verifyEnvelopeChain(activeChain, anchor, 2500);
    ok = expect(
             authoritative.status == EnvelopeChainStatus::Authoritative &&
                 authoritative.errorCode.isEmpty() &&
                 authoritative.strictVerificationError.isEmpty() &&
                 authoritative.authority.isValid() &&
                 authoritative.generation == 3 &&
                 authoritative.generation ==
                     authoritative.authority.generation() &&
                 authoritative.ringIdentity ==
                     ring3.value(QStringLiteral("ring_identity")).toString() &&
                 authoritative.ringIdentity ==
                     authoritative.authority.ringIdentity() &&
                 authoritative.trustAnchorIdentity == anchor.anchorIdentity() &&
                 authoritative.trustAnchorIdentity ==
                     authoritative.authority.trustAnchorIdentity() &&
                 authoritative.authorityIdentity ==
                     authoritative.authority.authorityIdentity() &&
                 authoritative.checkpoints.size() == 3 &&
                 authoritative.checkpoints.at(0).generation == 1 &&
                 authoritative.checkpoints.at(0).ringIdentity ==
                     ring1.value(QStringLiteral("ring_identity")).toString() &&
                 authoritative.checkpoints.at(1).generation == 2 &&
                 authoritative.checkpoints.at(1).ringIdentity ==
                     ring2.value(QStringLiteral("ring_identity")).toString() &&
                 authoritative.checkpoints.at(2).generation == 3 &&
                 authoritative.checkpoints.at(2).authorityIdentity ==
                     authoritative.authorityIdentity,
             "active generation 1->2->3 chain was not authoritative") &&
         ok;

    const auto empty =
        UpdateSigningKeyRing::verifyEnvelopeChain({}, anchor, 2500);
    ok = expect(empty.status == EnvelopeChainStatus::Invalid &&
                    empty.errorCode ==
                        QStringLiteral("update-signing-key-ring-chain-empty") &&
                    empty.strictVerificationError == empty.errorCode &&
                    !empty.authority.isValid() && empty.generation == 0 &&
                    empty.ringIdentity.isEmpty() &&
                    empty.trustAnchorIdentity.isEmpty() &&
                    empty.authorityIdentity.isEmpty() &&
                    empty.checkpoints.isEmpty(),
                "empty key-ring chain was not rejected without metadata") &&
         ok;

    const auto invalidClock =
        UpdateSigningKeyRing::verifyEnvelopeChain({envelope1}, anchor, 0);
    ok = expect(
             invalidClock.status == EnvelopeChainStatus::Invalid &&
                 invalidClock.errorCode ==
                     QStringLiteral("update-signing-key-ring-clock-invalid") &&
                 invalidClock.strictVerificationError ==
                     invalidClock.errorCode &&
                 !invalidClock.authority.isValid(),
             "invalid chain verification clock was accepted") &&
         ok;

    const auto duplicate = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, envelope2, envelope2}, anchor, 2500);
    ok = expect(duplicate.status == EnvelopeChainStatus::Invalid &&
                    duplicate.errorCode ==
                        QStringLiteral(
                            "update-signing-key-ring-chain-duplicate") &&
                    duplicate.strictVerificationError == duplicate.errorCode &&
                    !duplicate.authority.isValid(),
                "duplicate envelope in a key-ring chain was accepted") &&
         ok;

    const auto outOfOrder = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, envelope3, envelope2}, anchor, 2500);
    ok = expect(
             outOfOrder.status == EnvelopeChainStatus::Invalid &&
                 outOfOrder.errorCode ==
                     QStringLiteral("update-signing-key-ring-signer-unknown") &&
                 outOfOrder.strictVerificationError == outOfOrder.errorCode &&
                 !outOfOrder.authority.isValid(),
             "out-of-order key-ring chain was accepted") &&
         ok;

    const QJsonObject ring4 =
        ringObject(4, QJsonArray{rootRecord, nextRecord, thirdRecord});
    const QByteArray envelope4 = encoded(
        signedRingObject(QStringLiteral("chain-second"), 2400, ring4, next));
    const auto gap = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, envelope2, envelope4}, anchor, 2500);
    ok = expect(
             gap.status == EnvelopeChainStatus::Invalid &&
                 gap.errorCode ==
                     QStringLiteral("update-signing-key-ring-generation-gap") &&
                 gap.strictVerificationError == gap.errorCode &&
                 !gap.authority.isValid(),
             "generation gap in a key-ring chain was accepted") &&
         ok;

    const QByteArray futureEnvelope = encoded(
        signedRingObject(QStringLiteral("chain-second"), 2501, ring3, next));
    const auto future = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, envelope2, futureEnvelope}, anchor, 2500);
    ok =
        expect(future.status == EnvelopeChainStatus::Invalid &&
                   future.errorCode ==
                       QStringLiteral(
                           "update-signing-key-ring-envelope-fields-invalid") &&
                   future.strictVerificationError == future.errorCode &&
                   !future.authority.isValid(),
               "future-signed key-ring chain became cached authority") &&
        ok;

    const auto expiredRoot =
        UpdateSigningKeyRing::verifyEnvelopeChain(activeChain, anchor, 11000);
    ok = expect(
             expiredRoot.status ==
                     EnvelopeChainStatus::CachedButNotAuthoritative &&
                 expiredRoot.errorCode.isEmpty() &&
                 expiredRoot.strictVerificationError ==
                     QStringLiteral(
                         "update-signing-key-ring-bootstrap-root-invalid") &&
                 !expiredRoot.authority.isValid() &&
                 expiredRoot.generation == 3 &&
                 expiredRoot.ringIdentity ==
                     ring3.value(QStringLiteral("ring_identity")).toString() &&
                 expiredRoot.trustAnchorIdentity == anchor.anchorIdentity() &&
                 !expiredRoot.authorityIdentity.isEmpty() &&
                 expiredRoot.checkpoints.size() == 3 &&
                 expiredRoot.checkpoints.constLast().authorityIdentity ==
                     expiredRoot.authorityIdentity,
             "historically valid chain with an expired Root was not "
             "cached-only") &&
         ok;

    QJsonObject tamperedEnvelope2 = QJsonDocument::fromJson(envelope2).object();
    QString tamperedSignature =
        tamperedEnvelope2.value(QStringLiteral("signature")).toString();
    tamperedSignature[0] = tamperedSignature.at(0) == QLatin1Char('A')
                               ? QLatin1Char('B')
                               : QLatin1Char('A');
    tamperedEnvelope2.insert(QStringLiteral("signature"), tamperedSignature);
    const auto tamperedExpired = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, encoded(tamperedEnvelope2), envelope3}, anchor, 11000);
    ok = expect(tamperedExpired.status == EnvelopeChainStatus::Invalid &&
                    tamperedExpired.errorCode ==
                        QStringLiteral(
                            "update-signing-key-ring-signature-invalid") &&
                    tamperedExpired.strictVerificationError ==
                        QStringLiteral(
                            "update-signing-key-ring-bootstrap-root-invalid") &&
                    !tamperedExpired.authority.isValid() &&
                    tamperedExpired.generation == 0 &&
                    tamperedExpired.authorityIdentity.isEmpty() &&
                    tamperedExpired.checkpoints.isEmpty(),
                "tampered expired envelope was accepted as cached-only") &&
         ok;

    const QByteArray admissionBackdatedEnvelope = encoded(
        signedRingObject(QStringLiteral("chain-second"), 1850, ring3, next));
    const auto admissionBackdated = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, envelope2, admissionBackdatedEnvelope}, anchor, 11000);
    ok =
        expect(admissionBackdated.status == EnvelopeChainStatus::Invalid &&
                   admissionBackdated.errorCode ==
                       QStringLiteral(
                           "update-signing-key-ring-signer-not-yet-admitted") &&
                   admissionBackdated.strictVerificationError ==
                       QStringLiteral(
                           "update-signing-key-ring-bootstrap-root-invalid") &&
                   !admissionBackdated.authority.isValid(),
               "expired chain bypassed signer admission history") &&
        ok;

    const QByteArray signedTimeRollbackEnvelope = encoded(
        signedRingObject(QStringLiteral("chain-root"), 1800, ring3, root));
    const auto signedTimeRollback = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, envelope2, signedTimeRollbackEnvelope}, anchor, 11000);
    ok = expect(signedTimeRollback.status == EnvelopeChainStatus::Invalid &&
                    signedTimeRollback.errorCode ==
                        QStringLiteral(
                            "update-signing-key-ring-signed-time-rollback") &&
                    signedTimeRollback.strictVerificationError ==
                        QStringLiteral(
                            "update-signing-key-ring-bootstrap-root-invalid") &&
                    !signedTimeRollback.authority.isValid(),
                "expired chain bypassed signed-time monotonicity") &&
         ok;

    const QJsonObject orphanThirdRecord =
        keyObject(QStringLiteral("chain-third"), third, 2200, 10000, false,
                  QJsonValue(QStringLiteral("chain-missing")));
    const QJsonObject orphanRing3 =
        ringObject(3, QJsonArray{rootRecord, nextRecord, orphanThirdRecord});
    const QByteArray orphanEnvelope3 = encoded(signedRingObject(
        QStringLiteral("chain-second"), 2300, orphanRing3, next));
    const auto invalidLineage = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, envelope2, orphanEnvelope3}, anchor, 11000);
    ok = expect(invalidLineage.status == EnvelopeChainStatus::Invalid &&
                    invalidLineage.errorCode ==
                        QStringLiteral("update-signing-key-lineage-unknown") &&
                    invalidLineage.strictVerificationError ==
                        QStringLiteral(
                            "update-signing-key-ring-bootstrap-root-invalid") &&
                    !invalidLineage.authority.isValid(),
                "expired chain bypassed lineage validation") &&
         ok;

    QJsonObject wrongIdentityRing3 = ring3;
    wrongIdentityRing3.insert(
        QStringLiteral("ring_identity"),
        QStringLiteral("update-signing-key-ring:sha256:"
                       "0000000000000000000000000000000000000000000000000000000"
                       "000000000"));
    const QByteArray wrongIdentityEnvelope3 = encoded(signedRingObject(
        QStringLiteral("chain-second"), 2300, wrongIdentityRing3, next));
    const auto invalidIdentity = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, envelope2, wrongIdentityEnvelope3}, anchor, 11000);
    ok = expect(invalidIdentity.status == EnvelopeChainStatus::Invalid &&
                    invalidIdentity.errorCode ==
                        QStringLiteral(
                            "update-signing-key-ring-identity-mismatch") &&
                    invalidIdentity.strictVerificationError ==
                        QStringLiteral(
                            "update-signing-key-ring-bootstrap-root-invalid") &&
                    !invalidIdentity.authority.isValid(),
                "expired chain bypassed Ring identity validation") &&
         ok;

    const QJsonObject shortNextRecord =
        keyObject(QStringLiteral("chain-second"), next, 1800, 3000, false,
                  QJsonValue(QStringLiteral("chain-root")));
    const QJsonObject shortRing2 =
        ringObject(2, QJsonArray{rootRecord, shortNextRecord});
    const QJsonObject shortRing3 =
        ringObject(3, QJsonArray{rootRecord, shortNextRecord, thirdRecord});
    const QByteArray shortEnvelope2 = encoded(
        signedRingObject(QStringLiteral("chain-root"), 1900, shortRing2, root));
    const QByteArray shortEnvelope3 = encoded(signedRingObject(
        QStringLiteral("chain-second"), 2300, shortRing3, next));
    const auto expiredIntermediate = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, shortEnvelope2, shortEnvelope3}, anchor, 3500);
    ok = expect(expiredIntermediate.status ==
                        EnvelopeChainStatus::CachedButNotAuthoritative &&
                    expiredIntermediate.errorCode.isEmpty() &&
                    expiredIntermediate.strictVerificationError ==
                        QStringLiteral(
                            "update-signing-key-ring-signer-inactive") &&
                    !expiredIntermediate.authority.isValid() &&
                    expiredIntermediate.generation == 3 &&
                    expiredIntermediate.ringIdentity ==
                        shortRing3.value(QStringLiteral("ring_identity"))
                            .toString() &&
                    expiredIntermediate.trustAnchorIdentity ==
                        anchor.anchorIdentity() &&
                    !expiredIntermediate.authorityIdentity.isEmpty(),
                "expired intermediate signer restored current authority") &&
         ok;

    const QJsonObject shortenedRoot =
        keyObject(QStringLiteral("chain-root"), root, 1000, 3000, false,
                  QJsonValue(QJsonValue::Null));
    const QJsonObject ringOnlyNext = keyObject(
        QStringLiteral("chain-second"), next, 1800, 10000, false,
        QJsonValue(QStringLiteral("chain-root")), {QStringLiteral("key-ring")});
    const QJsonObject usageExpiredRing =
        ringObject(2, QJsonArray{shortenedRoot, ringOnlyNext});
    const QByteArray usageExpiredEnvelope = encoded(signedRingObject(
        QStringLiteral("chain-root"), 2500, usageExpiredRing, root));
    const auto expiredUsage = UpdateSigningKeyRing::verifyEnvelopeChain(
        {envelope1, usageExpiredEnvelope}, anchor, 3500);
    ok =
        expect(expiredUsage.status ==
                       EnvelopeChainStatus::CachedButNotAuthoritative &&
                   expiredUsage.errorCode.isEmpty() &&
                   expiredUsage.strictVerificationError ==
                       QStringLiteral(
                           "update-signing-key-ring-no-current-active-usage") &&
                   !expiredUsage.authority.isValid() &&
                   expiredUsage.generation == 2 &&
                   expiredUsage.ringIdentity ==
                       usageExpiredRing.value(QStringLiteral("ring_identity"))
                           .toString(),
               "historically valid expired usage restored current authority") &&
        ok;
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const SigningKey root('r');
    const SigningKey next('n');
    const SigningKey third('t');
    if (!expect(root.isValid() && next.isValid() && third.isValid(),
                "deterministic Ed25519 test key initialization failed")) {
        return 1;
    }
    bool ok = true;
    ok = bootstrapAndParserTests(root) && ok;
    ok = rotationAndArtifactTests(root, next) && ok;
    ok = admissionHistoryBindingTests(root, next) && ok;
    ok = rotationNegativeTests(root, next, third) && ok;
    ok = envelopeChainTests(root, next, third) && ok;
    return ok ? 0 : 1;
}
