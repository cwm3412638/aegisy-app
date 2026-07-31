#include "update_artifact_set.h"

#include "aap_transport_runtime.h"
#include "artifact_manifest.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QSysInfo>
#include <QUrl>

#include <openssl/evp.h>

#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#endif

namespace UpdateArtifactSet {

struct InstallationLayout
{
    QString applicationPath;
    QString installationRoot;
    QString artifactRoot;
    QString receiptPath;
    QString manifestPath;
    QString runtimePath;
    QString adapterPath;
    QString applicationFileIdentity;
    QString applicationSha256;
    quint64 applicationSizeBytes = 0;
    QString installationRootFileIdentity;
    QString artifactRootFileIdentity;
    QString receiptFileIdentity;
    QString receiptSha256;
    quint64 receiptSizeBytes = 0;
    QString manifestFileIdentity;
    QString manifestSha256;
    quint64 manifestSizeBytes = 0;
    QString runtimeFileIdentity;
    QString runtimeSha256;
    quint64 runtimeSizeBytes = 0;
    QString adapterFileIdentity;
    QString adapterSha256;
    quint64 adapterSizeBytes = 0;
#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
    bool testOnly = false;
#endif
};

namespace {

constexpr double kMaximumSafeJsonInteger = 9007199254740991.0;
constexpr quint64 kMaximumInstallerBytes = 2ULL * 1024 * 1024 * 1024;
constexpr qint64 kMaximumClockSkewMs = 5LL * 60 * 1000;
constexpr qint64 kMaximumApplicationBytes = 512LL * 1024 * 1024;
constexpr int kMaximumCompatibleSources = 64;
constexpr qsizetype kMaximumEnvelopeBytes = 256 * 1024;
const QString kLegacySchema =
    QStringLiteral("aegisy-update-artifact-set/0.1");
const QString kSchema = QStringLiteral("aegisy-update-artifact-set/0.2");
const QString kInstalledReceiptFileName =
    QStringLiteral("aegisy-update-artifact-set.json");
const QString kArtifactManifestFileName =
    QStringLiteral("aegisy-agentd.manifest.json");

struct InstalledSetData
{
    QString schemaVersion;
    quint64 releaseSequence = 0;
    QString channel;
    QString applicationVersion;
    QString platform;
    QString architecture;
    quint64 applicationSizeBytes = 0;
    QString applicationSha256;
    QString manifestSha256;
    QString runtimeId;
    QString runtimeVersion;
    QString adapterId;
    QString adapterVersion;
};

bool fail(QString *errorCode, const QString &code);

struct NativePathMetadata
{
    QString identity;
    quint64 sizeBytes = 0;
};

struct LayoutPathObservation
{
    QString canonicalPath;
    QString fileIdentity;
    QString sha256;
    quint64 sizeBytes = 0;
};

bool inspectNativePath(const QString &path, bool expectDirectory,
                       NativePathMetadata *metadata)
{
    if (!metadata) return false;
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT
        | (expectDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool inspected = GetFileInformationByHandle(handle, &information) != 0;
    CloseHandle(handle);
    if (!inspected
        || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || (((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            != expectDirectory)
        || (!expectDirectory && information.nNumberOfLinks != 1)) {
        return false;
    }
    const quint64 fileIndex = (static_cast<quint64>(information.nFileIndexHigh)
                               << 32)
        | static_cast<quint64>(information.nFileIndexLow);
    metadata->identity = QStringLiteral("windows:%1:%2")
        .arg(static_cast<quint64>(information.dwVolumeSerialNumber))
        .arg(fileIndex);
    metadata->sizeBytes = (static_cast<quint64>(information.nFileSizeHigh) << 32)
        | static_cast<quint64>(information.nFileSizeLow);
#else
    const QByteArray encodedPath = QFile::encodeName(path);
    struct stat information {};
    if (::lstat(encodedPath.constData(), &information) != 0
        || (expectDirectory ? !S_ISDIR(information.st_mode)
                            : !S_ISREG(information.st_mode))
        || (!expectDirectory
            && (information.st_nlink != 1 || information.st_size < 0))) {
        return false;
    }
    metadata->identity = QStringLiteral("unix:%1:%2")
        .arg(static_cast<qulonglong>(information.st_dev))
        .arg(static_cast<qulonglong>(information.st_ino));
    metadata->sizeBytes = expectDirectory
        ? 0 : static_cast<quint64>(information.st_size);
#endif
    return !metadata->identity.isEmpty();
}

bool inspectOpenNativeFile(const QFile &file, NativePathMetadata *metadata)
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

bool observeLayoutDirectory(const QString &path,
                            LayoutPathObservation *observation)
{
    if (!observation) return false;
    const QFileInfo info(path);
    const QString canonicalPath = info.canonicalFilePath();
    NativePathMetadata before;
    NativePathMetadata after;
    if (!info.isDir() || info.isSymLink() || canonicalPath.isEmpty()
        || !inspectNativePath(canonicalPath, true, &before)
        || !inspectNativePath(canonicalPath, true, &after)
        || before.identity != after.identity) {
        return false;
    }
    observation->canonicalPath = canonicalPath;
    observation->fileIdentity = before.identity;
    observation->sha256.clear();
    observation->sizeBytes = 0;
    return true;
}

bool observeLayoutApplication(const QString &path,
                              LayoutPathObservation *observation)
{
    if (!observation) return false;
    const QFileInfo info(path);
    const QString canonicalPath = info.canonicalFilePath();
    NativePathMetadata before;
    if (!info.isFile() || !info.isReadable() || info.isSymLink()
        || canonicalPath.isEmpty()
        || !inspectNativePath(canonicalPath, false, &before)
        || before.sizeBytes == 0
        || before.sizeBytes > static_cast<quint64>(kMaximumApplicationBytes)) {
        return false;
    }

    QFile file(canonicalPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    NativePathMetadata opened;
    if (!inspectOpenNativeFile(file, &opened)
        || opened.identity != before.identity
        || opened.sizeBytes != before.sizeBytes) {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 totalBytes = 0;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(64 * 1024);
        if (chunk.isEmpty() && !file.atEnd()) return false;
        if (chunk.size() > kMaximumApplicationBytes - totalBytes) return false;
        totalBytes += chunk.size();
        hash.addData(chunk);
    }
    if (file.error() != QFile::NoError
        || static_cast<quint64>(totalBytes) != before.sizeBytes) {
        return false;
    }
    NativePathMetadata after;
    if (!inspectNativePath(canonicalPath, false, &after)
        || after.identity != before.identity
        || after.sizeBytes != before.sizeBytes) {
        return false;
    }
    observation->canonicalPath = canonicalPath;
    observation->fileIdentity = before.identity;
    observation->sha256 = QString::fromLatin1(hash.result().toHex());
    observation->sizeBytes = before.sizeBytes;
    return true;
}

QString runtimeFileName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("aegisy-agentd.exe");
#else
    return QStringLiteral("aegisy-agentd");
#endif
}

bool currentReleaseTarget(QString *platform, QString *architecture)
{
#ifdef Q_OS_WIN
    *platform = QStringLiteral("windows");
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    *platform = QStringLiteral("macos");
#else
    Q_UNUSED(platform);
    Q_UNUSED(architecture);
    return false;
#endif

    const QString buildArchitecture = QSysInfo::buildCpuArchitecture().toLower();
    if (buildArchitecture == QStringLiteral("x86_64")
        || buildArchitecture == QStringLiteral("amd64")) {
        *architecture = QStringLiteral("x86_64");
    } else if (buildArchitecture == QStringLiteral("arm64")
               || buildArchitecture == QStringLiteral("aarch64")) {
        *architecture = QStringLiteral("arm64");
    } else {
        return false;
    }
    return true;
}

QString canonicalDirectory(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink()) return {};
    return info.canonicalFilePath();
}

bool samePath(const QString &left, const QString &right)
{
#ifdef Q_OS_WIN
    return left.compare(right, Qt::CaseInsensitive) == 0;
#else
    return left == right;
#endif
}

bool pathWithin(const QString &parent, const QString &path)
{
    const QString prefix = parent + QDir::separator();
#ifdef Q_OS_WIN
    return path.startsWith(prefix, Qt::CaseInsensitive);
#else
    return path.startsWith(prefix);
#endif
}

bool deriveCurrentInstallationLayout(InstallationLayout *layout,
                                     QString *errorCode)
{
    if (!layout) return fail(errorCode,
                             QStringLiteral("installed-authority-layout-invalid"));
    const QFileInfo applicationInfo(QCoreApplication::applicationFilePath());
    const QString applicationPath = applicationInfo.canonicalFilePath();
#ifdef Q_OS_WIN
    const bool expectedExecutableName = applicationInfo.fileName().compare(
        QStringLiteral("AegisyClient.exe"), Qt::CaseInsensitive) == 0;
#else
    const bool expectedExecutableName = applicationInfo.fileName()
        == QStringLiteral("AegisyClient");
#endif
    if (!expectedExecutableName || !applicationInfo.isFile()
        || !applicationInfo.isExecutable() || applicationInfo.isSymLink()
        || applicationPath.isEmpty()) {
        return fail(errorCode,
                    QStringLiteral("installed-authority-application-invalid"));
    }

    QString installationRoot;
    QString artifactRoot;
#ifdef Q_OS_WIN
    installationRoot = canonicalDirectory(applicationInfo.absolutePath());
    artifactRoot = installationRoot;
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    QDir macOsDirectory(applicationInfo.absolutePath());
    if (macOsDirectory.dirName() != QStringLiteral("MacOS")
        || !macOsDirectory.cdUp()
        || macOsDirectory.dirName() != QStringLiteral("Contents")) {
        return fail(errorCode,
                    QStringLiteral("installed-authority-layout-invalid"));
    }
    artifactRoot = canonicalDirectory(applicationInfo.absolutePath());
    QDir bundleDirectory(macOsDirectory);
    if (!bundleDirectory.cdUp()) {
        return fail(errorCode,
                    QStringLiteral("installed-authority-layout-invalid"));
    }
    installationRoot = canonicalDirectory(bundleDirectory.absolutePath());
#else
    return fail(errorCode,
                QStringLiteral("installed-authority-platform-unsupported"));
#endif
    if (installationRoot.isEmpty() || artifactRoot.isEmpty()
        || (!samePath(artifactRoot, installationRoot)
            && !pathWithin(installationRoot, artifactRoot))) {
        return fail(errorCode,
                    QStringLiteral("installed-authority-layout-invalid"));
    }

    layout->applicationPath = applicationPath;
    layout->installationRoot = installationRoot;
    layout->artifactRoot = artifactRoot;
    layout->receiptPath = QDir(artifactRoot).filePath(
        kInstalledReceiptFileName);
    layout->manifestPath = QDir(artifactRoot).filePath(
        kArtifactManifestFileName);
    layout->runtimePath = QDir(artifactRoot).filePath(runtimeFileName());
    if (errorCode) errorCode->clear();
    return true;
}

QString installationLayoutIdentity(const InstallationLayout &layout)
{
    if (layout.applicationPath.isEmpty()
        || layout.installationRoot.isEmpty()
        || layout.artifactRoot.isEmpty()
        || layout.applicationFileIdentity.isEmpty()
        || layout.applicationSha256.size() != 64
        || layout.applicationSizeBytes == 0
        || layout.installationRootFileIdentity.isEmpty()
        || layout.artifactRootFileIdentity.isEmpty()
        || layout.receiptPath.isEmpty()
        || layout.receiptFileIdentity.isEmpty()
        || layout.receiptSha256.size() != 64
        || layout.receiptSizeBytes == 0
        || layout.manifestPath.isEmpty()
        || layout.manifestFileIdentity.isEmpty()
        || layout.manifestSha256.size() != 64
        || layout.manifestSizeBytes == 0
        || layout.runtimePath.isEmpty()
        || layout.runtimeFileIdentity.isEmpty()
        || layout.runtimeSha256.size() != 64
        || layout.runtimeSizeBytes == 0
        || layout.adapterPath.isEmpty()
        || layout.adapterFileIdentity.isEmpty()
        || layout.adapterSha256.size() != 64
        || layout.adapterSizeBytes == 0) {
        return {};
    }
    QByteArray payload = QByteArrayLiteral(
        "aegisy-current-installation-layout/0.2\n");
    const auto appendValue = [&payload](const QByteArray &name,
                                        const QByteArray &value) {
        payload += name;
        payload += '=';
        payload += value;
        payload += '\n';
    };
    const auto appendPathHash = [&appendValue](const QByteArray &name,
                                               const QString &path) {
        appendValue(name, QCryptographicHash::hash(
            path.toUtf8(), QCryptographicHash::Sha256).toHex());
    };
    appendPathHash(QByteArrayLiteral("application.path_sha256"),
                   layout.applicationPath);
    appendValue(QByteArrayLiteral("application.file_identity"),
                layout.applicationFileIdentity.toUtf8());
    appendValue(QByteArrayLiteral("application.sha256"),
                layout.applicationSha256.toLatin1());
    appendValue(QByteArrayLiteral("application.size_bytes"),
                QByteArray::number(layout.applicationSizeBytes));
    appendPathHash(QByteArrayLiteral("installation_root.path_sha256"),
                   layout.installationRoot);
    appendValue(QByteArrayLiteral("installation_root.file_identity"),
                layout.installationRootFileIdentity.toUtf8());
    appendPathHash(QByteArrayLiteral("artifact_root.path_sha256"),
                   layout.artifactRoot);
    appendValue(QByteArrayLiteral("artifact_root.file_identity"),
                layout.artifactRootFileIdentity.toUtf8());
    const auto appendFile = [&appendPathHash, &appendValue](
                                const QByteArray &name,
                                const QString &path,
                                const QString &identity,
                                const QString &sha256,
                                quint64 sizeBytes) {
        appendPathHash(name + QByteArrayLiteral(".path_sha256"), path);
        appendValue(name + QByteArrayLiteral(".file_identity"),
                    identity.toUtf8());
        appendValue(name + QByteArrayLiteral(".sha256"), sha256.toLatin1());
        appendValue(name + QByteArrayLiteral(".size_bytes"),
                    QByteArray::number(sizeBytes));
    };
    appendFile(QByteArrayLiteral("receipt"), layout.receiptPath,
               layout.receiptFileIdentity, layout.receiptSha256,
               layout.receiptSizeBytes);
    appendFile(QByteArrayLiteral("manifest"), layout.manifestPath,
               layout.manifestFileIdentity, layout.manifestSha256,
               layout.manifestSizeBytes);
    appendFile(QByteArrayLiteral("runtime"), layout.runtimePath,
               layout.runtimeFileIdentity, layout.runtimeSha256,
               layout.runtimeSizeBytes);
    appendFile(QByteArrayLiteral("adapter"), layout.adapterPath,
               layout.adapterFileIdentity, layout.adapterSha256,
               layout.adapterSizeBytes);
    return QStringLiteral("current-installation-layout:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()));
}

struct Application
{
    QString version;
    QString platform;
    QString architecture;
    quint64 sizeBytes = 0;
    QString sha256;
};

struct Component
{
    QString id;
    QString version;
};

struct Manifest
{
    QString sha256;
    Component runtime;
    Component adapter;
};

struct Installer
{
    QString url;
    QString fileName;
    quint64 sizeBytes = 0;
    QString sha256;
    QString sparkleSignature;
};

struct Source
{
    quint64 releaseSequence = 0;
    QString channel;
    Application application;
    Manifest manifest;
};

struct Candidate
{
    QString schemaVersion;
    quint64 releaseSequence = 0;
    quint64 publishedAtMs = 0;
    quint64 signedAtMs = 0;
    quint64 expiresAtMs = 0;
    QString signingKeyId;
    QString payloadIdentity;
    QString channel;
    Application application;
    Installer installer;
    Manifest manifest;
    QList<Source> sources;
    QString signature;
};

bool fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
    return false;
}

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &keys)
{
    if (object.size() != keys.size()) return false;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!keys.contains(it.key())) return false;
    }
    return true;
}

bool safePositiveInteger(const QJsonValue &value, quint64 *output)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number <= 0 || number > kMaximumSafeJsonInteger) {
        return false;
    }
    *output = static_cast<quint64>(number);
    return true;
}

