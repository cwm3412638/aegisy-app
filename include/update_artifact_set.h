#ifndef UPDATE_ARTIFACT_SET_H
#define UPDATE_ARTIFACT_SET_H

#include "update_signing_key_ring.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace UpdateArtifactSet {

enum class State {
    Invalid,
    Incompatible,
    Compatible,
};

struct InstalledAuthorityResult;
struct Decision;

#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
namespace Testing {
InstalledAuthorityResult verifyInstalledAuthorityAtRoot(
    const QString &, const QByteArray &, qint64, const QString &,
    const QString &, const QString &, const QString &);
InstalledAuthorityResult verifyInstalledAuthorityAtRoot(
    const QString &, const UpdateSigningKeyRing::Authority &, qint64,
    const QString &, const QString &, const QString &, const QString &);
}
#endif

class InstalledArtifactSetAuthority
{
public:
    bool isValid() const { return m_valid; }
    QString authorityIdentity() const { return m_authorityIdentity; }

private:
    class Verifier;

    friend struct InstalledAuthorityResult;
    friend InstalledAuthorityResult verifyCurrentInstallationAuthority(
        const UpdateSigningKeyRing::Authority &, qint64, const QString &);
#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
    friend InstalledAuthorityResult verifyCurrentInstallationAuthority(
        const QByteArray &, qint64, const QString &);
    friend InstalledAuthorityResult Testing::verifyInstalledAuthorityAtRoot(
        const QString &, const QByteArray &, qint64, const QString &,
        const QString &, const QString &, const QString &);
    friend InstalledAuthorityResult Testing::verifyInstalledAuthorityAtRoot(
        const QString &, const UpdateSigningKeyRing::Authority &, qint64,
        const QString &, const QString &, const QString &, const QString &);
#endif
    friend Decision verifyCandidate(
        const QByteArray &, const UpdateSigningKeyRing::Authority &, qint64,
        const InstalledArtifactSetAuthority &, const QString &, quint64);
#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
    friend Decision verifyCandidate(
        const QByteArray &, const QByteArray &, qint64,
        const InstalledArtifactSetAuthority &, const QString &, quint64);
#endif

    bool m_valid = false;
    QString m_schemaVersion;
    quint64 m_releaseSequence = 0;
    QString m_channel;
    QString m_applicationVersion;
    QString m_platform;
    QString m_architecture;
    quint64 m_applicationSizeBytes = 0;
    QString m_applicationSha256;
    QString m_manifestSha256;
    QString m_runtimeId;
    QString m_runtimeVersion;
    QString m_adapterId;
    QString m_adapterVersion;
    QString m_receiptIdentity;
    QString m_installedArtifactSetIdentity;
    QString m_installationLayoutIdentity;
    QString m_authorityIdentity;
    QString m_trustAnchorIdentity;
    QString m_ringIdentity;
    quint64 m_ringGeneration = 0;
    QString m_ringAuthorityIdentity;
    QString m_receiptSignerKeyId;
    QString m_receiptSignerKeyIdentity;
    QString m_applicationPath;
    QString m_installationRoot;
    QString m_receiptPath;
    QString m_manifestPath;
    QString m_runtimePath;
#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
    bool m_testOnlyLayout = false;
#endif
};

struct InstalledAuthorityResult
{
    bool ok = false;
    QString errorCode;
    InstalledArtifactSetAuthority authority;
};

struct Decision
{
    State state = State::Invalid;
    // Compatibility never grants updater download or install authority.
    bool candidateCompatible = false;
    bool downloadAuthorized = false;
    bool installAuthorized = false;
    QString errorCode;
    QString artifactSetIdentity;
    QString installedArtifactSetIdentity;
    QString installedAuthorityIdentity;
    QString compatibilityEvaluationIdentity;
    QString evaluatedSelectedChannel;
    quint64 evaluatedAcceptedReleaseSequenceHighWater = 0;
    quint64 evaluatedAtMs = 0;
    QString payloadIdentity;
    QString signingKeyId;
    QString signerKeyIdentity;
    QString signingTrustAnchorIdentity;
    QString signingRingIdentity;
    quint64 signingRingGeneration = 0;
    QString signingRingAuthorityIdentity;
    quint64 signedAtMs = 0;
    quint64 expiresAtMs = 0;
    quint64 targetReleaseSequence = 0;
    QString targetChannel;
    QString targetApplicationVersion;
    quint64 targetApplicationSizeBytes = 0;
    QString targetApplicationSha256;
    QString platform;
    QString architecture;
    QString installerUrl;
    QString installerFileName;
    quint64 installerSizeBytes = 0;
    QString installerSha256;
    QString installerSparkleSignature;
    QString targetManifestSha256;
    QString targetRuntimeId;
    QString targetRuntimeVersion;
    QString targetAdapterId;
    QString targetAdapterVersion;
    quint64 matchedSourceReleaseSequence = 0;
};

