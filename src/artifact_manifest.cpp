#include "artifact_manifest.h"

#include "aap_transport_runtime.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
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

struct ArtifactFileMetadata
{
    QString identity;
    quint64 sizeBytes = 0;
};

struct VerifiedArtifact
{
    QString id;
    QString version;
    QString path;
    QString fileIdentity;
    quint64 sizeBytes = 0;
    QString sha256;
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

ArtifactFileStatus inspectArtifactFile(const QString &path,
                                       ArtifactFileMetadata *metadata = nullptr)
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
    if (metadata) {
        const quint64 fileIndex =
            (static_cast<quint64>(information.nFileIndexHigh) << 32)
            | static_cast<quint64>(information.nFileIndexLow);
        metadata->identity = QStringLiteral("windows:%1:%2")
            .arg(static_cast<quint64>(information.dwVolumeSerialNumber))
            .arg(fileIndex);
        metadata->sizeBytes =
            (static_cast<quint64>(information.nFileSizeHigh) << 32)
            | static_cast<quint64>(information.nFileSizeLow);
    }
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
    if (metadata) {
        metadata->identity = QStringLiteral("unix:%1:%2")
            .arg(static_cast<qulonglong>(information.st_dev))
            .arg(static_cast<qulonglong>(information.st_ino));
        metadata->sizeBytes = static_cast<quint64>(information.st_size);
    }
    return ArtifactFileStatus::Valid;
#endif
}

bool inspectOpenArtifactFile(const QFile &file, ArtifactFileMetadata *metadata)
{
    if (!metadata || !file.isOpen() || file.handle() < 0) return false;
#ifdef Q_OS_WIN
    const intptr_t nativeHandle = _get_osfhandle(file.handle());
    if (nativeHandle == -1) return false;
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(
            reinterpret_cast<HANDLE>(nativeHandle), &information) == 0
        || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY
                                            | FILE_ATTRIBUTE_REPARSE_POINT)) != 0
        || information.nNumberOfLinks != 1) {
        return false;
    }
    const quint64 fileIndex =
        (static_cast<quint64>(information.nFileIndexHigh) << 32)
        | static_cast<quint64>(information.nFileIndexLow);
    metadata->identity = QStringLiteral("windows:%1:%2")
        .arg(static_cast<quint64>(information.dwVolumeSerialNumber))
        .arg(fileIndex);
    metadata->sizeBytes =
        (static_cast<quint64>(information.nFileSizeHigh) << 32)
        | static_cast<quint64>(information.nFileSizeLow);
#else
    struct stat information {};
    if (::fstat(file.handle(), &information) != 0
        || !S_ISREG(information.st_mode) || information.st_nlink != 1
        || information.st_size < 0) {
        return false;
    }
    metadata->identity = QStringLiteral("unix:%1:%2")
        .arg(static_cast<qulonglong>(information.st_dev))
        .arg(static_cast<qulonglong>(information.st_ino));
    metadata->sizeBytes = static_cast<quint64>(information.st_size);