bool exactString(const QJsonObject &object, const QString &key, QString *output)
{
    const QJsonValue value = object.value(key);
    if (!value.isString()) return false;
    *output = value.toString();
    return true;
}

bool matches(const QString &value, const QRegularExpression &pattern)
{
    return pattern.match(value).hasMatch();
}

bool validChannel(const QString &channel)
{
    return channel == QStringLiteral("internal")
        || channel == QStringLiteral("preview")
        || channel == QStringLiteral("beta")
        || channel == QStringLiteral("stable");
}

bool validVersion(const QString &version)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[0-9A-Za-z][0-9A-Za-z.+-]{0,63}\\z"));
    return matches(version, pattern);
}

bool validAdapterVersion(const QString &version)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\Acodex-cli [0-9A-Za-z][0-9A-Za-z.+-]{0,63}\\z"));
    return matches(version, pattern);
}

bool validSha256(const QString &sha256)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[0-9a-f]{64}\\z"));
    return matches(sha256, pattern);
}

bool validKeyId(const QString &keyId)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[a-z0-9][a-z0-9._-]{0,63}\\z"));
    return matches(keyId, pattern);
}

bool validPlatformArchitecture(const QString &platform, const QString &architecture)
{
    return (platform == QStringLiteral("macos")
            && architecture == QStringLiteral("arm64"))
        || (platform == QStringLiteral("windows")
            && architecture == QStringLiteral("x86_64"));
}

QByteArray decodeCanonicalBase64(const QString &encoded, qsizetype expectedBytes)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[A-Za-z0-9+/]+={0,2}\\z"));
    if (!pattern.match(encoded).hasMatch()) return {};
    const QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
    if (bytes.size() != expectedBytes || bytes.toBase64() != encoded.toLatin1()) return {};
    return bytes;
}

enum class SignatureVerification {
    Valid,
    Invalid,
    Unavailable,
};

SignatureVerification verifyEd25519(const QByteArray &publicKey,
                                    const QByteArray &signature,
                                    const QByteArray &payload)
{
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char *>(publicKey.constData()),
        static_cast<size_t>(publicKey.size()));
    if (!key) return SignatureVerification::Unavailable;
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    if (!context
        || EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) != 1) {
        EVP_MD_CTX_free(context);
        EVP_PKEY_free(key);
        return SignatureVerification::Unavailable;
    }
    const int result = EVP_DigestVerify(
        context,
        reinterpret_cast<const unsigned char *>(signature.constData()),
        static_cast<size_t>(signature.size()),
        reinterpret_cast<const unsigned char *>(payload.constData()),
        static_cast<size_t>(payload.size()));
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    if (result == 1) return SignatureVerification::Valid;
    if (result == 0) return SignatureVerification::Invalid;
    return SignatureVerification::Unavailable;
}

bool parseLegacyApplication(const QJsonValue &value, Application *application,
                            QString *errorCode)
{
    if (!value.isObject()) {
        return fail(errorCode, QStringLiteral("artifact-set-application-invalid"));
    }
    const QJsonObject object = value.toObject();
    if (!hasExactKeys(object, {
            QStringLiteral("version"), QStringLiteral("platform"),
            QStringLiteral("architecture"),
        })
        || !exactString(object, QStringLiteral("version"), &application->version)
        || !exactString(object, QStringLiteral("platform"), &application->platform)
        || !exactString(object, QStringLiteral("architecture"), &application->architecture)
        || !validVersion(application->version)
        || !validPlatformArchitecture(application->platform,
                                      application->architecture)) {
        return fail(errorCode, QStringLiteral("artifact-set-application-invalid"));
    }
    return true;
}

bool parseApplication(const QJsonValue &value, Application *application,
                      QString *errorCode)
{
    if (!value.isObject()) {
        return fail(errorCode, QStringLiteral("artifact-set-application-invalid"));
    }
    const QJsonObject object = value.toObject();
    if (!hasExactKeys(object, {
            QStringLiteral("version"), QStringLiteral("platform"),
            QStringLiteral("architecture"), QStringLiteral("size_bytes"),
            QStringLiteral("sha256"),
        })
        || !exactString(object, QStringLiteral("version"), &application->version)
        || !exactString(object, QStringLiteral("platform"), &application->platform)
        || !exactString(object, QStringLiteral("architecture"),
                        &application->architecture)
        || !safePositiveInteger(object.value(QStringLiteral("size_bytes")),
                                &application->sizeBytes)
        || application->sizeBytes
            > static_cast<quint64>(kMaximumApplicationBytes)
        || !exactString(object, QStringLiteral("sha256"), &application->sha256)
        || !validVersion(application->version)
        || !validPlatformArchitecture(application->platform,
                                      application->architecture)
        || !validSha256(application->sha256)) {
        return fail(errorCode, QStringLiteral("artifact-set-application-invalid"));
    }
    return true;
}

bool parseComponent(const QJsonValue &value, const QString &requiredId,
                    bool adapter, Component *component, QString *errorCode)
{
    if (!value.isObject()) {
        return fail(errorCode, QStringLiteral("artifact-set-component-invalid"));
    }
    const QJsonObject object = value.toObject();
    if (!hasExactKeys(object, {
            QStringLiteral("id"), QStringLiteral("version"),
        })
        || !exactString(object, QStringLiteral("id"), &component->id)
        || !exactString(object, QStringLiteral("version"), &component->version)
        || component->id != requiredId
        || (adapter ? !validAdapterVersion(component->version)
                    : !validVersion(component->version))) {
        return fail(errorCode, QStringLiteral("artifact-set-component-invalid"));
    }
    return true;
}

bool parseManifest(const QJsonValue &value, Manifest *manifest, QString *errorCode)
{
    if (!value.isObject()) {
        return fail(errorCode, QStringLiteral("artifact-set-manifest-invalid"));
    }
    const QJsonObject object = value.toObject();
    if (!hasExactKeys(object, {
            QStringLiteral("sha256"), QStringLiteral("runtime"),
            QStringLiteral("adapter"),
        })
        || !exactString(object, QStringLiteral("sha256"), &manifest->sha256)
        || !validSha256(manifest->sha256)
        || !parseComponent(object.value(QStringLiteral("runtime")),
                           QStringLiteral("aegisy-agentd"), false,
                           &manifest->runtime, errorCode)
        || !parseComponent(object.value(QStringLiteral("adapter")),
                           QStringLiteral("codex-app-server"), true,
                           &manifest->adapter, errorCode)) {
        if (errorCode && errorCode->isEmpty()) {
            *errorCode = QStringLiteral("artifact-set-manifest-invalid");
        }
        return false;
    }
    return true;
}

bool printableAscii(const QString &value, int maximumBytes)
{
    if (value.isEmpty() || value.size() > maximumBytes) {
        return false;
    }
    for (const QChar characterValue : value) {
        const ushort character = characterValue.unicode();
        if (character < 0x21 || character > 0x7e) return false;
    }
    return true;
}

bool validDecodedPath(const QString &path)
{
    if (!path.startsWith(QLatin1Char('/'))) return false;
    const QStringList segments = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    if (segments.size() < 2 || !segments.constFirst().isEmpty()) return false;
    for (int index = 1; index < segments.size(); ++index) {
        const QString &segment = segments.at(index);
        if (segment.isEmpty() || segment == QStringLiteral(".")
            || segment == QStringLiteral("..")) {
            return false;
        }
        for (const QChar characterValue : segment) {
            const ushort character = characterValue.unicode();
            if (character <= 0x20 || character > 0x7e
                || character == '/' || character == '\\') {
                return false;
            }
        }
    }
    return true;
}

bool reservedWindowsFileName(const QString &fileName)
{
    const QString stem = fileName.section(QLatin1Char('.'), 0, 0).toUpper();
    static const QSet<QString> fixedNames{
        QStringLiteral("CON"), QStringLiteral("PRN"),
        QStringLiteral("AUX"), QStringLiteral("NUL"),
    };
    if (fixedNames.contains(stem)) return true;
    static const QRegularExpression numberedName(
        QStringLiteral("\\A(?:COM|LPT)[1-9]\\z"));
    return matches(stem, numberedName);
}

