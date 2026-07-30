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
Decision verifyCandidate(const QByteArray &envelopeJson,
                         const QByteArray &publicKeyBase64,
                         qint64 nowMs,
                         const InstalledArtifactSet &installed,
                         const QString &selectedChannel,
                         quint64 acceptedReleaseSequenceHighWater);

} // namespace UpdateArtifactSet

#endif // UPDATE_ARTIFACT_SET_H
