#include "update_artifact_set.h"

#include <QCoreApplication>
#include <QFile>
#include <QStringList>

#include <cstdio>

namespace {

constexpr qint64 kEvaluationNowMs = 1700000000000LL;
constexpr qint64 kMaximumRingBytes = 128 * 1024;
constexpr qint64 kMaximumArtifactSetBytes = 256 * 1024;

QByteArray readBounded(const QString &path, qint64 maximumBytes)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0
        || file.size() > maximumBytes) {
        return {};
    }
    const QByteArray bytes = file.read(maximumBytes + 1);
    if (bytes.size() != file.size() || !file.atEnd()) return {};
    return bytes;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() != 6
        || arguments.at(1)
            != QStringLiteral("--verify-current-installation-v2")) {
        return 2;
    }
    QCoreApplication::setApplicationVersion(QStringLiteral("2.5.2"));

    const QByteArray bootstrapRing = readBounded(
        arguments.at(2), kMaximumRingBytes);
    const QByteArray rotationRing = readBounded(
        arguments.at(3), kMaximumRingBytes);
    const QByteArray candidate = readBounded(
        arguments.at(4), kMaximumArtifactSetBytes);
    const QByteArray legacyCandidate = readBounded(
        arguments.at(5), kMaximumArtifactSetBytes);
    if (bootstrapRing.isEmpty() || rotationRing.isEmpty()
        || candidate.isEmpty() || legacyCandidate.isEmpty()) {
        return 1;
    }

    const UpdateSigningKeyRing::TrustAnchorAuthority trustAnchor =
        UpdateSigningKeyRing::embeddedTrustAnchor();
    const UpdateSigningKeyRing::AuthorityResult bootstrapAuthority =
        UpdateSigningKeyRing::verifyBootstrap(
            bootstrapRing, trustAnchor, kEvaluationNowMs);
    if (!trustAnchor.isValid() || !bootstrapAuthority.ok
        || !bootstrapAuthority.authority.isValid()) {
        std::fprintf(stderr, "production bootstrap authority failed: %s\n",
                     bootstrapAuthority.errorCode.toUtf8().constData());
        return 1;
    }
    const UpdateSigningKeyRing::AuthorityResult signingAuthority =
        UpdateSigningKeyRing::verifyRotation(
            rotationRing, bootstrapAuthority.authority, kEvaluationNowMs);
    if (!signingAuthority.ok || signingAuthority.idempotent
        || !signingAuthority.authority.isValid()) {
        std::fprintf(stderr, "production rotation authority failed: %s\n",
                     signingAuthority.errorCode.toUtf8().constData());
        return 1;
    }

    const UpdateArtifactSet::InstalledAuthorityResult installed =
        UpdateArtifactSet::verifyCurrentInstallationAuthority(
            signingAuthority.authority, kEvaluationNowMs,
            QStringLiteral("stable"));
    if (!installed.ok || !installed.authority.isValid()) {
        std::fprintf(stderr, "production installation authority failed: %s\n",
                     installed.errorCode.toUtf8().constData());
        return 1;
    }

    const UpdateArtifactSet::Decision decision =
        UpdateArtifactSet::verifyCandidate(
            candidate, signingAuthority.authority, kEvaluationNowMs,
            installed.authority, QStringLiteral("stable"), 41);
    const UpdateArtifactSet::Decision legacyDecision =
        UpdateArtifactSet::verifyCandidate(
            legacyCandidate, signingAuthority.authority, kEvaluationNowMs,
            installed.authority, QStringLiteral("stable"), 41);
    return decision.state == UpdateArtifactSet::State::Compatible
            && decision.candidateCompatible
            && !decision.downloadAuthorized
            && !decision.installAuthorized
            && decision.signingKeyId == QStringLiteral("artifact-fixture-next")
            && decision.signingRingGeneration == 2
            && decision.signingRingAuthorityIdentity
                == signingAuthority.authority.authorityIdentity()
            && decision.installedAuthorityIdentity
                == installed.authority.authorityIdentity()
            && legacyDecision.state == UpdateArtifactSet::State::Invalid
            && !legacyDecision.candidateCompatible
            && !legacyDecision.downloadAuthorized
            && !legacyDecision.installAuthorized
            && legacyDecision.errorCode
                == QStringLiteral("artifact-set-fields-invalid")
        ? 0 : 1;
}