bool parseInstaller(const QJsonValue &value, const Application &application,
                    Installer *installer, QString *errorCode)
{
    if (!value.isObject()) {
        return fail(errorCode, QStringLiteral("artifact-set-installer-invalid"));
    }
    const QJsonObject object = value.toObject();
    if (!hasExactKeys(object, {
            QStringLiteral("url"), QStringLiteral("file_name"),
            QStringLiteral("size_bytes"), QStringLiteral("sha256"),
            QStringLiteral("sparkle_ed_signature"),
        })
        || !exactString(object, QStringLiteral("url"), &installer->url)
        || !exactString(object, QStringLiteral("file_name"), &installer->fileName)
        || !safePositiveInteger(object.value(QStringLiteral("size_bytes")),
                                &installer->sizeBytes)
        || installer->sizeBytes > kMaximumInstallerBytes
        || !exactString(object, QStringLiteral("sha256"), &installer->sha256)
        || !validSha256(installer->sha256)
        || !exactString(object, QStringLiteral("sparkle_ed_signature"),
                        &installer->sparkleSignature)
        || decodeCanonicalBase64(installer->sparkleSignature, 64).isEmpty()) {
        return fail(errorCode, QStringLiteral("artifact-set-installer-invalid"));
    }
    static const QRegularExpression fileNamePattern(
        QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._-]{0,127}\\z"));
    const QString requiredSuffix = application.platform == QStringLiteral("macos")
        ? QStringLiteral(".zip") : QStringLiteral(".exe");
    if (!matches(installer->fileName, fileNamePattern)
        || !installer->fileName.endsWith(requiredSuffix, Qt::CaseSensitive)
        || (application.platform == QStringLiteral("windows")
            && reservedWindowsFileName(installer->fileName))
        || !printableAscii(installer->url, 2048)) {
        return fail(errorCode, QStringLiteral("artifact-set-installer-invalid"));
    }
    const QUrl url(installer->url, QUrl::StrictMode);
    const QString encodedPath = url.path(QUrl::FullyEncoded);
    const QString decodedPath = url.path(QUrl::FullyDecoded);
    const QString encodedAuthority = url.authority(QUrl::FullyEncoded);
    if (!url.isValid() || url.scheme() != QStringLiteral("https")
        || url.host().isEmpty() || encodedAuthority.contains(QLatin1Char('@'))
        || url.hasQuery() || url.hasFragment()
        || (url.port(-1) != -1 && url.port(-1) != 443)
        || url.toString(QUrl::FullyEncoded) != installer->url
        || encodedPath != decodedPath
        || encodedPath.contains(QStringLiteral("%25"), Qt::CaseInsensitive)
        || encodedPath.contains(QStringLiteral("%2f"), Qt::CaseInsensitive)
        || encodedPath.contains(QStringLiteral("%5c"), Qt::CaseInsensitive)
        || encodedPath.contains(QLatin1Char('\\'))
        || !validDecodedPath(decodedPath)
        || !encodedPath.endsWith(QStringLiteral("/") + installer->fileName)) {
        return fail(errorCode, QStringLiteral("artifact-set-installer-url-invalid"));
    }
    if (url.fileName(QUrl::FullyDecoded) != installer->fileName) {
        return fail(errorCode, QStringLiteral("artifact-set-installer-url-invalid"));
    }
    return true;
}

bool parseLegacySource(const QJsonValue &value, Source *source,
                       QString *errorCode)
{
    if (!value.isObject()) {
        return fail(errorCode, QStringLiteral("artifact-set-source-invalid"));
    }
    const QJsonObject object = value.toObject();
    if (!hasExactKeys(object, {
            QStringLiteral("release_sequence"), QStringLiteral("channel"),
            QStringLiteral("application"), QStringLiteral("manifest"),
        })
        || !safePositiveInteger(object.value(QStringLiteral("release_sequence")),
                                &source->releaseSequence)
        || !exactString(object, QStringLiteral("channel"), &source->channel)
        || !validChannel(source->channel)
        || !parseLegacyApplication(object.value(QStringLiteral("application")),
                                   &source->application, errorCode)
        || !parseManifest(object.value(QStringLiteral("manifest")),
                          &source->manifest, errorCode)) {
        if (errorCode && errorCode->isEmpty()) {
            *errorCode = QStringLiteral("artifact-set-source-invalid");
        }
        return false;
    }
    return true;
}

bool parseLegacyCandidate(const QJsonObject &envelope, Candidate *candidate,
                          QString *errorCode)
{
    if (!hasExactKeys(envelope, {
            QStringLiteral("schema_version"), QStringLiteral("release_sequence"),
            QStringLiteral("published_at_ms"), QStringLiteral("channel"),
            QStringLiteral("application"), QStringLiteral("installer"),
            QStringLiteral("target_manifest"), QStringLiteral("compatible_sources"),
            QStringLiteral("signature"),
        })) {
        return fail(errorCode, QStringLiteral("artifact-set-fields-invalid"));
    }
    QString schema;
    if (!exactString(envelope, QStringLiteral("schema_version"), &schema)
        || schema != kLegacySchema) {
        return fail(errorCode, QStringLiteral("artifact-set-schema-invalid"));
    }
    candidate->schemaVersion = schema;
    if (!safePositiveInteger(envelope.value(QStringLiteral("release_sequence")),
                             &candidate->releaseSequence)
        || !safePositiveInteger(envelope.value(QStringLiteral("published_at_ms")),
                                &candidate->publishedAtMs)
        || !exactString(envelope, QStringLiteral("channel"), &candidate->channel)
        || !validChannel(candidate->channel)
        || !exactString(envelope, QStringLiteral("signature"), &candidate->signature)
        || candidate->signature.size() > 128
        || !parseLegacyApplication(
            envelope.value(QStringLiteral("application")),
            &candidate->application, errorCode)
        || !parseInstaller(envelope.value(QStringLiteral("installer")),
                           candidate->application, &candidate->installer, errorCode)
        || !parseManifest(envelope.value(QStringLiteral("target_manifest")),
                          &candidate->manifest, errorCode)) {
        if (errorCode && errorCode->isEmpty()) {
            *errorCode = QStringLiteral("artifact-set-value-invalid");
        }
        return false;
    }
    const QJsonValue sourcesValue = envelope.value(QStringLiteral("compatible_sources"));
    if (!sourcesValue.isArray()) {
        return fail(errorCode, QStringLiteral("artifact-set-sources-invalid"));
    }
    const QJsonArray sources = sourcesValue.toArray();
    if (sources.isEmpty() || sources.size() > kMaximumCompatibleSources) {
        return fail(errorCode, QStringLiteral("artifact-set-sources-invalid"));
    }
    quint64 previousSequence = 0;
    for (const QJsonValue &sourceValue : sources) {
        Source source;
        if (!parseLegacySource(sourceValue, &source, errorCode)) return false;
        if (source.releaseSequence <= previousSequence
            || source.releaseSequence >= candidate->releaseSequence) {
            return fail(errorCode, QStringLiteral("artifact-set-source-sequence-invalid"));
        }
        if (source.application.platform != candidate->application.platform
            || source.application.architecture != candidate->application.architecture
            || source.application.version == candidate->application.version) {
            return fail(errorCode, QStringLiteral("artifact-set-source-target-invalid"));
        }
        previousSequence = source.releaseSequence;
        candidate->sources.append(source);
    }
    if (errorCode) errorCode->clear();
    return true;
}

bool validPayloadIdentity(const QString &identity)
{
    static const QRegularExpression pattern(QStringLiteral(
        "\\Aupdate-artifact-set-payload:sha256:[0-9a-f]{64}\\z"));
    return matches(identity, pattern);
}

bool parseSource(const QJsonValue &value, Source *source, QString *errorCode)
{
    if (!value.isObject()) {
        return fail(errorCode, QStringLiteral("artifact-set-source-invalid"));
    }
    const QJsonObject object = value.toObject();
    if (!hasExactKeys(object, {
            QStringLiteral("release_sequence"), QStringLiteral("channel"),
            QStringLiteral("application"), QStringLiteral("manifest"),
        })
        || !safePositiveInteger(object.value(QStringLiteral("release_sequence")),
                                &source->releaseSequence)
        || !exactString(object, QStringLiteral("channel"), &source->channel)
        || !validChannel(source->channel)
        || !parseApplication(object.value(QStringLiteral("application")),
                             &source->application, errorCode)
        || !parseManifest(object.value(QStringLiteral("manifest")),
                          &source->manifest, errorCode)) {
        if (errorCode && errorCode->isEmpty()) {
            *errorCode = QStringLiteral("artifact-set-source-invalid");
        }
        return false;
    }
    return true;
}

bool parseCandidate(const QJsonObject &envelope, Candidate *candidate,
                    bool allowEmptyPayloadIdentity, QString *errorCode)
{
    if (!hasExactKeys(envelope, {
            QStringLiteral("schema_version"), QStringLiteral("release_sequence"),
            QStringLiteral("signing_key_id"), QStringLiteral("signed_at_ms"),
            QStringLiteral("expires_at_ms"), QStringLiteral("payload_identity"),
            QStringLiteral("channel"), QStringLiteral("application"),
            QStringLiteral("installer"), QStringLiteral("target_manifest"),
            QStringLiteral("compatible_sources"), QStringLiteral("signature"),
        })) {
        return fail(errorCode, QStringLiteral("artifact-set-fields-invalid"));
    }
    QString schema;
    if (!exactString(envelope, QStringLiteral("schema_version"), &schema)
        || schema != kSchema) {
        return fail(errorCode, QStringLiteral("artifact-set-schema-invalid"));
    }
    candidate->schemaVersion = schema;
    if (!safePositiveInteger(envelope.value(QStringLiteral("release_sequence")),
                             &candidate->releaseSequence)
        || !exactString(envelope, QStringLiteral("signing_key_id"),
                        &candidate->signingKeyId)
        || !validKeyId(candidate->signingKeyId)
        || !safePositiveInteger(envelope.value(QStringLiteral("signed_at_ms")),
                                &candidate->signedAtMs)
        || !safePositiveInteger(envelope.value(QStringLiteral("expires_at_ms")),
                                &candidate->expiresAtMs)
        || candidate->signedAtMs >= candidate->expiresAtMs
        || !exactString(envelope, QStringLiteral("payload_identity"),
                        &candidate->payloadIdentity)
        || (!validPayloadIdentity(candidate->payloadIdentity)
            && !(allowEmptyPayloadIdentity
                 && candidate->payloadIdentity.isEmpty()))
        || !exactString(envelope, QStringLiteral("channel"), &candidate->channel)
        || !validChannel(candidate->channel)
        || !exactString(envelope, QStringLiteral("signature"), &candidate->signature)
        || candidate->signature.size() > 128
        || !parseApplication(envelope.value(QStringLiteral("application")),
                             &candidate->application, errorCode)
        || !parseInstaller(envelope.value(QStringLiteral("installer")),
                           candidate->application, &candidate->installer, errorCode)
        || !parseManifest(envelope.value(QStringLiteral("target_manifest")),
                          &candidate->manifest, errorCode)) {
        if (errorCode && errorCode->isEmpty()) {
            *errorCode = QStringLiteral("artifact-set-value-invalid");
        }
        return false;
    }
    const QJsonValue sourcesValue =
        envelope.value(QStringLiteral("compatible_sources"));
    if (!sourcesValue.isArray()) {
        return fail(errorCode, QStringLiteral("artifact-set-sources-invalid"));
    }
    const QJsonArray sources = sourcesValue.toArray();
    if (sources.isEmpty() || sources.size() > kMaximumCompatibleSources) {
        return fail(errorCode, QStringLiteral("artifact-set-sources-invalid"));
    }
    quint64 previousSequence = 0;
    for (const QJsonValue &sourceValue : sources) {
        Source source;
        if (!parseSource(sourceValue, &source, errorCode)) return false;
        if (source.releaseSequence <= previousSequence
            || source.releaseSequence >= candidate->releaseSequence) {
            return fail(errorCode,
                        QStringLiteral("artifact-set-source-sequence-invalid"));
        }
        if (source.application.platform != candidate->application.platform
            || source.application.architecture
                != candidate->application.architecture
            || source.application.version == candidate->application.version) {
            return fail(errorCode,
                        QStringLiteral("artifact-set-source-target-invalid"));
        }
        previousSequence = source.releaseSequence;
        candidate->sources.append(source);
    }
    if (errorCode) errorCode->clear();
    return true;
}

