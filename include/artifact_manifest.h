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
};

// Verifies a bounded, local artifact manifest. The manifest is data-only: it
// never downloads, executes, or changes the referenced artifacts.
VerificationResult verifyFile(const QString &manifestPath,
                              const QString &runtimePath = QString());

VerificationResult verifyObject(const QJsonObject &manifest,
                                const QString &baseDirectory,
                                const QString &runtimePath = QString());

} // namespace ArtifactManifest
