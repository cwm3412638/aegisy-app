#include "artifact_manifest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QRegularExpression>

namespace {

constexpr qint64 kMaximumArtifactBytes = 512LL * 1024 * 1024;

ArtifactManifest::VerificationResult fail(const QString &reason,
                                           const QString &artifactId = QString())
{
    return {false, reason, artifactId, {}};
}

bool validText(const QString &value, int maximumBytes)
{
    return !value.isEmpty() && value.toUtf8().size() <= maximumBytes
        && !value.contains(QChar::Null);
}

bool validSha256(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

bool safeRelativePath(const QString &path)
{
    if (!validText(path, 1024) || QFileInfo(path).isAbsolute()) return false;
    const QString normalized = QDir::cleanPath(path);
    if (normalized == QStringLiteral(".") || normalized.startsWith(QStringLiteral("../"))
        || normalized == QStringLiteral("..") || normalized.contains(QStringLiteral("/../"))) {
        return false;
    }
    return true;
}

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    const QStringList keys = object.keys();
    if (keys.size() != expected.size()) return false;
    for (const QString &key : keys) {
        if (!expected.contains(key)) return false;
    }
    return true;
}

ArtifactManifest::VerificationResult verifyArtifact(const QJsonObject &artifact,
                                                     const QString &baseDirectory,
                                                     const QString &expectedPath)
{
    const QString id = artifact.value(QStringLiteral("id")).toString();
    const QString version = artifact.value(QStringLiteral("version")).toString();
    const QString path = artifact.value(QStringLiteral("path")).toString();
    const QString expectedHash = artifact.value(QStringLiteral("sha256")).toString();
    if (!hasExactKeys(artifact, {
            QStringLiteral("id"), QStringLiteral("version"), QStringLiteral("path"),
            QStringLiteral("sha256")
        })
        || !validText(id, 128) || !validText(version, 128) || !safeRelativePath(path)
        || !validSha256(expectedHash)) {
        return fail(QStringLiteral("invalid-artifact-entry"), id);
    }

    const QFileInfo info(QDir(baseDirectory).filePath(path));
    if (!info.isFile() || !info.isReadable()) return fail(QStringLiteral("artifact-missing"), id);
    const QString baseCanonical = QFileInfo(baseDirectory).canonicalFilePath();
    const QString artifactCanonical = info.canonicalFilePath();
    if (baseCanonical.isEmpty() || artifactCanonical.isEmpty()
        || artifactCanonical == baseCanonical
        || !artifactCanonical.startsWith(baseCanonical + QDir::separator())) {
        return fail(QStringLiteral("artifact-path-escape"), id);
    }
    if (info.size() < 0 || info.size() > kMaximumArtifactBytes) {
        return fail(QStringLiteral("artifact-size-limit"), id);
    }
    if (!expectedPath.isEmpty()) {
        const QString expectedCanonical = QFileInfo(expectedPath).canonicalFilePath();
        if (expectedCanonical.isEmpty() || expectedCanonical != artifactCanonical) {
            return fail(QStringLiteral("artifact-path-mismatch"), id);
        }
    }

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("artifact-unreadable"), id);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && !file.atEnd()) return fail(QStringLiteral("artifact-read-failed"), id);
        hash.addData(chunk);
    }
    if (QString::fromLatin1(hash.result().toHex()) != expectedHash) {
        return fail(QStringLiteral("artifact-hash-mismatch"), id);
    }
    return {true, {}, id, version};
}

} // namespace

namespace ArtifactManifest {

VerificationResult verifyObject(const QJsonObject &manifest,
                                const QString &baseDirectory,
                                const QString &runtimePath)
{
    if (!hasExactKeys(manifest, {QStringLiteral("schema_version"), QStringLiteral("runtime"),
                                 QStringLiteral("adapter")})) {
        return fail(QStringLiteral("manifest-fields-invalid"));
    }
    if (manifest.value(QStringLiteral("schema_version")).toString()
            != QStringLiteral("aegisy-artifact-manifest/0.1")) {
        return fail(QStringLiteral("unsupported-schema"));
    }
    const QJsonValue runtime = manifest.value(QStringLiteral("runtime"));
    const QJsonValue adapter = manifest.value(QStringLiteral("adapter"));
    if (!runtime.isObject() || !adapter.isObject()) return fail(QStringLiteral("missing-artifact"));

    if (runtime.toObject().value(QStringLiteral("id")).toString()
            != QStringLiteral("aegisy-agentd")
        || adapter.toObject().value(QStringLiteral("id")).toString()
            != QStringLiteral("codex-app-server")) {
        return fail(QStringLiteral("artifact-identity-invalid"));
    }
    const auto runtimeResult = verifyArtifact(runtime.toObject(), baseDirectory, runtimePath);
    if (!runtimeResult.ok) return runtimeResult;
    const auto adapterResult = verifyArtifact(adapter.toObject(), baseDirectory, {});
    if (!adapterResult.ok) return adapterResult;
    return {true, {}, runtimeResult.artifactId,
            runtimeResult.version + QStringLiteral("/") + adapterResult.version};
}

VerificationResult verifyFile(const QString &manifestPath, const QString &runtimePath)
{
    const QFileInfo manifestInfo(manifestPath);
    if (manifestInfo.isSymLink() || manifestInfo.canonicalFilePath().isEmpty()) {
        return fail(QStringLiteral("manifest-path-invalid"));
    }
    QFile file(manifestPath);
    if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("manifest-unreadable"));
    if (file.size() < 0 || file.size() > 64 * 1024) return fail(QStringLiteral("manifest-size-limit"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("manifest-invalid-json"));
    }
    return verifyObject(document.object(), QFileInfo(manifestPath).absolutePath(), runtimePath);
}

} // namespace ArtifactManifest