void appendLine(QByteArray *payload, const QByteArray &key, const QString &value)
{
    payload->append(key);
    payload->append('=');
    payload->append(value.toUtf8());
    payload->append('\n');
}

void appendLine(QByteArray *payload, const QByteArray &key, quint64 value)
{
    payload->append(key);
    payload->append('=');
    payload->append(QByteArray::number(value));
    payload->append('\n');
}

void appendLegacyApplication(QByteArray *payload, const QByteArray &prefix,
                             const Application &application)
{
    appendLine(payload, prefix + ".version", application.version);
    appendLine(payload, prefix + ".platform", application.platform);
    appendLine(payload, prefix + ".architecture", application.architecture);
}

void appendApplication(QByteArray *payload, const QByteArray &prefix,
                       const Application &application)
{
    appendLegacyApplication(payload, prefix, application);
    appendLine(payload, prefix + ".size_bytes", application.sizeBytes);
    appendLine(payload, prefix + ".sha256", application.sha256);
}

void appendManifest(QByteArray *payload, const QByteArray &prefix,
                    const Manifest &manifest)
{
    appendLine(payload, prefix + ".sha256", manifest.sha256);
    appendLine(payload, prefix + ".runtime.id", manifest.runtime.id);
    appendLine(payload, prefix + ".runtime.version", manifest.runtime.version);
    appendLine(payload, prefix + ".adapter.id", manifest.adapter.id);
    appendLine(payload, prefix + ".adapter.version", manifest.adapter.version);
}

QByteArray buildLegacyPayload(const Candidate &candidate)
{
    QByteArray payload = QByteArrayLiteral("aegisy-update-artifact-set/0.1\n");
    appendLine(&payload, QByteArrayLiteral("release_sequence"),
               candidate.releaseSequence);
    appendLine(&payload, QByteArrayLiteral("published_at_ms"),
               candidate.publishedAtMs);
    appendLine(&payload, QByteArrayLiteral("channel"), candidate.channel);
    appendLegacyApplication(&payload, QByteArrayLiteral("application"),
                            candidate.application);
    appendLine(&payload, QByteArrayLiteral("installer.url"), candidate.installer.url);
    appendLine(&payload, QByteArrayLiteral("installer.file_name"),
               candidate.installer.fileName);
    appendLine(&payload, QByteArrayLiteral("installer.size_bytes"),
               candidate.installer.sizeBytes);
    appendLine(&payload, QByteArrayLiteral("installer.sha256"),
               candidate.installer.sha256);
    appendLine(&payload, QByteArrayLiteral("installer.sparkle_ed_signature"),
               candidate.installer.sparkleSignature);
    appendManifest(&payload, QByteArrayLiteral("target_manifest"),
                   candidate.manifest);
    appendLine(&payload, QByteArrayLiteral("compatible_sources.count"),
               static_cast<quint64>(candidate.sources.size()));
    for (int index = 0; index < candidate.sources.size(); ++index) {
        const Source &source = candidate.sources.at(index);
        const QByteArray prefix = QByteArrayLiteral("compatible_sources.")
            + QByteArray::number(index);
        appendLine(&payload, prefix + ".release_sequence", source.releaseSequence);
        appendLine(&payload, prefix + ".channel", source.channel);
        appendLegacyApplication(&payload, prefix + ".application",
                                source.application);
        appendManifest(&payload, prefix + ".manifest", source.manifest);
    }
    return payload;
}


QByteArray buildPayloadBody(const Candidate &candidate)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-artifact-set-payload/0.2\n");
    appendLine(&payload, QByteArrayLiteral("release_sequence"),
               candidate.releaseSequence);
    appendLine(&payload, QByteArrayLiteral("signing_key_id"),
               candidate.signingKeyId);
    appendLine(&payload, QByteArrayLiteral("signed_at_ms"),
               candidate.signedAtMs);
    appendLine(&payload, QByteArrayLiteral("expires_at_ms"),
               candidate.expiresAtMs);
    appendLine(&payload, QByteArrayLiteral("channel"), candidate.channel);
    appendApplication(&payload, QByteArrayLiteral("application"),
                      candidate.application);
    appendLine(&payload, QByteArrayLiteral("installer.url"),
               candidate.installer.url);
    appendLine(&payload, QByteArrayLiteral("installer.file_name"),
               candidate.installer.fileName);
    appendLine(&payload, QByteArrayLiteral("installer.size_bytes"),
               candidate.installer.sizeBytes);
    appendLine(&payload, QByteArrayLiteral("installer.sha256"),
               candidate.installer.sha256);
    appendLine(&payload, QByteArrayLiteral("installer.sparkle_ed_signature"),
               candidate.installer.sparkleSignature);
    appendManifest(&payload, QByteArrayLiteral("target_manifest"),
                   candidate.manifest);
    appendLine(&payload, QByteArrayLiteral("compatible_sources.count"),
               static_cast<quint64>(candidate.sources.size()));
    for (int index = 0; index < candidate.sources.size(); ++index) {
        const Source &source = candidate.sources.at(index);
        const QByteArray prefix = QByteArrayLiteral("compatible_sources.")
            + QByteArray::number(index);
        appendLine(&payload, prefix + ".release_sequence", source.releaseSequence);
        appendLine(&payload, prefix + ".channel", source.channel);
        appendApplication(&payload, prefix + ".application", source.application);
        appendManifest(&payload, prefix + ".manifest", source.manifest);
    }
    return payload;
}

