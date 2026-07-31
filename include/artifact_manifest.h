#pragma once

#include <QJsonObject>
#include <QString>

namespace ArtifactManifest {

struct VerificationResult
{
    bool ok = false;
    QString reason;
    QString artifactId;
    QString version;
    QString manifestSha256;
    QString runtimeId;
    QString runtimeVersion;
    QString runtimePath;
    QString runtimeFileIdentity;
    quint64 runtimeSizeBytes = 0;
    QString runtimeSha256;
    QString adapterId;
    QString adapterVersion;
    QString adapterPath;
    QString adapterFileIdentity;
    quint64 adapterSizeBytes = 0;
    QString adapterSha256;
    QString manifestPath;
    QString manifestFileIdentity;
    quint64 manifestSizeBytes = 0;
};

// Verifies a bounded, local artifact manifest. The manifest is data-only: it
// never downloads, executes, or changes the referenced artifacts.
VerificationResult verifyFile(const QString &manifestPath,
                              const QString &runtimePath = QString());

VerificationResult verifyObject(const QJsonObject &manifest,
                                const QString &baseDirectory,
                                const QString &runtimePath = QString());

} // namespace ArtifactManifest
