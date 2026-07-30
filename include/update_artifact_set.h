#ifndef UPDATE_ARTIFACT_SET_H
#define UPDATE_ARTIFACT_SET_H

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

class InstalledArtifactSetAuthority
{
public:
    bool isValid() const { return m_valid; }
    QString authorityIdentity() const { return m_authorityIdentity; }

private:
    friend struct InstalledAuthorityResult;
    friend InstalledAuthorityResult verifyInstalledAuthority(
        const QString &, const QString &, const QString &, const QByteArray &,
        qint64, const QString &, const QString &, const QString &,
        const QString &);
    friend Decision verifyCandidate(
        const QByteArray &, const QByteArray &, qint64,
        const InstalledArtifactSetAuthority &, const QString &, quint64);

    bool m_valid = false;
    quint64 m_releaseSequence = 0;
    QString m_channel;
    QString m_applicationVersion;
    QString m_platform;
    QString m_architecture;
    QString m_manifestSha256;
    QString m_runtimeId;
    QString m_runtimeVersion;
    QString m_adapterId;
    QString m_adapterVersion;
    QString m_receiptIdentity;
    QString m_installedArtifactSetIdentity;
    QString m_authorityIdentity;
    QString m_receiptPath;
    QString m_manifestPath;
    QString m_runtimePath;
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
    quint64 targetReleaseSequence = 0;
    QString targetChannel;
    QString targetApplicationVersion;
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

QByteArray signaturePayload(const QJsonObject &envelope,
                            QString *errorCode = nullptr);
InstalledAuthorityResult verifyInstalledAuthority(
    const QString &receiptPath,
    const QString &manifestPath,
    const QString &runtimePath,
    const QByteArray &publicKeyBase64,
    qint64 nowMs,
    const QString &expectedApplicationVersion,
    const QString &expectedChannel,
    const QString &expectedPlatform,
    const QString &expectedArchitecture);
Decision verifyCandidate(const QByteArray &envelopeJson,
                         const QByteArray &publicKeyBase64,
                         qint64 nowMs,
                         const InstalledArtifactSetAuthority &installedAuthority,
                         const QString &selectedChannel,
                         quint64 acceptedReleaseSequenceHighWater);

#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
namespace Testing {

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

} // namespace Testing

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
