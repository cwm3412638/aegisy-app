#include "workbench_emergency_policy.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>

#include <openssl/evp.h>

#include <iostream>

namespace {

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << message << '\n';
    return condition;
}

class SigningKey
{
public:
    SigningKey()
    {
        const QByteArray seed(32, '\x2a');
        key = EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr,
            reinterpret_cast<const unsigned char *>(seed.constData()), seed.size());
        unsigned char bytes[32];
        size_t size = sizeof(bytes);
        if (key && EVP_PKEY_get_raw_public_key(key, bytes, &size) == 1 && size == sizeof(bytes)) {
            publicKey = QByteArray(reinterpret_cast<const char *>(bytes), int(size)).toBase64();
        }
    }

    ~SigningKey() { EVP_PKEY_free(key); }

    QJsonObject envelope(quint64 sequence, qint64 issuedAtMs, qint64 expiresAtMs,
                         bool disabled, const QString &reasonCode) const
    {
        QJsonObject value{
            {QStringLiteral("schema_version"),
             QStringLiteral("aegisy-workbench-emergency-policy/0.1")},
            {QStringLiteral("sequence"), double(sequence)},
            {QStringLiteral("issued_at_ms"), double(issuedAtMs)},
            {QStringLiteral("expires_at_ms"), double(expiresAtMs)},
            {QStringLiteral("workbench_disabled"), disabled},
            {QStringLiteral("reason_code"), reasonCode},
            {QStringLiteral("signature"), QString()},
        };
        const QByteArray payload = WorkbenchEmergencyPolicy::signaturePayload(value);
        EVP_MD_CTX *context = EVP_MD_CTX_new();
        size_t signatureSize = 64;
        QByteArray signature(int(signatureSize), Qt::Uninitialized);
        const bool signedValue = context
            && EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1
            && EVP_DigestSign(
                   context,
                   reinterpret_cast<unsigned char *>(signature.data()), &signatureSize,
                   reinterpret_cast<const unsigned char *>(payload.constData()),
                   size_t(payload.size())) == 1;
        EVP_MD_CTX_free(context);
        if (!signedValue) return {};
        signature.resize(int(signatureSize));
        value.insert(QStringLiteral("signature"), QString::fromLatin1(signature.toBase64()));
        return value;
    }

    EVP_PKEY *key = nullptr;
    QByteArray publicKey;
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const qint64 now = 1'900'000'000'000LL;
    SigningKey signingKey;
    if (!require(signingKey.key && signingKey.publicKey.size() == 44,
                 "test signing key is unavailable")) return 1;

    QJsonObject disabled = signingKey.envelope(
        10, now - 1000, now + 60 * 60 * 1000, true, QStringLiteral("incident-response"));
    auto decision = WorkbenchEmergencyPolicy::verify(
        disabled, signingKey.publicKey, now);
    if (!require(decision.state == WorkbenchEmergencyPolicy::State::Disabled
                     && decision.blocksNewWork && decision.sequence == 10
                     && decision.reasonCode == QStringLiteral("incident-response")
                     && decision.policyIdentity.startsWith(
                         QStringLiteral("workbench-emergency-policy:sha256:")),
                 "valid signed disable was rejected")) return 1;

    QJsonObject tampered = disabled;
    tampered.insert(QStringLiteral("workbench_disabled"), false);
    decision = WorkbenchEmergencyPolicy::verify(tampered, signingKey.publicKey, now);
    if (!require(decision.state == WorkbenchEmergencyPolicy::State::Invalid
                     && decision.blocksNewWork,
                 "tampered policy was accepted")) return 1;

    QJsonObject contentBearing = disabled;
    contentBearing.insert(QStringLiteral("message"), QStringLiteral("session-content"));
    decision = WorkbenchEmergencyPolicy::verify(contentBearing, signingKey.publicKey, now);
    if (!require(decision.state == WorkbenchEmergencyPolicy::State::Invalid,
                 "unknown content field was accepted")) return 1;

    QTemporaryDir directory;
    if (!require(directory.isValid(), "temporary policy store is unavailable")) return 1;
    QSettings settings(directory.filePath(QStringLiteral("policy.ini")), QSettings::IniFormat);
    auto installed = WorkbenchEmergencyPolicy::install(
        &settings, disabled, signingKey.publicKey, now);
    if (!require(installed.accepted && installed.decision.blocksNewWork,
                 "signed disable was not persisted")) return 1;
    decision = WorkbenchEmergencyPolicy::load(&settings, signingKey.publicKey, now);
    if (!require(decision.state == WorkbenchEmergencyPolicy::State::Disabled,
                 "persisted disable did not survive reload")) return 1;

    const QJsonObject rollback = signingKey.envelope(
        9, now - 1000, now + 60 * 60 * 1000, false, QStringLiteral("service-restored"));
    installed = WorkbenchEmergencyPolicy::install(
        &settings, rollback, signingKey.publicKey, now);
    if (!require(!installed.accepted
                     && installed.errorCode == QStringLiteral("policy-sequence-rollback")
                     && installed.decision.blocksNewWork,
                 "lower-sequence allow cleared the disable")) return 1;

    const QJsonObject enabled = signingKey.envelope(
        11, now - 1000, now + 60 * 60 * 1000, false, QStringLiteral("service-restored"));
    installed = WorkbenchEmergencyPolicy::install(
        &settings, enabled, signingKey.publicKey, now);
    if (!require(installed.accepted
                     && installed.decision.state == WorkbenchEmergencyPolicy::State::Enabled
                     && !installed.decision.blocksNewWork,
                 "higher-sequence signed allow did not clear the disable")) return 1;

    settings.setValue(QStringLiteral("workbench/emergencyPolicy/envelope"),
                      QJsonDocument(disabled).toJson(QJsonDocument::Compact));
    settings.sync();
    decision = WorkbenchEmergencyPolicy::load(&settings, signingKey.publicKey, now);
    if (!require(decision.state == WorkbenchEmergencyPolicy::State::Invalid
                     && decision.blocksNewWork
                     && decision.errorCode
                         == QStringLiteral("policy-cache-marker-mismatch"),
                 "cache rollback after restart did not fail closed")) return 1;
    installed = WorkbenchEmergencyPolicy::install(
        &settings, enabled, signingKey.publicKey, now);
    if (!require(installed.accepted
                     && installed.decision.state
                         == WorkbenchEmergencyPolicy::State::Enabled,
                 "exact high-water policy did not repair the rolled-back cache")) return 1;

    const QJsonObject conflicting = signingKey.envelope(
        11, now - 1000, now + 60 * 60 * 1000, true,
        QStringLiteral("incident-response"));
    installed = WorkbenchEmergencyPolicy::install(
        &settings, conflicting, signingKey.publicKey, now);
    if (!require(!installed.accepted
                     && installed.errorCode
                         == QStringLiteral("policy-sequence-conflict")
                     && !installed.decision.blocksNewWork,
                 "same-sequence conflicting policy was accepted")) return 1;

    decision = WorkbenchEmergencyPolicy::load(
        &settings, signingKey.publicKey, now + 2 * 60 * 60 * 1000);
    if (!require(decision.state == WorkbenchEmergencyPolicy::State::Stale
                     && decision.blocksNewWork,
                 "expired cached policy did not fail closed")) return 1;

    settings.remove(QStringLiteral("workbench/emergencyPolicy/envelope"));
    settings.sync();
    decision = WorkbenchEmergencyPolicy::load(&settings, signingKey.publicKey, now);
    if (!require(decision.state == WorkbenchEmergencyPolicy::State::Invalid
                     && decision.blocksNewWork
                     && decision.errorCode == QStringLiteral("policy-cache-missing"),
                 "missing previously-installed cache did not fail closed")) return 1;

    QSettings firstOpen(directory.filePath(QStringLiteral("first-open.ini")),
                        QSettings::IniFormat);
    decision = WorkbenchEmergencyPolicy::load(&firstOpen, signingKey.publicKey, now);
    if (!require(decision.state == WorkbenchEmergencyPolicy::State::NoPolicy
                     && !decision.blocksNewWork,
                 "first open without a production source was not distinguished")) return 1;

    firstOpen.setValue(QStringLiteral("workbench/emergencyPolicy/envelope"),
                       QJsonDocument(disabled).toJson(QJsonDocument::Compact));
    firstOpen.sync();
    decision = WorkbenchEmergencyPolicy::load(&firstOpen, signingKey.publicKey, now);
    if (!require(decision.state == WorkbenchEmergencyPolicy::State::Invalid
                     && decision.blocksNewWork
                     && decision.errorCode
                         == QStringLiteral("policy-cache-marker-missing"),
                 "cache without its high-water marker did not fail closed")) return 1;

    return 0;
}
