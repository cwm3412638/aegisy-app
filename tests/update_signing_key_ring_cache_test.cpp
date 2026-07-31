#include "update_signing_key_ring_cache.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLockFile>
#include <QStringList>
#include <QTemporaryDir>

#include <openssl/evp.h>

#include <cstdio>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace {

constexpr qint64 kBootstrapNowMs = 2000;
constexpr qint64 kRotationNowMs = 3000;
constexpr qint64 kExpiredRootNowMs = 5000;

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
    appendLine(payload, key, QString::number(value));
}

void appendLine(QByteArray *payload, const QByteArray &key, bool value)
{
    appendLine(payload, key, static_cast<quint64>(value ? 1 : 0));
}

QString identity(const QByteArray &payload, const QString &prefix)
{
    return prefix + QString::fromLatin1(QCryptographicHash::hash(
                                            payload, QCryptographicHash::Sha256)
                                            .toHex());
}

QString continuityHeadIdentity(const QJsonObject &head)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring-continuity-head/0.1\n");
    appendLine(&payload, QByteArrayLiteral("trust_anchor_identity"),
               head.value(QStringLiteral("trust_anchor_identity")).toString());
    appendLine(
        &payload, QByteArrayLiteral("chain_length"),
        static_cast<quint64>(
            head.value(QStringLiteral("chain_length")).toDouble()));
    appendLine(
        &payload, QByteArrayLiteral("latest_generation"),
        static_cast<quint64>(
            head.value(QStringLiteral("latest_generation")).toDouble()));
    appendLine(&payload, QByteArrayLiteral("latest_entry_identity"),
               head.value(QStringLiteral("latest_entry_identity")).toString());
    appendLine(&payload, QByteArrayLiteral("chain_identity"),
               head.value(QStringLiteral("chain_identity")).toString());
    appendLine(
        &payload, QByteArrayLiteral("previous_cache_identity"),
        head.value(QStringLiteral("previous_cache_identity")).toString());
    appendLine(&payload, QByteArrayLiteral("ring_identity"),
               head.value(QStringLiteral("ring_identity")).toString());
    appendLine(
        &payload, QByteArrayLiteral("ring_authority_identity"),
        head.value(QStringLiteral("ring_authority_identity")).toString());
    const QList<QByteArray> falseFields{
        QByteArrayLiteral("update_authorized"),
        QByteArrayLiteral("network_authorized"),
        QByteArrayLiteral("download_authorized"),
        QByteArrayLiteral("install_authorized"),
        QByteArrayLiteral("rollback_authorized"),
        QByteArrayLiteral("resume_authorized"),
        QByteArrayLiteral("execution_authorized"),
        QByteArrayLiteral("anti_rollback_protected"),
        QByteArrayLiteral("anti_deletion_protected"),
        QByteArrayLiteral("trusted_time_available"),
        QByteArrayLiteral("expired_signer_recovery_available"),
    };
    for (const QByteArray &field : falseFields) {
        appendLine(&payload, field, QStringLiteral("false"));
    }
    return identity(
        payload,
        QStringLiteral(
            "update-signing-key-ring-continuity-cache:sha256:"));
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
                m_key, reinterpret_cast<unsigned char *>(publicKey.data()),
                &size) != 1 ||
            size != 32) {
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
        const bool measured =
            context &&
            EVP_DigestSignInit(context, nullptr, nullptr, nullptr, m_key) ==
                1 &&
            EVP_DigestSign(
                context, nullptr, &size,
                reinterpret_cast<const unsigned char *>(payload.constData()),
                static_cast<size_t>(payload.size())) == 1;
        if (!measured || size != 64) {
            EVP_MD_CTX_free(context);
            return {};
        }
        QByteArray signature(static_cast<int>(size), '\0');
        const bool signedPayload =
            EVP_DigestSign(
                context, reinterpret_cast<unsigned char *>(signature.data()),
                &size,
                reinterpret_cast<const unsigned char *>(payload.constData()),
                static_cast<size_t>(payload.size())) == 1;
        EVP_MD_CTX_free(context);
        return signedPayload && size == 64
                   ? QString::fromLatin1(signature.toBase64())
                   : QString();
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
        appendLine(payload, prefix + ".usages." + QByteArray::number(index),
                   usages.at(index).toString());
    }
    if (includeIdentity) {
        appendLine(payload, prefix + ".key_identity",
                   key.value(QStringLiteral("key_identity")).toString());
    }
}

