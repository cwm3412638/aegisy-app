#include "artifact_manifest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

constexpr qint64 kMaximumArtifactBytes = 512LL * 1024 * 1024;
constexpr qint64 kMaximumManifestBytes = 64LL * 1024;

enum class ArtifactFileStatus {
    Valid,
    Invalid,
    LinkLike,
    MultipleLinks,
};

ArtifactManifest::VerificationResult fail(const QString &reason,
                                           const QString &artifactId = QString())
{
    ArtifactManifest::VerificationResult result;
    result.reason = reason;
    result.artifactId = artifactId;
    return result;
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
    if (!validText(path, 1024) || QFileInfo(path).isAbsolute()
        || path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\'))
        || path.contains(QLatin1Char(':'))) {
        return false;
    }
    const QStringList components = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString &component : components) {
        if (component.isEmpty() || component == QStringLiteral(".")
            || component == QStringLiteral("..")) {
            return false;
        }
        for (const QChar character : component) {
            if (character.isNull() || character.category() == QChar::Other_Control) {
                return false;
            }
        }
    }
    return true;
}

bool pathIsLinkLike(const QString &path)
{
    const QFileInfo info(path);
    if (info.isSymLink()) return true;
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(info.absoluteFilePath());
    const DWORD attributes = GetFileAttributesW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()));
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

bool hasLinkLikeComponent(const QString &canonicalBase,
                          const QString &relativePath)
{
    QString current = canonicalBase;
    const QStringList components = relativePath.split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        current = QDir(current).filePath(component);
        if (pathIsLinkLike(current)) return true;
    }
    return false;
}

ArtifactFileStatus inspectArtifactFile(const QString &path)
{
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return ArtifactFileStatus::Invalid;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool inspected = GetFileInformationByHandle(handle, &information) != 0;
    CloseHandle(handle);
    if (!inspected || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return ArtifactFileStatus::Invalid;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return ArtifactFileStatus::LinkLike;
    }
    if (information.nNumberOfLinks != 1) return ArtifactFileStatus::MultipleLinks;
    return ArtifactFileStatus::Valid;
#else
    const QByteArray encodedPath = QFile::encodeName(path);
    struct stat information {};
    if (::lstat(encodedPath.constData(), &information) != 0
        || !S_ISREG(information.st_mode)) {
        return S_ISLNK(information.st_mode)
            ? ArtifactFileStatus::LinkLike : ArtifactFileStatus::Invalid;
    }
    if (information.st_nlink != 1) return ArtifactFileStatus::MultipleLinks;
    return ArtifactFileStatus::Valid;
#endif
}

bool validBundledAdapterPath(const QString &path)
{
#ifdef Q_OS_WIN
    // std::process::Command appends .exe on Windows. Requiring the manifest to
    // name that exact image prevents an extensionless file from being verified
    // while a same-directory .exe shadow is executed.
    return path.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive);
#else
    Q_UNUSED(path);
    return true;
#endif
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
                                                     const QString &expectedPath,
                                                     bool bundledAdapter)
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
        || (bundledAdapter && !validBundledAdapterPath(path))
        || !validSha256(expectedHash)) {
        return fail(QStringLiteral("invalid-artifact-entry"), id);
    }

    const QString baseCanonical = QFileInfo(baseDirectory).canonicalFilePath();
    if (baseCanonical.isEmpty()) return fail(QStringLiteral("artifact-path-escape"), id);
    const QFileInfo info(QDir(baseCanonical).filePath(path));
    if (!info.isFile() || !info.isReadable()) return fail(QStringLiteral("artifact-missing"), id);
    if (hasLinkLikeComponent(baseCanonical, path)) {
        return fail(QStringLiteral("artifact-path-invalid"), id);
    }
    const ArtifactFileStatus fileStatus = inspectArtifactFile(info.absoluteFilePath());
    if (fileStatus == ArtifactFileStatus::LinkLike
        || fileStatus == ArtifactFileStatus::Invalid) {
        return fail(QStringLiteral("artifact-path-invalid"), id);
    }
    if (fileStatus == ArtifactFileStatus::MultipleLinks) {
        return fail(QStringLiteral("artifact-hard-link"), id);
    }
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

    QFile file(artifactCanonical);
    if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("artifact-unreadable"), id);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 totalBytes = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && !file.atEnd()) return fail(QStringLiteral("artifact-read-failed"), id);
        if (chunk.size() > kMaximumArtifactBytes - totalBytes) {
            return fail(QStringLiteral("artifact-size-limit"), id);
        }
        totalBytes += chunk.size();
        hash.addData(chunk);
    }
    if (file.error() != QFile::NoError || totalBytes != info.size()) {
        return fail(QStringLiteral("artifact-read-failed"), id);
    }
    if (QString::fromLatin1(hash.result().toHex()) != expectedHash) {
        return fail(QStringLiteral("artifact-hash-mismatch"), id);
    }
    ArtifactManifest::VerificationResult result;
    result.ok = true;
    result.artifactId = id;
    result.version = version;
    return result;
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
    const auto runtimeResult = verifyArtifact(
        runtime.toObject(), baseDirectory, runtimePath, false);
    if (!runtimeResult.ok) return runtimeResult;
    const auto adapterResult = verifyArtifact(adapter.toObject(), baseDirectory, {}, true);
    if (!adapterResult.ok) return adapterResult;
    VerificationResult result;
    result.ok = true;
    result.artifactId = runtimeResult.artifactId;
    result.version = runtimeResult.version + QStringLiteral("/")
        + adapterResult.version;
    result.runtimeId = runtime.toObject().value(QStringLiteral("id")).toString();
    result.runtimeVersion = runtimeResult.version;
    result.adapterId = adapter.toObject().value(QStringLiteral("id")).toString();
    result.adapterVersion = adapterResult.version;
    return result;
}

VerificationResult verifyFile(const QString &manifestPath, const QString &runtimePath)
{
    const QFileInfo manifestInfo(manifestPath);
    const QString manifestCanonical = manifestInfo.canonicalFilePath();
    if (!manifestInfo.isFile() || !manifestInfo.isReadable()
        || manifestInfo.isSymLink() || manifestCanonical.isEmpty()) {
        return fail(QStringLiteral("manifest-path-invalid"));
    }
    QFile file(manifestCanonical);
    if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("manifest-unreadable"));
    const qint64 manifestSize = file.size();
    if (manifestSize < 0 || manifestSize > kMaximumManifestBytes) {
        return fail(QStringLiteral("manifest-size-limit"));
    }
    const QByteArray manifestBytes = file.read(kMaximumManifestBytes + 1);
    if (manifestBytes.size() != manifestSize || file.error() != QFile::NoError
        || !file.atEnd()) {
        return fail(QStringLiteral("manifest-read-failed"));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return fail(QStringLiteral("manifest-invalid-json"));
    }
    VerificationResult result = verifyObject(
        document.object(), QFileInfo(manifestCanonical).absolutePath(), runtimePath);
    if (result.ok) {
        result.manifestSha256 = QString::fromLatin1(QCryptographicHash::hash(
            manifestBytes, QCryptographicHash::Sha256).toHex());
    }
    return result;
}

} // namespace ArtifactManifest