QString candidatePayloadIdentity(const Candidate &candidate)
{
    return QStringLiteral("update-artifact-set-payload:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            buildPayloadBody(candidate), QCryptographicHash::Sha256).toHex()));
}

QByteArray buildPayload(const Candidate &candidate)
{
    QByteArray payload = QByteArrayLiteral("aegisy-update-artifact-set/0.2\n");
    appendLine(&payload, QByteArrayLiteral("payload_identity"),
               candidate.payloadIdentity);
    payload += buildPayloadBody(candidate);
    return payload;
}

bool validLegacyInstalled(const InstalledSetData &installed)
{
    return installed.releaseSequence > 0
        && installed.releaseSequence <= static_cast<quint64>(kMaximumSafeJsonInteger)
        && validChannel(installed.channel)
        && validVersion(installed.applicationVersion)
        && validPlatformArchitecture(installed.platform, installed.architecture)
        && validSha256(installed.manifestSha256)
        && installed.runtimeId == QStringLiteral("aegisy-agentd")
        && validVersion(installed.runtimeVersion)
        && installed.adapterId == QStringLiteral("codex-app-server")
        && validAdapterVersion(installed.adapterVersion);
}

bool validInstalled(const InstalledSetData &installed)
{
    return installed.schemaVersion == kSchema
        && validLegacyInstalled(installed)
        && installed.applicationSizeBytes > 0
        && installed.applicationSizeBytes
            <= static_cast<quint64>(kMaximumApplicationBytes)
        && validSha256(installed.applicationSha256);
}

bool matchesInstalled(const Source &source, const InstalledSetData &installed)
{
    return source.releaseSequence == installed.releaseSequence
        && source.channel == installed.channel
        && source.application.version == installed.applicationVersion
        && source.application.platform == installed.platform
        && source.application.architecture == installed.architecture
        && source.application.sizeBytes == installed.applicationSizeBytes
        && source.application.sha256 == installed.applicationSha256
        && source.manifest.sha256 == installed.manifestSha256
        && source.manifest.runtime.id == installed.runtimeId
        && source.manifest.runtime.version == installed.runtimeVersion
        && source.manifest.adapter.id == installed.adapterId
        && source.manifest.adapter.version == installed.adapterVersion;
}

QString installedArtifactSetIdentity(const InstalledSetData &installed)
{
    const bool legacy = installed.schemaVersion != kSchema;
    QByteArray payload = legacy
        ? QByteArrayLiteral("aegisy-installed-artifact-set/0.1\n")
        : QByteArrayLiteral("aegisy-installed-artifact-set/0.2\n");
    appendLine(&payload, QByteArrayLiteral("release_sequence"),
               installed.releaseSequence);
    appendLine(&payload, QByteArrayLiteral("channel"), installed.channel);
    appendLine(&payload, QByteArrayLiteral("application.version"),
               installed.applicationVersion);
    appendLine(&payload, QByteArrayLiteral("application.platform"),
               installed.platform);
    appendLine(&payload, QByteArrayLiteral("application.architecture"),
               installed.architecture);
    if (!legacy) {
        appendLine(&payload, QByteArrayLiteral("application.size_bytes"),
                   installed.applicationSizeBytes);
        appendLine(&payload, QByteArrayLiteral("application.sha256"),
                   installed.applicationSha256);
    }
    appendLine(&payload, QByteArrayLiteral("manifest.sha256"),
               installed.manifestSha256);
    appendLine(&payload, QByteArrayLiteral("manifest.runtime.id"),
               installed.runtimeId);
    appendLine(&payload, QByteArrayLiteral("manifest.runtime.version"),
               installed.runtimeVersion);
    appendLine(&payload, QByteArrayLiteral("manifest.adapter.id"),
               installed.adapterId);
    appendLine(&payload, QByteArrayLiteral("manifest.adapter.version"),
               installed.adapterVersion);
    return QStringLiteral("installed-artifact-set:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()));
}

QString legacyInstalledAuthorityIdentity(const QString &receiptIdentity,
                                         const QString &installedIdentity,
                                         const QString &layoutIdentity,
                                         const QByteArray &publicKey)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-installed-artifact-set-authority/0.1\n");
    appendLine(&payload, QByteArrayLiteral("receipt.identity"), receiptIdentity);
    appendLine(&payload, QByteArrayLiteral("installed.identity"), installedIdentity);
    appendLine(&payload, QByteArrayLiteral("installation.layout_identity"),
               layoutIdentity);
    appendLine(&payload, QByteArrayLiteral("verification_key.sha256"),
               QString::fromLatin1(QCryptographicHash::hash(
                   publicKey, QCryptographicHash::Sha256).toHex()));
    return QStringLiteral("installed-artifact-set-authority:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()));
}

QString installedAuthorityIdentity(
    const QString &receiptIdentity,
    const QString &installedIdentity,
    const QString &layoutIdentity,
    const UpdateSigningKeyRing::ArtifactSignatureResult &signature)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-installed-artifact-set-authority/0.2\n");
    appendLine(&payload, QByteArrayLiteral("receipt.identity"), receiptIdentity);
    appendLine(&payload, QByteArrayLiteral("installed.identity"), installedIdentity);
    appendLine(&payload, QByteArrayLiteral("installation.layout_identity"),
               layoutIdentity);
    appendLine(&payload, QByteArrayLiteral("trust_anchor.identity"),
               signature.trustAnchorIdentity);
    appendLine(&payload, QByteArrayLiteral("key_ring.identity"),
               signature.ringIdentity);
    appendLine(&payload, QByteArrayLiteral("key_ring.generation"),
               signature.ringGeneration);
    appendLine(&payload, QByteArrayLiteral("key_ring.authority_identity"),
               signature.ringAuthorityIdentity);
    appendLine(&payload, QByteArrayLiteral("receipt.signer_key_id"),
               signature.signerKeyId);
    appendLine(&payload, QByteArrayLiteral("receipt.signer_key_identity"),
               signature.signerKeyIdentity);
    return QStringLiteral("installed-artifact-set-authority:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()));
}

#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
QString legacyCompatibilityEvaluationIdentity(
    const QString &artifactSetIdentity,
    const QString &installedIdentity,
    const QString &installedAuthorityIdentityValue,
    const QByteArray &publicKey,
    const QString &selectedChannel,
    quint64 acceptedReleaseSequenceHighWater,
    quint64 evaluatedAtMs)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-artifact-set-evaluation/0.1\n");
    appendLine(&payload, QByteArrayLiteral("artifact_set.identity"),
               artifactSetIdentity);
    appendLine(&payload, QByteArrayLiteral("installed.identity"),
               installedIdentity);
    if (!installedAuthorityIdentityValue.isEmpty()) {
        appendLine(&payload, QByteArrayLiteral("installed.authority_identity"),
                   installedAuthorityIdentityValue);
    }
    appendLine(&payload, QByteArrayLiteral("verification_key.sha256"),
               QString::fromLatin1(QCryptographicHash::hash(
                   publicKey, QCryptographicHash::Sha256).toHex()));
    appendLine(&payload, QByteArrayLiteral("selected_channel"), selectedChannel);
    appendLine(&payload, QByteArrayLiteral("accepted_release_sequence_high_water"),
               acceptedReleaseSequenceHighWater);
    appendLine(&payload, QByteArrayLiteral("evaluated_at_ms"), evaluatedAtMs);
    return QStringLiteral("update-artifact-set-evaluation:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()));
}
#endif

QString compatibilityEvaluationIdentity(
    const QString &artifactSetIdentity,
    const QString &payloadIdentity,
    const QString &installedIdentity,
    const QString &installedAuthorityIdentityValue,
    const QString &trustAnchorIdentity,
    const QString &ringIdentity,
    quint64 ringGeneration,
    const QString &ringAuthorityIdentity,
    const QString &receiptSignerKeyId,
    const QString &receiptSignerKeyIdentity,
    const UpdateSigningKeyRing::ArtifactSignatureResult &candidateSignature,
    const QString &selectedChannel,
    quint64 acceptedReleaseSequenceHighWater,
    quint64 evaluatedAtMs)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-artifact-set-evaluation/0.2\n");
    appendLine(&payload, QByteArrayLiteral("artifact_set.identity"),
               artifactSetIdentity);
    appendLine(&payload, QByteArrayLiteral("artifact_set.payload_identity"),
               payloadIdentity);
    appendLine(&payload, QByteArrayLiteral("installed.identity"),
               installedIdentity);
    appendLine(&payload, QByteArrayLiteral("installed.authority_identity"),
               installedAuthorityIdentityValue);
    appendLine(&payload, QByteArrayLiteral("trust_anchor.identity"),
               trustAnchorIdentity);
    appendLine(&payload, QByteArrayLiteral("key_ring.identity"),
               ringIdentity);
    appendLine(&payload, QByteArrayLiteral("key_ring.generation"),
               ringGeneration);
    appendLine(&payload, QByteArrayLiteral("key_ring.authority_identity"),
               ringAuthorityIdentity);
    appendLine(&payload, QByteArrayLiteral("receipt.signer_key_id"),
               receiptSignerKeyId);
    appendLine(&payload, QByteArrayLiteral("receipt.signer_key_identity"),
               receiptSignerKeyIdentity);
    appendLine(&payload, QByteArrayLiteral("candidate.signer_key_id"),
               candidateSignature.signerKeyId);
    appendLine(&payload, QByteArrayLiteral("candidate.signer_key_identity"),
               candidateSignature.signerKeyIdentity);
    appendLine(&payload, QByteArrayLiteral("selected_channel"), selectedChannel);
    appendLine(&payload, QByteArrayLiteral("accepted_release_sequence_high_water"),
               acceptedReleaseSequenceHighWater);
    appendLine(&payload, QByteArrayLiteral("evaluated_at_ms"), evaluatedAtMs);
    return QStringLiteral("update-artifact-set-evaluation:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()));
}

Decision invalidDecision(const QString &errorCode)
{
    Decision decision;
    decision.errorCode = errorCode;
    return decision;
}

bool parseEnvelopeJson(const QByteArray &envelopeJson, QJsonObject *envelope,
                       QString *errorCode)
{
    if (envelopeJson.isEmpty() || envelopeJson.size() > kMaximumEnvelopeBytes) {
        return fail(errorCode, QStringLiteral("artifact-set-json-size-invalid"));
    }
    using namespace aegisy::aap::transport_runtime;
    TransportJsonValue parsed;
    QString parseError;
    if (!parseTransportJsonRaw(envelopeJson, &parsed, &parseError)) {
        return fail(errorCode, QStringLiteral("artifact-set-json-invalid"));
    }
    QJsonValue projected;
    TransportProjectionError projectionError = TransportProjectionError::None;
    if (!projectJsonSafeTransportValue(parsed, &projected, &projectionError)
        || !projected.isObject()) {
        return fail(errorCode, QStringLiteral("artifact-set-json-invalid"));
    }
    *envelope = projected.toObject();
    return true;
}

bool parseVerifiedLegacyCandidate(const QByteArray &envelopeJson,
                                  const QByteArray &publicKeyBase64,
                                  qint64 nowMs,
                                  Candidate *candidate,
                                  QByteArray *publicKey,
                                  QByteArray *payload,
                                  QString *errorCode)
{
    // A canonical Base64 encoding of a 32-byte Ed25519 public key is 44 bytes.
    if (publicKeyBase64.size() != 44) {
        return fail(errorCode, QStringLiteral("artifact-set-public-key-invalid"));
    }
    QJsonObject envelope;
    if (!parseEnvelopeJson(envelopeJson, &envelope, errorCode)
        || !parseLegacyCandidate(envelope, candidate, errorCode)) {
        return false;
    }
    *payload = buildLegacyPayload(*candidate);
    *publicKey = decodeCanonicalBase64(
        QString::fromLatin1(publicKeyBase64), 32);
    const QByteArray signature = decodeCanonicalBase64(candidate->signature, 64);
    if (publicKey->isEmpty()) {
        return fail(errorCode, QStringLiteral("artifact-set-public-key-invalid"));
    }
    if (signature.isEmpty()) {
        return fail(errorCode,
                    QStringLiteral("artifact-set-signature-encoding-invalid"));
    }
    const SignatureVerification verification = verifyEd25519(
        *publicKey, signature, *payload);
    if (verification == SignatureVerification::Unavailable) {
        return fail(errorCode,
                    QStringLiteral("artifact-set-signature-verifier-unavailable"));
    }
    if (verification == SignatureVerification::Invalid) {
        return fail(errorCode, QStringLiteral("artifact-set-signature-invalid"));
    }
    if (nowMs <= 0
        || static_cast<quint64>(nowMs)
            > static_cast<quint64>(kMaximumSafeJsonInteger)) {
        return fail(errorCode, QStringLiteral("artifact-set-clock-invalid"));
    }
    if (candidate->publishedAtMs
        > static_cast<quint64>(nowMs + kMaximumClockSkewMs)) {
        return fail(errorCode,
                    QStringLiteral("artifact-set-published-time-invalid"));
    }
    if (errorCode) errorCode->clear();
    return true;
}

bool parseVerifiedCandidate(
    const QByteArray &envelopeJson,
    const UpdateSigningKeyRing::Authority &signingAuthority,
    qint64 nowMs,
    bool requireCurrentlyActive,
    Candidate *candidate,
    QByteArray *payload,
    UpdateSigningKeyRing::ArtifactSignatureResult *signature,
    QString *errorCode)
{
    if (nowMs <= 0
        || static_cast<quint64>(nowMs)
            > static_cast<quint64>(kMaximumSafeJsonInteger)) {
        return fail(errorCode, QStringLiteral("artifact-set-clock-invalid"));
    }
    QJsonObject envelope;
    if (!parseEnvelopeJson(envelopeJson, &envelope, errorCode)
        || !parseCandidate(envelope, candidate, false, errorCode)) {
        return false;
    }
    const QString expectedPayloadIdentity = candidatePayloadIdentity(*candidate);
    if (candidate->payloadIdentity != expectedPayloadIdentity) {
        return fail(errorCode,
                    QStringLiteral("artifact-set-payload-identity-invalid"));
    }
    if (candidate->signedAtMs
        > static_cast<quint64>(nowMs + kMaximumClockSkewMs)) {
        return fail(errorCode,
                    QStringLiteral("artifact-set-signed-time-invalid"));
    }
    if (requireCurrentlyActive
        && static_cast<quint64>(nowMs) >= candidate->expiresAtMs) {
        return fail(errorCode, QStringLiteral("artifact-set-expired"));
    }
    *payload = buildPayload(*candidate);
    *signature = UpdateSigningKeyRing::verifyArtifactSetSignature(
        signingAuthority, candidate->signingKeyId, candidate->signedAtMs,
        static_cast<quint64>(nowMs), requireCurrentlyActive, *payload,
        candidate->signature);
    if (!signature->ok) return fail(errorCode, signature->errorCode);
    if (errorCode) errorCode->clear();
    return true;
}

#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
Decision evaluateLegacyCandidate(
    const Candidate &candidate,
    const QByteArray &payload,
    const QByteArray &publicKey,
    qint64 nowMs,
    const InstalledSetData &installed,
    const QString &installedAuthorityIdentityValue,
    const QString &selectedChannel,
    quint64 acceptedReleaseSequenceHighWater)
{
    if (!validLegacyInstalled(installed)) {
        return invalidDecision(QStringLiteral("installed-artifact-set-invalid"));
    }
    if (!validChannel(selectedChannel)) {
        return invalidDecision(QStringLiteral("selected-update-channel-invalid"));
    }
    if (acceptedReleaseSequenceHighWater < installed.releaseSequence
        || acceptedReleaseSequenceHighWater
            > static_cast<quint64>(kMaximumSafeJsonInteger)) {
        return invalidDecision(QStringLiteral("artifact-set-high-water-invalid"));
    }

    Decision decision;
    decision.state = State::Incompatible;
    decision.artifactSetIdentity = QStringLiteral("update-artifact-set:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()));
    decision.installedArtifactSetIdentity = installedArtifactSetIdentity(installed);
    decision.installedAuthorityIdentity = installedAuthorityIdentityValue;
    decision.evaluatedSelectedChannel = selectedChannel;
    decision.evaluatedAcceptedReleaseSequenceHighWater =
        acceptedReleaseSequenceHighWater;
    decision.evaluatedAtMs = static_cast<quint64>(nowMs);
    decision.compatibilityEvaluationIdentity =
        legacyCompatibilityEvaluationIdentity(
        decision.artifactSetIdentity, decision.installedArtifactSetIdentity,
        installedAuthorityIdentityValue, publicKey, selectedChannel,
        acceptedReleaseSequenceHighWater, decision.evaluatedAtMs);
    decision.targetReleaseSequence = candidate.releaseSequence;
    decision.targetChannel = candidate.channel;
    decision.targetApplicationVersion = candidate.application.version;
    decision.platform = candidate.application.platform;
    decision.architecture = candidate.application.architecture;
    decision.installerUrl = candidate.installer.url;
    decision.installerFileName = candidate.installer.fileName;
    decision.installerSizeBytes = candidate.installer.sizeBytes;
    decision.installerSha256 = candidate.installer.sha256;
    decision.installerSparkleSignature = candidate.installer.sparkleSignature;
    decision.targetManifestSha256 = candidate.manifest.sha256;
    decision.targetRuntimeId = candidate.manifest.runtime.id;
    decision.targetRuntimeVersion = candidate.manifest.runtime.version;
    decision.targetAdapterId = candidate.manifest.adapter.id;
    decision.targetAdapterVersion = candidate.manifest.adapter.version;

    if (candidate.channel != selectedChannel) {
        decision.errorCode = QStringLiteral("artifact-set-channel-incompatible");
        return decision;
    }
    if (candidate.application.platform != installed.platform
        || candidate.application.architecture != installed.architecture) {
        decision.errorCode = QStringLiteral("artifact-set-platform-incompatible");
        return decision;
    }
    if (candidate.releaseSequence <= installed.releaseSequence) {
        decision.errorCode = QStringLiteral("artifact-set-not-newer");
        return decision;
    }
    if (candidate.releaseSequence <= acceptedReleaseSequenceHighWater) {
        decision.errorCode = QStringLiteral("artifact-set-sequence-replay");
        return decision;
    }
    for (const Source &source : candidate.sources) {
        if (matchesInstalled(source, installed)) {
            decision.state = State::Compatible;
            decision.candidateCompatible = true;
            decision.matchedSourceReleaseSequence = source.releaseSequence;
            return decision;
        }
    }
    decision.errorCode = QStringLiteral("artifact-set-source-incompatible");
    return decision;
}
#endif

Decision evaluateCandidate(
    const Candidate &candidate,
    const QByteArray &payload,
    const UpdateSigningKeyRing::ArtifactSignatureResult &candidateSignature,
    qint64 nowMs,
    const InstalledSetData &installed,
    const QString &installedAuthorityIdentityValue,
    const QString &trustAnchorIdentity,
    const QString &ringIdentity,
    quint64 ringGeneration,
    const QString &ringAuthorityIdentity,
    const QString &receiptSignerKeyId,
    const QString &receiptSignerKeyIdentity,
    const QString &selectedChannel,
    quint64 acceptedReleaseSequenceHighWater)
{
    if (!validInstalled(installed)) {
        return invalidDecision(QStringLiteral("installed-artifact-set-invalid"));
    }
    if (!validChannel(selectedChannel)) {
        return invalidDecision(QStringLiteral("selected-update-channel-invalid"));
    }
    if (acceptedReleaseSequenceHighWater < installed.releaseSequence
        || acceptedReleaseSequenceHighWater
            > static_cast<quint64>(kMaximumSafeJsonInteger)) {
        return invalidDecision(QStringLiteral("artifact-set-high-water-invalid"));
    }

    Decision decision;
    decision.state = State::Incompatible;
    decision.artifactSetIdentity = QStringLiteral("update-artifact-set:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()));
    decision.installedArtifactSetIdentity = installedArtifactSetIdentity(installed);
    decision.installedAuthorityIdentity = installedAuthorityIdentityValue;
    decision.evaluatedSelectedChannel = selectedChannel;
    decision.evaluatedAcceptedReleaseSequenceHighWater =
        acceptedReleaseSequenceHighWater;
    decision.evaluatedAtMs = static_cast<quint64>(nowMs);
    decision.payloadIdentity = candidate.payloadIdentity;
    decision.signingKeyId = candidateSignature.signerKeyId;
    decision.signerKeyIdentity = candidateSignature.signerKeyIdentity;
    decision.signingTrustAnchorIdentity =
        candidateSignature.trustAnchorIdentity;
    decision.signingRingIdentity = candidateSignature.ringIdentity;
    decision.signingRingGeneration = candidateSignature.ringGeneration;
    decision.signingRingAuthorityIdentity =
        candidateSignature.ringAuthorityIdentity;
    decision.signedAtMs = candidate.signedAtMs;
    decision.expiresAtMs = candidate.expiresAtMs;
    decision.compatibilityEvaluationIdentity = compatibilityEvaluationIdentity(
        decision.artifactSetIdentity, decision.payloadIdentity,
        decision.installedArtifactSetIdentity, installedAuthorityIdentityValue,
        trustAnchorIdentity, ringIdentity, ringGeneration, ringAuthorityIdentity,
        receiptSignerKeyId, receiptSignerKeyIdentity, candidateSignature,
        selectedChannel, acceptedReleaseSequenceHighWater,
        decision.evaluatedAtMs);
    decision.targetReleaseSequence = candidate.releaseSequence;
    decision.targetChannel = candidate.channel;
    decision.targetApplicationVersion = candidate.application.version;
    decision.targetApplicationSizeBytes = candidate.application.sizeBytes;
    decision.targetApplicationSha256 = candidate.application.sha256;
    decision.platform = candidate.application.platform;
    decision.architecture = candidate.application.architecture;
    decision.installerUrl = candidate.installer.url;
    decision.installerFileName = candidate.installer.fileName;
    decision.installerSizeBytes = candidate.installer.sizeBytes;
    decision.installerSha256 = candidate.installer.sha256;
    decision.installerSparkleSignature = candidate.installer.sparkleSignature;
    decision.targetManifestSha256 = candidate.manifest.sha256;
    decision.targetRuntimeId = candidate.manifest.runtime.id;
    decision.targetRuntimeVersion = candidate.manifest.runtime.version;
    decision.targetAdapterId = candidate.manifest.adapter.id;
    decision.targetAdapterVersion = candidate.manifest.adapter.version;

    if (candidateSignature.trustAnchorIdentity != trustAnchorIdentity
        || candidateSignature.ringIdentity != ringIdentity
        || candidateSignature.ringGeneration != ringGeneration
        || candidateSignature.ringAuthorityIdentity != ringAuthorityIdentity) {
        return invalidDecision(
            QStringLiteral("artifact-set-signing-authority-drift"));
    }
    if (candidate.channel != selectedChannel) {
        decision.errorCode = QStringLiteral("artifact-set-channel-incompatible");
        return decision;
    }
    if (candidate.application.platform != installed.platform
        || candidate.application.architecture != installed.architecture) {
        decision.errorCode = QStringLiteral("artifact-set-platform-incompatible");
        return decision;
    }
    if (candidate.releaseSequence <= installed.releaseSequence) {
        decision.errorCode = QStringLiteral("artifact-set-not-newer");
        return decision;
    }
    if (candidate.releaseSequence <= acceptedReleaseSequenceHighWater) {
        decision.errorCode = QStringLiteral("artifact-set-sequence-replay");
        return decision;
    }
    for (const Source &source : candidate.sources) {
        if (matchesInstalled(source, installed)) {
            decision.state = State::Compatible;
            decision.candidateCompatible = true;
            decision.matchedSourceReleaseSequence = source.releaseSequence;
            return decision;
        }
    }
    decision.errorCode = QStringLiteral("artifact-set-source-incompatible");
    return decision;
}

} // namespace

#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
namespace Testing {

QByteArray signaturePayload(const QJsonObject &envelope, QString *errorCode)
{
    const QString schema = envelope.value(QStringLiteral("schema_version"))
        .toString();
    Candidate candidate;
    if (schema == kLegacySchema) {
        if (!parseLegacyCandidate(envelope, &candidate, errorCode)) return {};
        if (errorCode) errorCode->clear();
        return buildLegacyPayload(candidate);
    }
    if (!parseCandidate(envelope, &candidate, false, errorCode)) return {};
    if (errorCode) errorCode->clear();
    return buildPayload(candidate);
}

QString payloadIdentity(const QJsonObject &envelope, QString *errorCode)
{
    Candidate candidate;
    if (!parseCandidate(envelope, &candidate, true, errorCode)) return {};
    if (errorCode) errorCode->clear();
    return candidatePayloadIdentity(candidate);
}

} // namespace Testing
#endif

class InstalledArtifactSetAuthority::Verifier
{
public:
    static InstalledAuthorityResult verify(
        const InstallationLayout &layout,
        const UpdateSigningKeyRing::Authority &signingAuthority,
        qint64 nowMs,
        const QString &expectedApplicationVersion,
        const QString &expectedChannel,
        const QString &expectedPlatform,
        const QString &expectedArchitecture)
    {
        return verifyImpl(
            layout, &signingAuthority, QByteArray(), nowMs,
            expectedApplicationVersion, expectedChannel, expectedPlatform,
            expectedArchitecture);
    }

#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
    static InstalledAuthorityResult verifyLegacy(
        const InstallationLayout &layout,
        const QByteArray &publicKeyBase64,
        qint64 nowMs,
        const QString &expectedApplicationVersion,
        const QString &expectedChannel,
        const QString &expectedPlatform,
        const QString &expectedArchitecture)
    {
        return verifyImpl(
            layout, nullptr, publicKeyBase64, nowMs,
            expectedApplicationVersion, expectedChannel, expectedPlatform,
            expectedArchitecture);
    }
#endif

private:
    static InstalledAuthorityResult verifyImpl(
        const InstallationLayout &layout,
        const UpdateSigningKeyRing::Authority *signingAuthority,
        const QByteArray &legacyPublicKeyBase64,
        qint64 nowMs,
        const QString &expectedApplicationVersion,
        const QString &expectedChannel,
        const QString &expectedPlatform,
        const QString &expectedArchitecture)
    {
        InstalledAuthorityResult result;
        const auto reject = [&result](const QString &errorCode) {
            result.ok = false;
            result.authority = InstalledArtifactSetAuthority{};
            result.errorCode = errorCode;
            return result;
        };
        if (!validVersion(expectedApplicationVersion)
            || !validChannel(expectedChannel)
            || !validPlatformArchitecture(expectedPlatform,
                                            expectedArchitecture)) {
            return reject(QStringLiteral(
                "installed-authority-expectation-invalid"));
        }
        LayoutPathObservation applicationObservation;
        LayoutPathObservation installationRootObservation;
        LayoutPathObservation artifactRootObservation;
        if (!observeLayoutApplication(layout.applicationPath,
                                      &applicationObservation)
            || !observeLayoutDirectory(layout.installationRoot,
                                       &installationRootObservation)
            || !observeLayoutDirectory(layout.artifactRoot,
                                       &artifactRootObservation)) {
            return reject(QStringLiteral("installed-authority-layout-invalid"));
        }
        const QString artifactRoot = artifactRootObservation.canonicalPath;
        const QString installationRoot =
            installationRootObservation.canonicalPath;
        const QString applicationCanonical = applicationObservation.canonicalPath;
        const QString expectedReceiptPath = QDir(artifactRoot).filePath(
            kInstalledReceiptFileName);
        const QString expectedManifestPath = QDir(artifactRoot).filePath(
            kArtifactManifestFileName);
        const QString expectedRuntimePath = QDir(artifactRoot).filePath(
            runtimeFileName());
        if ((!samePath(artifactRoot, installationRoot)
             && !pathWithin(installationRoot, artifactRoot))
            || !pathWithin(installationRoot, applicationCanonical)
            || samePath(applicationCanonical, expectedReceiptPath)
            || samePath(applicationCanonical, expectedManifestPath)
            || samePath(applicationCanonical, expectedRuntimePath)
            || QFileInfo(layout.receiptPath).absoluteFilePath()
                != QFileInfo(expectedReceiptPath).absoluteFilePath()
            || QFileInfo(layout.manifestPath).absoluteFilePath()
                != QFileInfo(expectedManifestPath).absoluteFilePath()
            || QFileInfo(layout.runtimePath).absoluteFilePath()
                != QFileInfo(expectedRuntimePath).absoluteFilePath()) {
            return reject(QStringLiteral("installed-authority-layout-invalid"));
        }

        const QFileInfo receiptInfo(expectedReceiptPath);
        const QFileInfo manifestInfo(expectedManifestPath);
        if (receiptInfo.fileName() != kInstalledReceiptFileName
            || manifestInfo.fileName() != kArtifactManifestFileName
            || !receiptInfo.isFile() || !manifestInfo.isFile()
            || receiptInfo.isSymLink() || manifestInfo.isSymLink()
            || receiptInfo.canonicalFilePath().isEmpty()
            || manifestInfo.canonicalFilePath().isEmpty()
            || QFileInfo(receiptInfo.absolutePath()).canonicalFilePath()
                != QFileInfo(manifestInfo.absolutePath()).canonicalFilePath()) {
            return reject(QStringLiteral("installed-authority-path-invalid"));
        }
        NativePathMetadata receiptMetadata;
        NativePathMetadata manifestMetadata;
        if (!inspectNativePath(receiptInfo.absoluteFilePath(), false,
                               &receiptMetadata)
            || !inspectNativePath(manifestInfo.absoluteFilePath(), false,
                                  &manifestMetadata)) {
            return reject(QStringLiteral("installed-authority-path-invalid"));
        }
        const QString receiptCanonical = receiptInfo.canonicalFilePath();
        const QString manifestCanonical = manifestInfo.canonicalFilePath();
        const QFileInfo runtimeInfo(expectedRuntimePath);
        const QString runtimeCanonical = runtimeInfo.canonicalFilePath();
        if (!runtimeInfo.isFile() || !runtimeInfo.isReadable()
            || runtimeInfo.isSymLink() || runtimeCanonical.isEmpty()) {
            return reject(QStringLiteral("installed-authority-path-invalid"));
        }
        QFile receiptFile(receiptCanonical);
        if (!receiptFile.open(QIODevice::ReadOnly)) {
            return reject(QStringLiteral("installed-receipt-unreadable"));
        }
        NativePathMetadata receiptOpened;
        if (!inspectOpenNativeFile(receiptFile, &receiptOpened)
            || receiptOpened.identity != receiptMetadata.identity
            || receiptOpened.sizeBytes != receiptMetadata.sizeBytes) {
            return reject(QStringLiteral("installed-receipt-path-drift"));
        }
        const qint64 receiptSize = receiptFile.size();
        if (receiptSize <= 0 || receiptSize > kMaximumEnvelopeBytes) {
            return reject(QStringLiteral("installed-receipt-size-invalid"));
        }
        const QByteArray receiptBytes = receiptFile.read(
            kMaximumEnvelopeBytes + 1);
        if (receiptBytes.size() != receiptSize
            || receiptFile.error() != QFile::NoError || !receiptFile.atEnd()) {
            return reject(QStringLiteral("installed-receipt-read-failed"));
        }
        NativePathMetadata receiptAfter;
        if (!inspectNativePath(receiptCanonical, false, &receiptAfter)
            || receiptAfter.identity != receiptMetadata.identity
            || receiptAfter.sizeBytes != receiptMetadata.sizeBytes
            || static_cast<quint64>(receiptSize) != receiptMetadata.sizeBytes
            || QFileInfo(receiptCanonical).canonicalFilePath()
                != receiptCanonical) {
            return reject(QStringLiteral("installed-receipt-path-drift"));
        }
        Candidate candidate;
        QByteArray publicKey;
        QByteArray payload;
        UpdateSigningKeyRing::ArtifactSignatureResult receiptSignature;
        QString errorCode;
        const bool legacy = signingAuthority == nullptr;
        const bool verified = legacy
            ? parseVerifiedLegacyCandidate(
                receiptBytes, legacyPublicKeyBase64, nowMs, &candidate,
                &publicKey, &payload, &errorCode)
            : parseVerifiedCandidate(
                receiptBytes, *signingAuthority, nowMs, false, &candidate,
                &payload, &receiptSignature, &errorCode);
        if (!verified) {
            return reject(QStringLiteral("installed-receipt-") + errorCode);
        }
        if (candidate.application.version != expectedApplicationVersion
            || candidate.channel != expectedChannel
            || candidate.application.platform != expectedPlatform
            || candidate.application.architecture != expectedArchitecture) {
            return reject(QStringLiteral("installed-receipt-target-mismatch"));
        }
        if (!legacy
            && (candidate.application.sizeBytes
                    != applicationObservation.sizeBytes
                || candidate.application.sha256
                    != applicationObservation.sha256)) {
            return reject(QStringLiteral(
                "installed-receipt-application-mismatch"));
        }
        const ArtifactManifest::VerificationResult manifest =
            ArtifactManifest::verifyFile(manifestCanonical, runtimeCanonical);
        if (!manifest.ok) {
            return reject(QStringLiteral("installed-manifest-invalid"));
        }
        if (manifest.manifestPath != manifestCanonical
            || manifest.manifestFileIdentity != manifestMetadata.identity
            || manifest.manifestSizeBytes != manifestMetadata.sizeBytes
            || manifest.runtimePath != runtimeCanonical
            || manifest.manifestSha256 != candidate.manifest.sha256
            || manifest.runtimeId != candidate.manifest.runtime.id
            || manifest.runtimeVersion != candidate.manifest.runtime.version
            || manifest.adapterId != candidate.manifest.adapter.id
            || manifest.adapterVersion != candidate.manifest.adapter.version) {
            return reject(QStringLiteral("installed-manifest-mismatch"));
        }

        LayoutPathObservation finalApplicationObservation;
        LayoutPathObservation finalInstallationRootObservation;
        LayoutPathObservation finalArtifactRootObservation;
        if (!observeLayoutApplication(applicationCanonical,
                                      &finalApplicationObservation)
            || !observeLayoutDirectory(installationRoot,
                                       &finalInstallationRootObservation)
            || !observeLayoutDirectory(artifactRoot,
                                       &finalArtifactRootObservation)
            || !samePath(finalApplicationObservation.canonicalPath,
                         applicationCanonical)
            || finalApplicationObservation.fileIdentity
                != applicationObservation.fileIdentity
            || finalApplicationObservation.sha256
                != applicationObservation.sha256
            || finalApplicationObservation.sizeBytes
                != applicationObservation.sizeBytes
            || finalInstallationRootObservation.fileIdentity
                != installationRootObservation.fileIdentity
            || finalArtifactRootObservation.fileIdentity
                != artifactRootObservation.fileIdentity) {
            return reject(QStringLiteral("installed-authority-layout-drift"));
        }

        InstalledSetData installed;
        installed.schemaVersion = candidate.schemaVersion;
        installed.releaseSequence = candidate.releaseSequence;
        installed.channel = candidate.channel;
        installed.applicationVersion = candidate.application.version;
        installed.platform = candidate.application.platform;
        installed.architecture = candidate.application.architecture;
        installed.applicationSizeBytes = candidate.application.sizeBytes;
        installed.applicationSha256 = candidate.application.sha256;
        installed.manifestSha256 = candidate.manifest.sha256;
        installed.runtimeId = candidate.manifest.runtime.id;
        installed.runtimeVersion = candidate.manifest.runtime.version;
        installed.adapterId = candidate.manifest.adapter.id;
        installed.adapterVersion = candidate.manifest.adapter.version;
        const QString receiptIdentity = QStringLiteral(
            "update-artifact-set:sha256:%1")
            .arg(QString::fromLatin1(QCryptographicHash::hash(
                payload, QCryptographicHash::Sha256).toHex()));
        const QString installedIdentity = installedArtifactSetIdentity(installed);

        result.authority.m_valid = true;
        result.authority.m_schemaVersion = installed.schemaVersion;
        result.authority.m_releaseSequence = installed.releaseSequence;
        result.authority.m_channel = installed.channel;
        result.authority.m_applicationVersion = installed.applicationVersion;
        result.authority.m_platform = installed.platform;
        result.authority.m_architecture = installed.architecture;
        result.authority.m_applicationSizeBytes =
            installed.applicationSizeBytes;
        result.authority.m_applicationSha256 = installed.applicationSha256;
        result.authority.m_manifestSha256 = installed.manifestSha256;
        result.authority.m_runtimeId = installed.runtimeId;
        result.authority.m_runtimeVersion = installed.runtimeVersion;
        result.authority.m_adapterId = installed.adapterId;
        result.authority.m_adapterVersion = installed.adapterVersion;
        result.authority.m_receiptIdentity = receiptIdentity;
        result.authority.m_installedArtifactSetIdentity = installedIdentity;
        InstallationLayout verifiedLayout = layout;
        verifiedLayout.applicationPath = applicationCanonical;
        verifiedLayout.installationRoot = installationRoot;
        verifiedLayout.artifactRoot = artifactRoot;
        verifiedLayout.receiptPath = receiptCanonical;
        verifiedLayout.manifestPath = manifestCanonical;
        verifiedLayout.runtimePath = runtimeCanonical;
        verifiedLayout.adapterPath = manifest.adapterPath;
        verifiedLayout.applicationFileIdentity =
            finalApplicationObservation.fileIdentity;
        verifiedLayout.applicationSha256 = finalApplicationObservation.sha256;
        verifiedLayout.applicationSizeBytes =
            finalApplicationObservation.sizeBytes;
        verifiedLayout.installationRootFileIdentity =
            finalInstallationRootObservation.fileIdentity;
        verifiedLayout.artifactRootFileIdentity =
            finalArtifactRootObservation.fileIdentity;
        verifiedLayout.receiptFileIdentity = receiptAfter.identity;
        verifiedLayout.receiptSha256 = QString::fromLatin1(
            QCryptographicHash::hash(receiptBytes,
                                     QCryptographicHash::Sha256).toHex());
        verifiedLayout.receiptSizeBytes = receiptAfter.sizeBytes;
        verifiedLayout.manifestFileIdentity = manifest.manifestFileIdentity;
        verifiedLayout.manifestSha256 = manifest.manifestSha256;
        verifiedLayout.manifestSizeBytes = manifest.manifestSizeBytes;
        verifiedLayout.runtimeFileIdentity = manifest.runtimeFileIdentity;
        verifiedLayout.runtimeSha256 = manifest.runtimeSha256;
        verifiedLayout.runtimeSizeBytes = manifest.runtimeSizeBytes;
        verifiedLayout.adapterFileIdentity = manifest.adapterFileIdentity;
        verifiedLayout.adapterSha256 = manifest.adapterSha256;
        verifiedLayout.adapterSizeBytes = manifest.adapterSizeBytes;
        const QString layoutIdentity = installationLayoutIdentity(verifiedLayout);
        if (layoutIdentity.isEmpty()) {
            return reject(QStringLiteral("installed-authority-layout-invalid"));
        }
        result.authority.m_installationLayoutIdentity = layoutIdentity;
        if (legacy) {
            result.authority.m_authorityIdentity =
                legacyInstalledAuthorityIdentity(
                    receiptIdentity, installedIdentity, layoutIdentity,
                    publicKey);
        } else {
            result.authority.m_trustAnchorIdentity =
                receiptSignature.trustAnchorIdentity;
            result.authority.m_ringIdentity = receiptSignature.ringIdentity;
            result.authority.m_ringGeneration = receiptSignature.ringGeneration;
            result.authority.m_ringAuthorityIdentity =
                receiptSignature.ringAuthorityIdentity;
            result.authority.m_receiptSignerKeyId =
                receiptSignature.signerKeyId;
            result.authority.m_receiptSignerKeyIdentity =
                receiptSignature.signerKeyIdentity;
            result.authority.m_authorityIdentity = installedAuthorityIdentity(
                receiptIdentity, installedIdentity, layoutIdentity,
                receiptSignature);
        }
        result.authority.m_applicationPath = applicationCanonical;
        result.authority.m_installationRoot = installationRoot;
        result.authority.m_receiptPath = receiptCanonical;
        result.authority.m_manifestPath = manifestCanonical;
        result.authority.m_runtimePath = runtimeCanonical;
#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
        result.authority.m_testOnlyLayout = verifiedLayout.testOnly;
#endif
        result.ok = true;
        return result;
    }
};

InstalledAuthorityResult verifyCurrentInstallationAuthority(
    const UpdateSigningKeyRing::Authority &signingAuthority,
    qint64 nowMs,
    const QString &expectedChannel)
{
    InstallationLayout layout;
    QString errorCode;
    if (!deriveCurrentInstallationLayout(&layout, &errorCode)) {
        InstalledAuthorityResult result;
        result.errorCode = errorCode;
        return result;
    }
    QString platform;
    QString architecture;
    const QString applicationVersion = QCoreApplication::applicationVersion();
    if (!currentReleaseTarget(&platform, &architecture)
        || !validVersion(applicationVersion)) {
        InstalledAuthorityResult result;
        result.errorCode = QStringLiteral(
            "installed-authority-application-invalid");
        return result;
    }
    return InstalledArtifactSetAuthority::Verifier::verify(
        layout, signingAuthority, nowMs, applicationVersion,
        expectedChannel, platform, architecture);
}

#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
InstalledAuthorityResult verifyCurrentInstallationAuthority(
    const QByteArray &publicKeyBase64,
    qint64 nowMs,
    const QString &expectedChannel)
{
    InstallationLayout layout;
    QString errorCode;
    if (!deriveCurrentInstallationLayout(&layout, &errorCode)) {
        InstalledAuthorityResult result;
        result.errorCode = errorCode;
        return result;
    }
    QString platform;
    QString architecture;
    const QString applicationVersion = QCoreApplication::applicationVersion();
    if (!currentReleaseTarget(&platform, &architecture)
        || !validVersion(applicationVersion)) {
        InstalledAuthorityResult result;
        result.errorCode = QStringLiteral(
            "installed-authority-application-invalid");
        return result;
    }
    return InstalledArtifactSetAuthority::Verifier::verifyLegacy(
        layout, publicKeyBase64, nowMs, applicationVersion,
        expectedChannel, platform, architecture);
}
#endif

Decision verifyCandidate(const QByteArray &envelopeJson,
                         const UpdateSigningKeyRing::Authority &signingAuthority,
                         qint64 nowMs,
                         const InstalledArtifactSetAuthority &authority,
                         const QString &selectedChannel,
                         quint64 acceptedReleaseSequenceHighWater)
{
    if (!authority.m_valid || authority.m_schemaVersion != kSchema
        || !signingAuthority.isValid()
        || authority.m_trustAnchorIdentity
            != signingAuthority.trustAnchorIdentity()
        || authority.m_ringIdentity != signingAuthority.ringIdentity()
        || authority.m_ringGeneration != signingAuthority.generation()
        || authority.m_ringAuthorityIdentity
            != signingAuthority.authorityIdentity()) {
        return invalidDecision(QStringLiteral("installed-artifact-authority-invalid"));
    }
    InstallationLayout layout;
#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
    if (authority.m_testOnlyLayout) {
        layout.applicationPath = authority.m_applicationPath;
        layout.installationRoot = authority.m_installationRoot;
        layout.artifactRoot = QFileInfo(authority.m_receiptPath).absolutePath();
        layout.receiptPath = authority.m_receiptPath;
        layout.manifestPath = authority.m_manifestPath;
        layout.runtimePath = authority.m_runtimePath;
        layout.testOnly = true;
    } else
#endif
    {
        QString layoutError;
        if (!deriveCurrentInstallationLayout(&layout, &layoutError)
            || QCoreApplication::applicationVersion()
                != authority.m_applicationVersion) {
            return invalidDecision(
                QStringLiteral("installed-artifact-authority-invalid"));
        }
    }
    const InstalledAuthorityResult refreshed =
        InstalledArtifactSetAuthority::Verifier::verify(
            layout, signingAuthority, nowMs, authority.m_applicationVersion,
            authority.m_channel, authority.m_platform, authority.m_architecture);
    if (!refreshed.ok
        || refreshed.authority.m_authorityIdentity != authority.m_authorityIdentity) {
        return invalidDecision(QStringLiteral("installed-artifact-authority-invalid"));
    }
    const InstalledArtifactSetAuthority &current = refreshed.authority;
    InstalledSetData installed;
    installed.schemaVersion = current.m_schemaVersion;
    installed.releaseSequence = current.m_releaseSequence;
    installed.channel = current.m_channel;
    installed.applicationVersion = current.m_applicationVersion;
    installed.platform = current.m_platform;
    installed.architecture = current.m_architecture;
    installed.applicationSizeBytes = current.m_applicationSizeBytes;
    installed.applicationSha256 = current.m_applicationSha256;
    installed.manifestSha256 = current.m_manifestSha256;
    installed.runtimeId = current.m_runtimeId;
    installed.runtimeVersion = current.m_runtimeVersion;
    installed.adapterId = current.m_adapterId;
    installed.adapterVersion = current.m_adapterVersion;
    const QString installedIdentity = installedArtifactSetIdentity(installed);
    if (!validInstalled(installed)
        || current.m_installedArtifactSetIdentity != installedIdentity
        || current.m_installationLayoutIdentity.isEmpty()
        || current.m_trustAnchorIdentity
            != signingAuthority.trustAnchorIdentity()
        || current.m_ringIdentity != signingAuthority.ringIdentity()
        || current.m_ringGeneration != signingAuthority.generation()
        || current.m_ringAuthorityIdentity
            != signingAuthority.authorityIdentity()
        || current.m_receiptSignerKeyId.isEmpty()
        || current.m_receiptSignerKeyIdentity.isEmpty()) {
        return invalidDecision(QStringLiteral("installed-artifact-authority-invalid"));
    }

    Candidate candidate;
    QByteArray payload;
    UpdateSigningKeyRing::ArtifactSignatureResult candidateSignature;
    QString errorCode;
    if (!parseVerifiedCandidate(
            envelopeJson, signingAuthority, nowMs, true, &candidate,
            &payload, &candidateSignature, &errorCode)) {
        return invalidDecision(errorCode);
    }
    return evaluateCandidate(
        candidate, payload, candidateSignature, nowMs, installed,
        current.m_authorityIdentity, current.m_trustAnchorIdentity,
        current.m_ringIdentity, current.m_ringGeneration,
        current.m_ringAuthorityIdentity, current.m_receiptSignerKeyId,
        current.m_receiptSignerKeyIdentity, selectedChannel,
        acceptedReleaseSequenceHighWater);
}

#ifdef AEGISY_UPDATE_ARTIFACT_SET_TESTING
Decision verifyCandidate(const QByteArray &envelopeJson,
                         const QByteArray &publicKeyBase64,
                         qint64 nowMs,
                         const InstalledArtifactSetAuthority &authority,
                         const QString &selectedChannel,
                         quint64 acceptedReleaseSequenceHighWater)
{
    if (!authority.m_valid || authority.m_schemaVersion != kLegacySchema) {
        return invalidDecision(QStringLiteral(
            "installed-artifact-authority-invalid"));
    }
    InstallationLayout layout;
    if (authority.m_testOnlyLayout) {
        layout.applicationPath = authority.m_applicationPath;
        layout.installationRoot = authority.m_installationRoot;
        layout.artifactRoot = QFileInfo(authority.m_receiptPath).absolutePath();
        layout.receiptPath = authority.m_receiptPath;
        layout.manifestPath = authority.m_manifestPath;
        layout.runtimePath = authority.m_runtimePath;
        layout.testOnly = true;
    } else {
        QString layoutError;
        if (!deriveCurrentInstallationLayout(&layout, &layoutError)
            || QCoreApplication::applicationVersion()
                != authority.m_applicationVersion) {
            return invalidDecision(QStringLiteral(
                "installed-artifact-authority-invalid"));
        }
    }
    const InstalledAuthorityResult refreshed =
        InstalledArtifactSetAuthority::Verifier::verifyLegacy(
            layout, publicKeyBase64, nowMs, authority.m_applicationVersion,
            authority.m_channel, authority.m_platform,
            authority.m_architecture);
    if (!refreshed.ok
        || refreshed.authority.m_authorityIdentity
            != authority.m_authorityIdentity) {
        return invalidDecision(QStringLiteral(
            "installed-artifact-authority-invalid"));
    }
    const InstalledArtifactSetAuthority &current = refreshed.authority;
    const QByteArray publicKey = decodeCanonicalBase64(
        QString::fromLatin1(publicKeyBase64), 32);
    InstalledSetData installed;
    installed.schemaVersion = kLegacySchema;
    installed.releaseSequence = current.m_releaseSequence;
    installed.channel = current.m_channel;
    installed.applicationVersion = current.m_applicationVersion;
    installed.platform = current.m_platform;
    installed.architecture = current.m_architecture;
    installed.manifestSha256 = current.m_manifestSha256;
    installed.runtimeId = current.m_runtimeId;
    installed.runtimeVersion = current.m_runtimeVersion;
    installed.adapterId = current.m_adapterId;
    installed.adapterVersion = current.m_adapterVersion;
    const QString installedIdentity = installedArtifactSetIdentity(installed);
    if (publicKey.isEmpty() || !validLegacyInstalled(installed)
        || current.m_installedArtifactSetIdentity != installedIdentity
        || current.m_installationLayoutIdentity.isEmpty()
        || current.m_authorityIdentity != legacyInstalledAuthorityIdentity(
            current.m_receiptIdentity, installedIdentity,
            current.m_installationLayoutIdentity, publicKey)) {
        return invalidDecision(QStringLiteral(
            "installed-artifact-authority-invalid"));
    }

    Candidate candidate;
    QByteArray verifiedPublicKey;
    QByteArray payload;
    QString errorCode;
    if (!parseVerifiedLegacyCandidate(
            envelopeJson, publicKeyBase64, nowMs, &candidate,
            &verifiedPublicKey, &payload, &errorCode)) {
        return invalidDecision(errorCode);
    }
    return evaluateLegacyCandidate(
        candidate, payload, verifiedPublicKey, nowMs, installed,
        current.m_authorityIdentity, selectedChannel,
        acceptedReleaseSequenceHighWater);
}

namespace Testing {

Decision verifyCandidateWithUntrustedInputs(
    const QByteArray &envelopeJson,
    const QByteArray &publicKeyBase64,
    qint64 nowMs,
    const InstalledArtifactSet &untrusted,
    const QString &selectedChannel,
    quint64 acceptedReleaseSequenceHighWater)
{
    InstalledSetData installed;
    installed.schemaVersion = kLegacySchema;
    installed.releaseSequence = untrusted.releaseSequence;
    installed.channel = untrusted.channel;
    installed.applicationVersion = untrusted.applicationVersion;
    installed.platform = untrusted.platform;
    installed.architecture = untrusted.architecture;
    installed.manifestSha256 = untrusted.manifestSha256;
    installed.runtimeId = untrusted.runtimeId;
    installed.runtimeVersion = untrusted.runtimeVersion;
    installed.adapterId = untrusted.adapterId;
    installed.adapterVersion = untrusted.adapterVersion;

    Candidate candidate;
    QByteArray publicKey;
    QByteArray payload;
    QString errorCode;
    if (!parseVerifiedLegacyCandidate(
            envelopeJson, publicKeyBase64, nowMs, &candidate, &publicKey,
            &payload, &errorCode)) {
        return invalidDecision(errorCode);
    }
    return evaluateLegacyCandidate(
        candidate, payload, publicKey, nowMs, installed, QString(),
        selectedChannel, acceptedReleaseSequenceHighWater);
}

InstalledAuthorityResult verifyInstalledAuthorityAtRoot(
    const QString &artifactRoot,
    const QByteArray &publicKeyBase64,
    qint64 nowMs,
    const QString &expectedApplicationVersion,
    const QString &expectedChannel,
    const QString &expectedPlatform,
    const QString &expectedArchitecture)
{
    InstallationLayout layout;
    const QString canonicalRoot = canonicalDirectory(artifactRoot);
    layout.applicationPath = QDir(canonicalRoot).filePath(
        QStringLiteral("AegisyClient.test-image"));
    layout.installationRoot = canonicalRoot;
    layout.artifactRoot = canonicalRoot;
    layout.receiptPath = QDir(canonicalRoot).filePath(kInstalledReceiptFileName);
    layout.manifestPath = QDir(canonicalRoot).filePath(kArtifactManifestFileName);
    layout.runtimePath = QDir(canonicalRoot).filePath(runtimeFileName());
    layout.testOnly = true;
    return InstalledArtifactSetAuthority::Verifier::verifyLegacy(
        layout, publicKeyBase64, nowMs, expectedApplicationVersion,
        expectedChannel, expectedPlatform, expectedArchitecture);
}

InstalledAuthorityResult verifyInstalledAuthorityAtRoot(
    const QString &artifactRoot,
    const UpdateSigningKeyRing::Authority &signingAuthority,
    qint64 nowMs,
    const QString &expectedApplicationVersion,
    const QString &expectedChannel,
    const QString &expectedPlatform,
    const QString &expectedArchitecture)
{
    InstallationLayout layout;
    const QString canonicalRoot = canonicalDirectory(artifactRoot);
    layout.applicationPath = QDir(canonicalRoot).filePath(
        QStringLiteral("AegisyClient.test-image"));
    layout.installationRoot = canonicalRoot;
    layout.artifactRoot = canonicalRoot;
    layout.receiptPath = QDir(canonicalRoot).filePath(kInstalledReceiptFileName);
    layout.manifestPath = QDir(canonicalRoot).filePath(kArtifactManifestFileName);
    layout.runtimePath = QDir(canonicalRoot).filePath(runtimeFileName());
    layout.testOnly = true;
    return InstalledArtifactSetAuthority::Verifier::verify(
        layout, signingAuthority, nowMs, expectedApplicationVersion,
        expectedChannel, expectedPlatform, expectedArchitecture);
}

} // namespace Testing
#endif

} // namespace UpdateArtifactSet