QJsonObject keyObject(const QString &keyId, const SigningKey &key,
                      quint64 validFromMs, quint64 validUntilMs,
                      const QJsonValue &replaces)
{
    QJsonArray usages;
    for (const QString &usage : bothUsages())
        usages.append(usage);
    QJsonObject result{
        {QStringLiteral("key_id"), keyId},
        {QStringLiteral("public_key_base64"),
         QString::fromLatin1(key.publicKeyBase64())},
        {QStringLiteral("valid_from_ms"), static_cast<double>(validFromMs)},
        {QStringLiteral("valid_until_ms"), static_cast<double>(validUntilMs)},
        {QStringLiteral("revoked"), false},
        {QStringLiteral("replaces"), replaces},
        {QStringLiteral("usages"), usages},
        {QStringLiteral("key_identity"), QString()},
    };
    QByteArray payload = QByteArrayLiteral("aegisy-update-signing-key/0.1\n");
    appendKey(&payload, QByteArrayLiteral("key"), result, false);
    result.insert(
        QStringLiteral("key_identity"),
        identity(payload, QStringLiteral("update-signing-key:sha256:")));
    return result;
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
    QByteArray payload =
        QByteArrayLiteral("aegisy-update-signing-key-ring/0.1\n");
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
        identity(payload, QStringLiteral("update-signing-key-ring:sha256:")));
    return ring;
}

QByteArray ringSigningPayload(const QString &signerKeyId, quint64 signedAtMs,
                              const QJsonObject &ring)
{
    QByteArray payload =
        QByteArrayLiteral("aegisy-update-signing-key-ring-signature/0.1\n");
    appendLine(&payload, QByteArrayLiteral("signer_key_id"), signerKeyId);
    appendLine(&payload, QByteArrayLiteral("signed_at_ms"), signedAtMs);
    appendLine(&payload, QByteArrayLiteral("key_ring.generation"),
               static_cast<quint64>(
                   ring.value(QStringLiteral("generation")).toDouble()));
    appendLine(&payload, QByteArrayLiteral("key_ring.identity"),
               ring.value(QStringLiteral("ring_identity")).toString());
    return payload;
}

QByteArray signedRing(const QString &signerKeyId, quint64 signedAtMs,
                      const QJsonObject &ring, const SigningKey &key)
{
    const QByteArray payload =
        ringSigningPayload(signerKeyId, signedAtMs, ring);
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"),
                    QStringLiteral(
                        "aegisy-update-signing-key-ring-signature/0.1")},
                   {QStringLiteral("signer_key_id"), signerKeyId},
                   {QStringLiteral("signed_at_ms"),
                    static_cast<double>(signedAtMs)},
                   {QStringLiteral("key_ring"), ring},
                   {QStringLiteral("payload_identity"),
                    identity(payload,
                             QStringLiteral(
                                 "update-signing-key-ring-payload:sha256:"))},
                   {QStringLiteral("signature"), key.sign(payload)},
               })
        .toJson(QJsonDocument::Compact);
}

bool authoritiesAreFalse(
    const UpdateSigningKeyRingCache::Observation &observation)
{
    return !observation.updateAuthorized && !observation.networkAuthorized &&
           !observation.downloadAuthorized && !observation.installAuthorized &&
           !observation.rollbackAuthorized && !observation.resumeAuthorized &&
           !observation.executionAuthorized &&
           !observation.antiRollbackProtected &&
           !observation.antiDeletionProtected &&
           !observation.trustedTimeAvailable &&
           !observation.expiredSignerRecoveryAvailable;
}