#endif
    return !metadata->identity.isEmpty();
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
                                                     bool bundledAdapter,
                                                     VerifiedArtifact *verified)
{
    if (!verified) return fail(QStringLiteral("artifact-path-invalid"));
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
    ArtifactFileMetadata before;
    const ArtifactFileStatus fileStatus = inspectArtifactFile(
        info.absoluteFilePath(), &before);
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
    if (before.identity.isEmpty()
        || before.sizeBytes > static_cast<quint64>(kMaximumArtifactBytes)
        || info.size() < 0
        || static_cast<quint64>(info.size()) != before.sizeBytes) {
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
    ArtifactFileMetadata opened;
    if (!inspectOpenArtifactFile(file, &opened)
        || opened.identity != before.identity
        || opened.sizeBytes != before.sizeBytes) {
        return fail(QStringLiteral("artifact-path-drift"), id);
    }
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
    if (file.error() != QFile::NoError
        || static_cast<quint64>(totalBytes) != before.sizeBytes) {
        return fail(QStringLiteral("artifact-read-failed"), id);
    }
    ArtifactFileMetadata after;
    if (inspectArtifactFile(artifactCanonical, &after)
            != ArtifactFileStatus::Valid
        || after.identity != before.identity
        || after.sizeBytes != before.sizeBytes
        || QFileInfo(artifactCanonical).canonicalFilePath() != artifactCanonical) {
        return fail(QStringLiteral("artifact-path-drift"), id);
    }
    const QString actualHash = QString::fromLatin1(hash.result().toHex());
    if (actualHash != expectedHash) {
        return fail(QStringLiteral("artifact-hash-mismatch"), id);
    }
    verified->id = id;
    verified->version = version;
    verified->path = artifactCanonical;
    verified->fileIdentity = before.identity;
    verified->sizeBytes = before.sizeBytes;
    verified->sha256 = actualHash;
    ArtifactManifest::VerificationResult success;
    success.ok = true;
    return success;
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
    VerifiedArtifact verifiedRuntime;
    const auto runtimeResult = verifyArtifact(
        runtime.toObject(), baseDirectory, runtimePath, false, &verifiedRuntime);
    if (!runtimeResult.ok) return runtimeResult;
    VerifiedArtifact verifiedAdapter;
    const auto adapterResult = verifyArtifact(
        adapter.toObject(), baseDirectory, {}, true, &verifiedAdapter);
    if (!adapterResult.ok) return adapterResult;
    if (verifiedRuntime.path == verifiedAdapter.path
        || verifiedRuntime.fileIdentity == verifiedAdapter.fileIdentity) {
        return fail(QStringLiteral("artifact-path-duplicate"));
    }
    VerificationResult result;
    result.ok = true;
    result.artifactId = verifiedRuntime.id;
    result.version = verifiedRuntime.version + QStringLiteral("/")
        + verifiedAdapter.version;
    result.runtimeId = verifiedRuntime.id;
    result.runtimeVersion = verifiedRuntime.version;
    result.runtimePath = verifiedRuntime.path;
    result.runtimeFileIdentity = verifiedRuntime.fileIdentity;
    result.runtimeSizeBytes = verifiedRuntime.sizeBytes;
    result.runtimeSha256 = verifiedRuntime.sha256;
    result.adapterId = verifiedAdapter.id;
    result.adapterVersion = verifiedAdapter.version;
    result.adapterPath = verifiedAdapter.path;
    result.adapterFileIdentity = verifiedAdapter.fileIdentity;
    result.adapterSizeBytes = verifiedAdapter.sizeBytes;
    result.adapterSha256 = verifiedAdapter.sha256;
    return result;
}

VerificationResult verifyFile(const QString &manifestPath, const QString &runtimePath)
{
    const QFileInfo manifestInfo(manifestPath);
    const QString manifestCanonical = manifestInfo.canonicalFilePath();
    if (!manifestInfo.isFile() || !manifestInfo.isReadable()
        || manifestInfo.isSymLink() || pathIsLinkLike(manifestPath)
        || manifestCanonical.isEmpty()) {
        return fail(QStringLiteral("manifest-path-invalid"));
    }
    ArtifactFileMetadata manifestBefore;
    const ArtifactFileStatus manifestStatus = inspectArtifactFile(
        manifestInfo.absoluteFilePath(), &manifestBefore);
    if (manifestStatus == ArtifactFileStatus::Invalid
        || manifestStatus == ArtifactFileStatus::LinkLike) {
        return fail(QStringLiteral("manifest-path-invalid"));
    }
    if (manifestStatus == ArtifactFileStatus::MultipleLinks) {
        return fail(QStringLiteral("manifest-hard-link"));
    }
    QFile file(manifestCanonical);
    if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("manifest-unreadable"));
    ArtifactFileMetadata manifestOpened;
    if (!inspectOpenArtifactFile(file, &manifestOpened)
        || manifestOpened.identity != manifestBefore.identity
        || manifestOpened.sizeBytes != manifestBefore.sizeBytes) {
        return fail(QStringLiteral("manifest-path-drift"));
    }
    const qint64 manifestSize = file.size();
    if (manifestSize < 0 || manifestSize > kMaximumManifestBytes
        || static_cast<quint64>(manifestSize) != manifestBefore.sizeBytes) {
        return fail(QStringLiteral("manifest-size-limit"));
    }
    const QByteArray manifestBytes = file.read(kMaximumManifestBytes + 1);
    if (manifestBytes.size() != manifestSize || file.error() != QFile::NoError
        || !file.atEnd()) {
        return fail(QStringLiteral("manifest-read-failed"));
    }
    ArtifactFileMetadata manifestAfter;
    if (inspectArtifactFile(manifestCanonical, &manifestAfter)
            != ArtifactFileStatus::Valid
        || manifestAfter.identity != manifestBefore.identity
        || manifestAfter.sizeBytes != manifestBefore.sizeBytes
        || QFileInfo(manifestCanonical).canonicalFilePath() != manifestCanonical) {
        return fail(QStringLiteral("manifest-path-drift"));
    }
    using namespace aegisy::aap::transport_runtime;
    TransportJsonValue parsed;
    QString parseError;
    if (!parseTransportJsonRaw(manifestBytes, &parsed, &parseError)) {
        return fail(QStringLiteral("manifest-invalid-json"));
    }
    QJsonValue projected;
    TransportProjectionError projectionError = TransportProjectionError::None;
    if (!projectJsonSafeTransportValue(parsed, &projected, &projectionError)
        || !projected.isObject()) {
        return fail(QStringLiteral("manifest-invalid-json"));
    }
    VerificationResult result = verifyObject(
        projected.toObject(), QFileInfo(manifestCanonical).absolutePath(), runtimePath);
    if (result.ok) {
        result.manifestSha256 = QString::fromLatin1(QCryptographicHash::hash(
            manifestBytes, QCryptographicHash::Sha256).toHex());
        result.manifestPath = manifestCanonical;
        result.manifestFileIdentity = manifestBefore.identity;
        result.manifestSizeBytes = manifestBefore.sizeBytes;
    }
    return result;
}

} // namespace ArtifactManifest