InstalledAuthorityResult verifyCurrentInstallationAuthority(
    const UpdateSigningKeyRing::Authority &signingAuthority,
    qint64 nowMs,
    const QString &expectedChannel);
Decision verifyCandidate(const QByteArray &envelopeJson,
                         const UpdateSigningKeyRing::Authority &signingAuthority,
                         qint64 nowMs,
                         const InstalledArtifactSetAuthority &installedAuthority,
                         const QString &selectedChannel,
                         quint64 acceptedReleaseSequenceHighWater);

#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
namespace Testing {

QByteArray signaturePayload(const QJsonObject &envelope,
                            QString *errorCode = nullptr);
QString payloadIdentity(const QJsonObject &envelope,
                        QString *errorCode = nullptr);

struct InstalledArtifactSet
{
    quint64 releaseSequence = 0;
    QString channel;
    QString applicationVersion;
    QString platform;
    QString architecture;
    QString manifestSha256;
    QString runtimeId;
    QString runtimeVersion;
    QString adapterId;
    QString adapterVersion;
};

Decision verifyCandidateWithUntrustedInputs(
    const QByteArray &envelopeJson,
    const QByteArray &publicKeyBase64,
    qint64 nowMs,
    const InstalledArtifactSet &installed,
    const QString &selectedChannel,
    quint64 acceptedReleaseSequenceHighWater);

InstalledAuthorityResult verifyInstalledAuthorityAtRoot(
    const QString &artifactRoot,
    const QByteArray &publicKeyBase64,
    qint64 nowMs,
    const QString &expectedApplicationVersion,
    const QString &expectedChannel,
    const QString &expectedPlatform,
    const QString &expectedArchitecture);

InstalledAuthorityResult verifyInstalledAuthorityAtRoot(
    const QString &artifactRoot,
    const UpdateSigningKeyRing::Authority &signingAuthority,
    qint64 nowMs,
    const QString &expectedApplicationVersion,
    const QString &expectedChannel,
    const QString &expectedPlatform,
    const QString &expectedArchitecture);

} // namespace Testing

inline QByteArray signaturePayload(const QJsonObject &envelope,
                                   QString *errorCode = nullptr)
{
    return Testing::signaturePayload(envelope, errorCode);
}

inline QString payloadIdentity(const QJsonObject &envelope,
                               QString *errorCode = nullptr)
{
    return Testing::payloadIdentity(envelope, errorCode);
}

InstalledAuthorityResult verifyCurrentInstallationAuthority(
    const QByteArray &publicKeyBase64,
    qint64 nowMs,
    const QString &expectedChannel);

Decision verifyCandidate(const QByteArray &envelopeJson,
                         const QByteArray &publicKeyBase64,
                         qint64 nowMs,
                         const InstalledArtifactSetAuthority &installedAuthority,
                         const QString &selectedChannel,
                         quint64 acceptedReleaseSequenceHighWater);

using InstalledArtifactSet = Testing::InstalledArtifactSet;

inline Decision verifyCandidate(
    const QByteArray &envelopeJson,
    const QByteArray &publicKeyBase64,
    qint64 nowMs,
    const InstalledArtifactSet &installed,
    const QString &selectedChannel,
    quint64 acceptedReleaseSequenceHighWater)
{
    return Testing::verifyCandidateWithUntrustedInputs(
        envelopeJson, publicKeyBase64, nowMs, installed, selectedChannel,
        acceptedReleaseSequenceHighWater);
}
#endif

} // namespace UpdateArtifactSet

#endif // UPDATE_ARTIFACT_SET_H