bool authoritiesAreFalse(const UpdateSigningKeyRingCache::CommitResult &result)
{
    return !result.updateAuthorized && !result.networkAuthorized &&
           !result.downloadAuthorized && !result.installAuthorized &&
           !result.rollbackAuthorized && !result.resumeAuthorized &&
           !result.executionAuthorized && !result.antiRollbackProtected &&
           !result.antiDeletionProtected && !result.trustedTimeAvailable &&
           !result.expiredSignerRecoveryAvailable &&
           authoritiesAreFalse(result.observation);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writePrivateFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size() || !file.flush()) {
        return false;
    }
    file.close();
#ifndef Q_OS_WIN
    return QFile::setPermissions(path, QFileDevice::ReadOwner |
                                           QFileDevice::WriteOwner);
#else
    return true;
#endif
}

QString cacheDirectory(const QString &root)
{
    return QDir(root).filePath(
        QStringLiteral("update-signing-key-ring-continuity-v1"));
}

QString objectsDirectory(const QString &root)
{
    return QDir(cacheDirectory(root)).filePath(QStringLiteral("objects"));
}

bool verifyInvalid(const UpdateSigningKeyRingCache::Store &store,
                   const UpdateSigningKeyRing::TrustAnchorAuthority &anchor,
                   const char *message)
{
    const UpdateSigningKeyRingCache::Observation observation =
        store.load(anchor, kRotationNowMs);
    return expect(observation.state ==
                          UpdateSigningKeyRingCache::State::Invalid &&
                      !observation.authority.isValid() &&
                      authoritiesAreFalse(observation),
                  message);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    bool ok = true;
    const SigningKey root('R');
    const SigningKey rotated('S');
    const SigningKey other('T');
    ok = expect(root.isValid() && rotated.isValid() && other.isValid(),
                "cache signing fixture keys are unavailable") &&
         ok;

    QString anchorError;
    const auto anchor = UpdateSigningKeyRing::testingTrustAnchor(
        QStringLiteral("artifact-2026-01"), root.publicKeyBase64(),
        &anchorError);
    const auto otherAnchor = UpdateSigningKeyRing::testingTrustAnchor(
        QStringLiteral("artifact-other"), other.publicKeyBase64(),
        &anchorError);
    ok = expect(anchor.isValid() && otherAnchor.isValid(),
                "cache trust anchors are unavailable") &&
         ok;

    const QJsonObject rootRecord =
        keyObject(QStringLiteral("artifact-2026-01"), root, 1000, 4000,
                  QJsonValue(QJsonValue::Null));
    const QByteArray bootstrapEnvelope =
        signedRing(QStringLiteral("artifact-2026-01"), 1500,
                   ringObject(1, QJsonArray{rootRecord}), root);
    const QJsonObject rotatedRecord =
        keyObject(QStringLiteral("artifact-2026-02"), rotated, 2000, 10000,
                  QStringLiteral("artifact-2026-01"));
    const QByteArray rotationEnvelope =
        signedRing(QStringLiteral("artifact-2026-01"), 2500,
                   ringObject(2, QJsonArray{rootRecord, rotatedRecord}), root);

    QTemporaryDir directory;
    ok =
        expect(directory.isValid(), "cache fixture directory is unavailable") &&
        ok;
    UpdateSigningKeyRingCache::Store store(directory.path());
    auto observation = store.load(anchor, kBootstrapNowMs);
    ok = expect(observation.state == UpdateSigningKeyRingCache::State::Empty &&
                    !observation.present && !observation.integrityVerified &&
                    !observation.authority.isValid() &&
                    authoritiesAreFalse(observation),
                "empty cache fabricated authority") &&
         ok;

    auto commit = store.bootstrap(bootstrapEnvelope, anchor, kBootstrapNowMs);
    ok = expect(
             commit.committed && !commit.idempotent &&
                 commit.postCommitVerified &&
                 commit.observation.state ==
                     UpdateSigningKeyRingCache::State::Authoritative &&
                 commit.observation.integrityVerified &&
                 commit.observation.authority.isValid() &&
                 commit.observation.generation == 1 &&
                 authoritiesAreFalse(commit),
             "valid cache bootstrap did not commit authoritative continuity") &&
         ok;
    const QString generationOneCacheIdentity = commit.observation.cacheIdentity;
    const QString generationOneEntryIdentity =
        commit.observation.latestEntryIdentity;

    commit = store.bootstrap(bootstrapEnvelope, anchor, kBootstrapNowMs);
    ok = expect(!commit.committed && commit.idempotent &&
                    commit.postCommitVerified &&
                    commit.observation.cacheIdentity ==
                        generationOneCacheIdentity &&
                    authoritiesAreFalse(commit),
                "exact cache bootstrap retry was not idempotent") &&
         ok;

    UpdateSigningKeyRingCache::Store reopened(directory.path());
    observation = reopened.load(anchor, kBootstrapNowMs);
    ok = expect(observation.state ==
                        UpdateSigningKeyRingCache::State::Authoritative &&
                    observation.cacheIdentity == generationOneCacheIdentity &&
                    observation.latestEntryIdentity ==
                        generationOneEntryIdentity &&
                    observation.authority.isValid(),
                "cache restart did not replay generation one from the root") &&
         ok;
    const auto wrongAnchor = reopened.load(otherAnchor, kBootstrapNowMs);
    ok =
        expect(wrongAnchor.state == UpdateSigningKeyRingCache::State::Invalid &&
                   !wrongAnchor.authority.isValid() &&
                   authoritiesAreFalse(wrongAnchor),
               "cache accepted a different opaque root authority") &&
        ok;

    const QString headPath = QDir(cacheDirectory(directory.path()))
                                 .filePath(QStringLiteral("current.json"));
    const QString markerPath = QDir(cacheDirectory(directory.path()))
                                   .filePath(QStringLiteral("marker.json"));
    const QByteArray generationOneHead = readFile(headPath);
    const QByteArray markerBytes = readFile(markerPath);
    ok = expect(!generationOneHead.contains("accepted_at") &&
                    !markerBytes.contains("accepted_at") &&
                    !generationOneHead.contains("Authority"),
                "cache persisted admission time or serialized authority") &&
         ok;

    commit = reopened.append(
        rotationEnvelope, anchor, kRotationNowMs,
        QStringLiteral("update-signing-key-ring-continuity-cache:sha256:"
                       "0000000000000000000000000000000000000000000000000000000"
                       "000000000"));
    ok = expect(
             !commit.committed && !commit.idempotent &&
                 commit.errorCode ==
                     QStringLiteral(
                         "update-signing-key-ring-cache-continuity-mismatch") &&
                 authoritiesAreFalse(commit),
             "cache append bypassed expected-head CAS") &&
         ok;

    commit = reopened.append(rotationEnvelope, anchor, kRotationNowMs,
                             generationOneCacheIdentity);
    ok = expect(commit.committed && !commit.idempotent &&
                    commit.postCommitVerified &&
                    commit.observation.generation == 2 &&
                    commit.observation.authority.isValid() &&
                    commit.observation.previousCacheIdentity ==
                        generationOneCacheIdentity &&
                    authoritiesAreFalse(commit),
                "valid cache rotation did not commit") &&
         ok;
    const QString generationTwoCacheIdentity = commit.observation.cacheIdentity;
    const QByteArray generationTwoHead = readFile(headPath);

    commit = reopened.append(rotationEnvelope, anchor, kRotationNowMs,
                             generationOneCacheIdentity);
    ok = expect(
             !commit.committed && commit.idempotent &&
                 commit.postCommitVerified &&
                 commit.observation.cacheIdentity ==
                     generationTwoCacheIdentity &&
                 authoritiesAreFalse(commit),
             "uncertain exact rotation retry did not bind the previous head") &&
         ok;

    observation = reopened.load(anchor, kExpiredRootNowMs);
    ok = expect(observation.state == UpdateSigningKeyRingCache::State::
                                         CachedButNotAuthoritative &&
                    observation.present && observation.integrityVerified &&
                    !observation.authority.isValid() &&
                    !observation.verificationErrorCode.isEmpty() &&
                    authoritiesAreFalse(observation),
                "expired historical signer restored cache authority") &&
         ok;

    commit = reopened.append(rotationEnvelope, anchor, kExpiredRootNowMs,
                             generationOneCacheIdentity);
    ok = expect(!commit.committed && commit.idempotent &&
                    commit.postCommitVerified &&
                    commit.observation.state ==
                        UpdateSigningKeyRingCache::State::
                            CachedButNotAuthoritative &&
                    commit.observation.cacheIdentity ==
                        generationTwoCacheIdentity &&
                    !commit.observation.authority.isValid() &&
                    authoritiesAreFalse(commit),
                "expired exact rotation retry did not remain cached-only") &&
         ok;
    commit = reopened.append(rotationEnvelope, anchor, kExpiredRootNowMs,
                             generationTwoCacheIdentity);
    ok = expect(!commit.committed && commit.idempotent &&
                    commit.postCommitVerified &&
                    commit.observation.state ==
                        UpdateSigningKeyRingCache::State::
                            CachedButNotAuthoritative &&
                    !commit.observation.authority.isValid() &&
                    authoritiesAreFalse(commit),
                "expired exact retry against the current head restored "
                "authority") &&
         ok;

    QByteArray differentEnvelope = rotationEnvelope;
    if (!differentEnvelope.isEmpty()) {
        differentEnvelope[differentEnvelope.size() / 2] ^= 1;
    }
    commit = reopened.append(differentEnvelope, anchor, kExpiredRootNowMs,
                             generationTwoCacheIdentity);
    ok =
        expect(!commit.committed && !commit.idempotent &&
                   commit.errorCode ==
                       QStringLiteral(
                           "update-signing-key-ring-cache-not-authoritative") &&
                   !commit.observation.authority.isValid() &&
                   authoritiesAreFalse(commit),
               "cached-only state admitted a different rotation envelope") &&
        ok;

    QTemporaryDir expiredBootstrapDirectory;
    ok = expect(expiredBootstrapDirectory.isValid(),
                "expired bootstrap retry directory is unavailable") &&
         ok;
    UpdateSigningKeyRingCache::Store expiredBootstrapStore(
        expiredBootstrapDirectory.path());
    commit = expiredBootstrapStore.bootstrap(bootstrapEnvelope, anchor,
                                             kBootstrapNowMs);
    ok = expect(commit.committed && commit.observation.authority.isValid(),
                "expired bootstrap retry fixture did not initialize") &&
         ok;
    commit = expiredBootstrapStore.bootstrap(bootstrapEnvelope, anchor,
                                             kExpiredRootNowMs);
    ok = expect(!commit.committed && commit.idempotent &&
                    commit.postCommitVerified &&
                    commit.observation.state ==
                        UpdateSigningKeyRingCache::State::
                            CachedButNotAuthoritative &&
                    commit.observation.integrityVerified &&
                    !commit.observation.authority.isValid() &&
                    authoritiesAreFalse(commit),
                "expired exact bootstrap retry restored authority") &&
         ok;

    QByteArray differentBootstrapEnvelope = bootstrapEnvelope;
    if (!differentBootstrapEnvelope.isEmpty()) {
        differentBootstrapEnvelope[differentBootstrapEnvelope.size() / 2] ^= 1;
    }
    commit = expiredBootstrapStore.bootstrap(differentBootstrapEnvelope, anchor,
                                             kExpiredRootNowMs);
    ok = expect(!commit.committed && !commit.idempotent &&
                    !commit.observation.authority.isValid() &&
                    authoritiesAreFalse(commit),
                "cached-only bootstrap accepted a different envelope") &&
         ok;

    const QFileInfoList objectFiles =
        QDir(objectsDirectory(directory.path()))
            .entryInfoList(QDir::Files, QDir::Name);
    ok = expect(objectFiles.size() == 2,
                "cache did not retain both generation objects") &&
         ok;
    const QString latestObjectPath = objectFiles.constLast().absoluteFilePath();
    const QByteArray latestObjectBytes = readFile(latestObjectPath);

    ok = expect(writePrivateFile(headPath, generationOneHead),
                "cache head rollback fixture could not be written") &&
         ok;
    ok = verifyInvalid(
             reopened, anchor,
             "rolled-back head with retained newer object was accepted") &&
         ok;
    ok = expect(writePrivateFile(headPath, generationTwoHead),
                "cache head could not be restored") &&
         ok;

    QByteArray tamperedObject = latestObjectBytes;
    if (!tamperedObject.isEmpty())
        tamperedObject[tamperedObject.size() / 2] ^= 1;
    ok = expect(writePrivateFile(latestObjectPath, tamperedObject),
                "cache object tamper fixture could not be written") &&
         ok;
    ok = verifyInvalid(reopened, anchor,
                       "tampered generation object was accepted") &&
         ok;
    ok = expect(writePrivateFile(latestObjectPath, latestObjectBytes),
                "cache object could not be restored") &&
         ok;

    ok = expect(QFile::remove(latestObjectPath),
                "cache generation object could not be removed") &&
         ok;
    ok = verifyInvalid(reopened, anchor,
                       "missing generation object was accepted") &&
         ok;
    ok = expect(writePrivateFile(latestObjectPath, latestObjectBytes),
                "cache generation object could not be restored") &&
         ok;

    const QByteArray savedHead = readFile(headPath);
    QJsonObject forgedHead = QJsonDocument::fromJson(savedHead).object();
    forgedHead.insert(QStringLiteral("download_authorized"), true);
    ok = expect(writePrivateFile(
                    headPath,
                    QJsonDocument(forgedHead).toJson(QJsonDocument::Compact)),
                "cache forged authority fixture could not be written") &&
         ok;
    ok = verifyInvalid(reopened, anchor,
                       "forged cache authority flag was accepted") &&
         ok;
    ok = expect(writePrivateFile(headPath, savedHead),
                "cache head could not be restored after authority tamper") &&
         ok;

    QJsonObject forgedPreviousHead =
        QJsonDocument::fromJson(savedHead).object();
    forgedPreviousHead.insert(
        QStringLiteral("previous_cache_identity"),
        QStringLiteral(
            "update-signing-key-ring-continuity-cache:sha256:"
            "0000000000000000000000000000000000000000000000000000000000000000"));
    forgedPreviousHead.insert(
        QStringLiteral("cache_identity"),
        continuityHeadIdentity(forgedPreviousHead));
    ok = expect(
             writePrivateFile(
                 headPath,
                 QJsonDocument(forgedPreviousHead)
                     .toJson(QJsonDocument::Compact)),
             "cache previous-head semantic tamper could not be written") &&
         ok;
    ok = verifyInvalid(
             reopened, anchor,
             "self-consistent forged previous cache identity was accepted") &&
         ok;
    ok = expect(
             writePrivateFile(headPath, savedHead),
             "cache head could not be restored after previous-head tamper") &&
         ok;

    QJsonObject acceptedAtHead = QJsonDocument::fromJson(savedHead).object();
    acceptedAtHead.insert(QStringLiteral("accepted_at_ms"), 2500.0);
    ok = expect(writePrivateFile(headPath, QJsonDocument(acceptedAtHead)
                                               .toJson(QJsonDocument::Compact)),
                "cache accepted-at fixture could not be written") &&
         ok;
    ok = verifyInvalid(reopened, anchor,
                       "cache imported a persisted admission time") &&
         ok;
    ok = expect(writePrivateFile(headPath, savedHead),
                "cache head could not be restored after accepted-at test") &&
         ok;

    const QString unknownObject = QDir(objectsDirectory(directory.path()))
                                      .filePath(QStringLiteral("unknown.json"));
    ok = expect(writePrivateFile(unknownObject, QByteArrayLiteral("{}")),
                "unknown cache object fixture could not be written") &&
         ok;
    ok = verifyInvalid(reopened, anchor, "unknown cache object was ignored") &&
         ok;
    ok = expect(QFile::remove(unknownObject),
                "unknown cache object could not be removed") &&
         ok;

#ifndef Q_OS_WIN
    const QFileDevice::Permissions privatePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    ok = expect(
             QFile::setPermissions(latestObjectPath,
                                   privatePermissions | QFileDevice::ReadGroup),
             "cache object permissions could not be widened") &&
         ok;
    ok = verifyInvalid(reopened, anchor,
                       "over-permissive cache object was accepted") &&
         ok;
    ok = expect(QFile::setPermissions(latestObjectPath, privatePermissions),
                "cache object permissions could not be restored") &&
         ok;

    const QString hardLinkPath =
        QDir(directory.path())
            .filePath(QStringLiteral("cache-object-alias.json"));
    ok = expect(::link(QFile::encodeName(latestObjectPath).constData(),
                       QFile::encodeName(hardLinkPath).constData()) == 0,
                "cache hard-link fixture could not be created") &&
         ok;
    ok = verifyInvalid(reopened, anchor,
                       "multiply linked cache object was accepted") &&
         ok;
    ok = expect(QFile::remove(hardLinkPath),
                "cache hard-link fixture could not be removed") &&
         ok;

    const QString savedHeadPath =
        QDir(directory.path())
            .filePath(QStringLiteral("saved-cache-head.json"));
    ok = expect(writePrivateFile(savedHeadPath, savedHead) &&
                    QFile::remove(headPath) &&
                    QFile::link(savedHeadPath, headPath),
                "cache head link fixture could not be created") &&
         ok;
    ok =
        verifyInvalid(reopened, anchor, "linked cache head was accepted") && ok;
    ok = expect(QFile::remove(headPath) &&
                    writePrivateFile(headPath, savedHead) &&
                    QFile::remove(savedHeadPath),
                "cache head link fixture could not be restored") &&
         ok;
#endif

    QLockFile heldLock(
        QDir(directory.path())
            .filePath(QStringLiteral(
                "aegisy-update-signing-key-ring-continuity.lock")));
    ok = expect(heldLock.tryLock(0),
                "cache writer lock fixture could not acquire the lock") &&
         ok;
    observation = reopened.load(anchor, kRotationNowMs);
    ok = expect(observation.state ==
                        UpdateSigningKeyRingCache::State::Unavailable &&
                    observation.errorCode ==
                        QStringLiteral("update-signing-key-ring-cache-busy") &&
                    !observation.authority.isValid() &&
                    authoritiesAreFalse(observation),
                "cache reader bypassed the local writer lock") &&
         ok;
    heldLock.unlock();

    ok = expect(QFile::remove(markerPath),
                "cache marker could not be removed") &&
         ok;
    ok = verifyInvalid(reopened, anchor, "missing cache marker was accepted") &&
         ok;
    ok = expect(writePrivateFile(markerPath, markerBytes),
                "cache marker could not be restored") &&
         ok;
    ok = expect(QFile::remove(headPath), "cache head could not be removed") &&
         ok;
    ok = verifyInvalid(reopened, anchor, "missing cache head was accepted") &&
         ok;
    ok = expect(writePrivateFile(headPath, savedHead),
                "cache head could not be restored") &&
         ok;

    ok = expect(QDir(cacheDirectory(directory.path())).removeRecursively(),
                "complete cache evidence could not be deleted") &&
         ok;
    observation = reopened.load(anchor, kRotationNowMs);
    ok = expect(observation.state == UpdateSigningKeyRingCache::State::Empty &&
                    !observation.present && !observation.integrityVerified &&
                    !observation.authority.isValid() &&
                    !observation.antiDeletionProtected &&
                    !observation.antiRollbackProtected &&
                    authoritiesAreFalse(observation),
                "complete local cache deletion fabricated detection or "
                "authority") &&
         ok;

    return ok ? 0 : 1;
}
