#include "update_signing_key_ring_cache.h"

#include "aap_transport_runtime.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QVector>

#include <cerrno>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace UpdateSigningKeyRingCache {
namespace {

constexpr quint64 kMaximumSafeJsonInteger = 9'007'199'254'740'991ULL;
constexpr int kMaximumEntries = 64;
constexpr qsizetype kMaximumEnvelopeBytes = 128 * 1024;
constexpr qsizetype kMaximumEntryBytes = 192 * 1024;
constexpr qsizetype kMaximumMetadataBytes = 32 * 1024;
constexpr qsizetype kMaximumChainBytes = 8 * 1024 * 1024;

const QString kCacheDirectoryName =
    QStringLiteral("update-signing-key-ring-continuity-v1");
const QString kObjectsDirectoryName = QStringLiteral("objects");
const QString kMarkerFileName = QStringLiteral("marker.json");
const QString kHeadFileName = QStringLiteral("current.json");
const QString kLockFileName =
    QStringLiteral("aegisy-update-signing-key-ring-continuity.lock");
const QString kEntrySchema =
    QStringLiteral("aegisy-update-signing-key-ring-continuity-entry/0.1");
const QString kMarkerSchema =
    QStringLiteral("aegisy-update-signing-key-ring-continuity-marker/0.1");
const QString kHeadSchema =
    QStringLiteral("aegisy-update-signing-key-ring-continuity-head/0.1");

struct NodeInfo
{
    bool inspected = false;
    bool missing = false;
    bool regular = false;
    bool directory = false;
    bool linkLike = false;
    bool singleLink = false;
    bool privatePermissions = false;
    quint64 size = 0;
    QByteArray identity;
};

struct Entry
{
    quint64 generation = 0;
    QByteArray envelope;
    QString envelopeSha256;
    QString previousEntryIdentity;
    QString entryIdentity;
};

struct Marker
{
    QString trustAnchorIdentity;
    QString bootstrapEntryIdentity;
    QString markerIdentity;
};

struct Head
{
    QString trustAnchorIdentity;
    quint64 chainLength = 0;
    quint64 latestGeneration = 0;
    QString latestEntryIdentity;
    QString chainIdentity;
    QString previousCacheIdentity;
    QString ringIdentity;
    QString ringAuthorityIdentity;
    QString cacheIdentity;
};

struct Snapshot
{
    Marker marker;
    Head head;
    QVector<Entry> entries;
};

enum class SnapshotState {
    Empty,
    Valid,
    Invalid,
    Unavailable,
};

struct SnapshotResult
{
    SnapshotState state = SnapshotState::Unavailable;
    bool present = false;
    QString errorCode;
    Snapshot snapshot;
};

bool fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
    return false;
}

QByteArray nodeIdentity(quint64 first, quint64 second)
{
    return QByteArray::number(first, 16) + ':' + QByteArray::number(second, 16);
}

NodeInfo inspectNode(const QString &path)
{
    NodeInfo result;
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        result.inspected =
            error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        result.missing = result.inspected;
        return result;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    result.inspected = GetFileInformationByHandle(handle, &information) != 0;
    CloseHandle(handle);
    if (!result.inspected) return result;
    result.directory =
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    result.regular = !result.directory;
    result.linkLike =
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    result.singleLink = result.directory || information.nNumberOfLinks == 1;
    result.privatePermissions = true;
    result.size = (static_cast<quint64>(information.nFileSizeHigh) << 32) |
                  information.nFileSizeLow;
    const quint64 fileIndex =
        (static_cast<quint64>(information.nFileIndexHigh) << 32) |
        information.nFileIndexLow;
    result.identity = nodeIdentity(information.dwVolumeSerialNumber, fileIndex);
#else
    struct stat information{};
    if (::lstat(QFile::encodeName(path).constData(), &information) != 0) {
        result.inspected = errno == ENOENT;
        result.missing = result.inspected;
        return result;
    }
    result.inspected = true;
    result.linkLike = S_ISLNK(information.st_mode);
    result.regular = S_ISREG(information.st_mode);
    result.directory = S_ISDIR(information.st_mode);
    result.singleLink = result.directory || information.st_nlink == 1;
    const mode_t forbidden =
        result.directory
            ? (S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX)
            : (S_IXUSR | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX);
    const mode_t required =
        result.directory ? (S_IRUSR | S_IWUSR | S_IXUSR) : (S_IRUSR | S_IWUSR);
    result.privatePermissions = (information.st_mode & forbidden) == 0 &&
                                (information.st_mode & required) == required;
    result.size =
        information.st_size < 0 ? 0 : static_cast<quint64>(information.st_size);
    result.identity = nodeIdentity(static_cast<quint64>(information.st_dev),
                                   static_cast<quint64>(information.st_ino));
#endif
    return result;
}

NodeInfo inspectOpenFile(const QFile &file)
{
    NodeInfo result;
    if (!file.isOpen() || file.handle() < 0) return result;
#ifdef Q_OS_WIN
    const intptr_t native = _get_osfhandle(file.handle());
    if (native == -1) return result;
    BY_HANDLE_FILE_INFORMATION information{};
    result.inspected = GetFileInformationByHandle(
                           reinterpret_cast<HANDLE>(native), &information) != 0;
    if (!result.inspected) return result;
    result.directory =
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    result.regular = !result.directory;
    result.linkLike =
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    result.singleLink = information.nNumberOfLinks == 1;
    result.privatePermissions = true;
    result.size = (static_cast<quint64>(information.nFileSizeHigh) << 32) |
                  information.nFileSizeLow;
    const quint64 fileIndex =
        (static_cast<quint64>(information.nFileIndexHigh) << 32) |
        information.nFileIndexLow;
    result.identity = nodeIdentity(information.dwVolumeSerialNumber, fileIndex);
#else
    struct stat information{};
    result.inspected = ::fstat(file.handle(), &information) == 0;
    if (!result.inspected) return result;
    result.linkLike = false;
    result.regular = S_ISREG(information.st_mode);
    result.directory = S_ISDIR(information.st_mode);
    result.singleLink = information.st_nlink == 1;
    result.privatePermissions =
        (information.st_mode &
         (S_IXUSR | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX)) == 0 &&
        (information.st_mode & (S_IRUSR | S_IWUSR)) == (S_IRUSR | S_IWUSR);
    result.size =
        information.st_size < 0 ? 0 : static_cast<quint64>(information.st_size);
    result.identity = nodeIdentity(static_cast<quint64>(information.st_dev),
                                   static_cast<quint64>(information.st_ino));
#endif
    return result;
}

bool sameNode(const NodeInfo &left, const NodeInfo &right)
{
    return left.inspected && right.inspected && !left.missing &&
           !right.missing && left.identity == right.identity &&
           left.regular == right.regular && left.directory == right.directory &&
           left.linkLike == right.linkLike &&
           left.singleLink == right.singleLink &&
           left.privatePermissions == right.privatePermissions &&
           left.size == right.size;
}

bool validStateRoot(const QString &path, const QByteArray &expectedIdentity)
{
    const NodeInfo current = inspectNode(path);
    return current.inspected && !current.missing && current.directory &&
           !current.linkLike && !expectedIdentity.isEmpty() &&
           current.identity == expectedIdentity;
}

bool validPrivateDirectory(const QString &path, QString *errorCode)
{
    const NodeInfo node = inspectNode(path);
    if (!node.inspected || node.missing || !node.directory || node.linkLike) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-path-invalid"));
    }
    if (!node.privatePermissions) {
        return fail(errorCode,
                    QStringLiteral(
                        "update-signing-key-ring-cache-permissions-invalid"));
    }
    return true;
}

bool readBoundedPrivateFile(const QString &path, qsizetype maximumBytes,
                            QByteArray *bytes, QString *errorCode)
{
    const NodeInfo before = inspectNode(path);
    if (!before.inspected || before.missing || !before.regular ||
        before.linkLike || !before.singleLink) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-path-invalid"));
    }
    if (!before.privatePermissions) {
        return fail(errorCode,
                    QStringLiteral(
                        "update-signing-key-ring-cache-permissions-invalid"));
    }
    if (before.size == 0 || before.size > static_cast<quint64>(maximumBytes)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-size-invalid"));
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-read-failed"));
    }
    const NodeInfo opened = inspectOpenFile(file);
    const QByteArray raw = file.read(maximumBytes + 1);
    const bool complete = file.error() == QFile::NoError && file.atEnd() &&
                          raw.size() == static_cast<qsizetype>(before.size);
    const NodeInfo after = inspectNode(path);
    if (!complete) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-read-failed"));
    }
    if (!sameNode(before, opened) || !sameNode(opened, after)) {
        return fail(errorCode,
                    QStringLiteral(
                        "update-signing-key-ring-cache-file-identity-drift"));
    }
    *bytes = raw;
    return true;
}

bool syncPath(const QString &path, bool directory)
{
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        directory ? FILE_READ_ATTRIBUTES : (GENERIC_READ | GENERIC_WRITE),
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT |
            (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0),
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool valid =
        GetFileInformationByHandle(handle, &information) != 0 &&
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (directory
             ? (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
             : (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ==
                   0) &&
        (directory || FlushFileBuffers(handle) != 0);
    CloseHandle(handle);
    return valid;
#else
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_DIRECTORY
    if (directory) flags |= O_DIRECTORY;
#endif
    const int descriptor = ::open(QFile::encodeName(path).constData(), flags);
    if (descriptor < 0) return false;
    const bool synced = ::fsync(descriptor) == 0;
    ::close(descriptor);
    return synced;
#endif
}

QString cachePath(const QString &root)
{
    return QDir(root).filePath(kCacheDirectoryName);
}

QString objectsPath(const QString &root)
{
    return QDir(cachePath(root)).filePath(kObjectsDirectoryName);
}

QString markerPath(const QString &root)
{
    return QDir(cachePath(root)).filePath(kMarkerFileName);
}

QString headPath(const QString &root)
{
    return QDir(cachePath(root)).filePath(kHeadFileName);
}

QString lockPath(const QString &root)
{
    return QDir(root).filePath(kLockFileName);
}

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &keys)
{
    if (object.size() != keys.size()) return false;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!keys.contains(it.key())) return false;
    }
    return true;
}

bool parseExactObject(const QByteArray &bytes, QJsonObject *object)
{
    using namespace aegisy::aap::transport_runtime;
    TransportJsonValue parsed;
    QString parseError;
    if (!parseTransportJsonRaw(bytes, &parsed, &parseError)) return false;
    QJsonValue projected;
    TransportProjectionError projectionError = TransportProjectionError::None;
    if (!projectJsonSafeTransportValue(parsed, &projected, &projectionError) ||
        !projected.isObject()) {
        return false;
    }
    *object = projected.toObject();
    return true;
}

bool positiveSafeInteger(const QJsonValue &value, quint64 *output)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number || number <= 0 ||
        number > static_cast<double>(kMaximumSafeJsonInteger)) {
        return false;
    }
    *output = static_cast<quint64>(number);
    return true;
}

bool validHash(const QString &value)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[0-9a-f]{64}\\z"));
    return pattern.match(value).hasMatch();
}

bool validIdentity(const QString &value, const QString &prefix)
{
    return value.startsWith(prefix) && validHash(value.mid(prefix.size()));
}

QByteArray decodeCanonicalBase64(const QString &encoded)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[A-Za-z0-9+/]+={0,2}\\z"));
    if (!pattern.match(encoded).hasMatch()) return {};
    const QByteArray decoded = QByteArray::fromBase64(encoded.toLatin1());
    return decoded.toBase64() == encoded.toLatin1() ? decoded : QByteArray();
}

void appendLine(QByteArray *payload, const QByteArray &key,
                const QString &value)
{
    payload->append(key);
    payload->append('=');
    payload->append(value.toUtf8());
    payload->append('\n');
}

void appendLine(QByteArray *payload, const QByteArray &key, quint64 value)
{
    appendLine(payload, key, QString::number(value));
}

void appendLine(QByteArray *payload, const QByteArray &key, bool value)
{
    appendLine(payload, key,
               value ? QStringLiteral("true") : QStringLiteral("false"));
}

QString sha256Identity(const QByteArray &payload, const QString &prefix)
{
    return prefix + QString::fromLatin1(QCryptographicHash::hash(
                                            payload, QCryptographicHash::Sha256)
                                            .toHex());
}

QString entryIdentity(const Entry &entry)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring-continuity-entry/0.1\n");
    appendLine(&payload, QByteArrayLiteral("generation"), entry.generation);
    appendLine(&payload, QByteArrayLiteral("envelope_bytes"),
               static_cast<quint64>(entry.envelope.size()));
    appendLine(&payload, QByteArrayLiteral("envelope_sha256"),
               entry.envelopeSha256);
    appendLine(&payload, QByteArrayLiteral("previous_entry_identity"),
               entry.previousEntryIdentity);
    return sha256Identity(
        payload,
        QStringLiteral("update-signing-key-ring-continuity-entry:sha256:"));
}

Entry makeEntry(quint64 generation, const QByteArray &envelope,
                const QString &previousEntryIdentity)
{
    Entry entry;
    entry.generation = generation;
    entry.envelope = envelope;
    entry.envelopeSha256 = QString::fromLatin1(
        QCryptographicHash::hash(envelope, QCryptographicHash::Sha256).toHex());
    entry.previousEntryIdentity = previousEntryIdentity;
    entry.entryIdentity = entryIdentity(entry);
    return entry;
}

QString markerIdentity(const Marker &marker)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring-continuity-marker/0.1\n");
    appendLine(&payload, QByteArrayLiteral("trust_anchor_identity"),
               marker.trustAnchorIdentity);
    appendLine(&payload, QByteArrayLiteral("bootstrap_entry_identity"),
               marker.bootstrapEntryIdentity);
    return sha256Identity(
        payload,
        QStringLiteral("update-signing-key-ring-continuity-marker:sha256:"));
}

QString chainIdentity(const QString &trustAnchorIdentity,
                      const QVector<Entry> &entries)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring-continuity-chain/0.1\n");
    appendLine(&payload, QByteArrayLiteral("trust_anchor_identity"),
               trustAnchorIdentity);
    appendLine(&payload, QByteArrayLiteral("entries.count"),
               static_cast<quint64>(entries.size()));
    for (int index = 0; index < entries.size(); ++index) {
        const QByteArray prefix =
            QByteArrayLiteral("entries.") + QByteArray::number(index);
        appendLine(&payload, prefix + ".generation",
                   entries.at(index).generation);
        appendLine(&payload, prefix + ".identity",
                   entries.at(index).entryIdentity);
    }
    return sha256Identity(
        payload,
        QStringLiteral("update-signing-key-ring-continuity-chain:sha256:"));
}

QString headIdentity(const Head &head)
{
    QByteArray payload = QByteArrayLiteral(
        "aegisy-update-signing-key-ring-continuity-head/0.1\n");
    appendLine(&payload, QByteArrayLiteral("trust_anchor_identity"),
               head.trustAnchorIdentity);
    appendLine(&payload, QByteArrayLiteral("chain_length"), head.chainLength);
    appendLine(&payload, QByteArrayLiteral("latest_generation"),
               head.latestGeneration);
    appendLine(&payload, QByteArrayLiteral("latest_entry_identity"),
               head.latestEntryIdentity);
    appendLine(&payload, QByteArrayLiteral("chain_identity"),
               head.chainIdentity);
    appendLine(&payload, QByteArrayLiteral("previous_cache_identity"),
               head.previousCacheIdentity);
    appendLine(&payload, QByteArrayLiteral("ring_identity"), head.ringIdentity);
    appendLine(&payload, QByteArrayLiteral("ring_authority_identity"),
               head.ringAuthorityIdentity);
    appendLine(&payload, QByteArrayLiteral("update_authorized"), false);
    appendLine(&payload, QByteArrayLiteral("network_authorized"), false);
    appendLine(&payload, QByteArrayLiteral("download_authorized"), false);
    appendLine(&payload, QByteArrayLiteral("install_authorized"), false);
    appendLine(&payload, QByteArrayLiteral("rollback_authorized"), false);
    appendLine(&payload, QByteArrayLiteral("resume_authorized"), false);
    appendLine(&payload, QByteArrayLiteral("execution_authorized"), false);
    appendLine(&payload, QByteArrayLiteral("anti_rollback_protected"), false);
    appendLine(&payload, QByteArrayLiteral("anti_deletion_protected"), false);
    appendLine(&payload, QByteArrayLiteral("trusted_time_available"), false);
    appendLine(&payload, QByteArrayLiteral("expired_signer_recovery_available"),
               false);
    return sha256Identity(
        payload,
        QStringLiteral("update-signing-key-ring-continuity-cache:sha256:"));
}

Head makeHead(const QString &trustAnchorIdentity, const QVector<Entry> &entries,
              const QString &previousCacheIdentity, const QString &ringIdentity,
              const QString &ringAuthorityIdentity)
{
    Head head;
    head.trustAnchorIdentity = trustAnchorIdentity;
    head.chainLength = static_cast<quint64>(entries.size());
    head.latestGeneration = entries.constLast().generation;
    head.latestEntryIdentity = entries.constLast().entryIdentity;
    head.chainIdentity = chainIdentity(trustAnchorIdentity, entries);
    head.previousCacheIdentity = previousCacheIdentity;
    head.ringIdentity = ringIdentity;
    head.ringAuthorityIdentity = ringAuthorityIdentity;
    head.cacheIdentity = headIdentity(head);
    return head;
}

QByteArray encodeEntry(const Entry &entry)
{
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), kEntrySchema},
                   {QStringLiteral("generation"),
                    static_cast<double>(entry.generation)},
                   {QStringLiteral("envelope_base64"),
                    QString::fromLatin1(entry.envelope.toBase64())},
                   {QStringLiteral("envelope_bytes"),
                    static_cast<double>(entry.envelope.size())},
                   {QStringLiteral("envelope_sha256"), entry.envelopeSha256},
                   {QStringLiteral("previous_entry_identity"),
                    entry.previousEntryIdentity},
                   {QStringLiteral("entry_identity"), entry.entryIdentity},
               })
        .toJson(QJsonDocument::Compact);
}

QByteArray encodeMarker(const Marker &marker)
{
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), kMarkerSchema},
                   {QStringLiteral("trust_anchor_identity"),
                    marker.trustAnchorIdentity},
                   {QStringLiteral("bootstrap_entry_identity"),
                    marker.bootstrapEntryIdentity},
                   {QStringLiteral("marker_identity"), marker.markerIdentity},
               })
        .toJson(QJsonDocument::Compact);
}

QByteArray encodeHead(const Head &head)
{
    return QJsonDocument(
               QJsonObject{
                   {QStringLiteral("schema_version"), kHeadSchema},
                   {QStringLiteral("trust_anchor_identity"),
                    head.trustAnchorIdentity},
                   {QStringLiteral("chain_length"),
                    static_cast<double>(head.chainLength)},
                   {QStringLiteral("latest_generation"),
                    static_cast<double>(head.latestGeneration)},
                   {QStringLiteral("latest_entry_identity"),
                    head.latestEntryIdentity},
                   {QStringLiteral("chain_identity"), head.chainIdentity},
                   {QStringLiteral("previous_cache_identity"),
                    head.previousCacheIdentity},
                   {QStringLiteral("ring_identity"), head.ringIdentity},
                   {QStringLiteral("ring_authority_identity"),
                    head.ringAuthorityIdentity},
                   {QStringLiteral("cache_identity"), head.cacheIdentity},
                   {QStringLiteral("update_authorized"), false},
                   {QStringLiteral("network_authorized"), false},
                   {QStringLiteral("download_authorized"), false},
                   {QStringLiteral("install_authorized"), false},
                   {QStringLiteral("rollback_authorized"), false},
                   {QStringLiteral("resume_authorized"), false},
                   {QStringLiteral("execution_authorized"), false},
                   {QStringLiteral("anti_rollback_protected"), false},
                   {QStringLiteral("anti_deletion_protected"), false},
                   {QStringLiteral("trusted_time_available"), false},
                   {QStringLiteral("expired_signer_recovery_available"), false},
               })
        .toJson(QJsonDocument::Compact);
}

bool parseEntry(const QByteArray &bytes, Entry *entry, QString *errorCode)
{
    QJsonObject object;
    if (!parseExactObject(bytes, &object)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-json-invalid"));
    }
    if (!hasExactKeys(object, {
                                  QStringLiteral("schema_version"),
                                  QStringLiteral("generation"),
                                  QStringLiteral("envelope_base64"),
                                  QStringLiteral("envelope_bytes"),
                                  QStringLiteral("envelope_sha256"),
                                  QStringLiteral("previous_entry_identity"),
                                  QStringLiteral("entry_identity"),
                              })) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-fields-invalid"));
    }
    const QJsonValue encoded = object.value(QStringLiteral("envelope_base64"));
    const QJsonValue hash = object.value(QStringLiteral("envelope_sha256"));
    const QJsonValue previous =
        object.value(QStringLiteral("previous_entry_identity"));
    const QJsonValue identity = object.value(QStringLiteral("entry_identity"));
    quint64 envelopeBytes = 0;
    if (object.value(QStringLiteral("schema_version")).toString() !=
            kEntrySchema ||
        !positiveSafeInteger(object.value(QStringLiteral("generation")),
                             &entry->generation) ||
        entry->generation > kMaximumEntries || !encoded.isString() ||
        !positiveSafeInteger(object.value(QStringLiteral("envelope_bytes")),
                             &envelopeBytes) ||
        envelopeBytes > static_cast<quint64>(kMaximumEnvelopeBytes) ||
        !hash.isString() || !validHash(hash.toString()) ||
        !previous.isString() || !identity.isString() ||
        !validIdentity(
            identity.toString(),
            QStringLiteral(
                "update-signing-key-ring-continuity-entry:sha256:"))) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-values-invalid"));
    }
    entry->envelope = decodeCanonicalBase64(encoded.toString());
    if (entry->envelope.isEmpty() ||
        static_cast<quint64>(entry->envelope.size()) != envelopeBytes) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-envelope-invalid"));
    }
    entry->envelopeSha256 = hash.toString();
    entry->previousEntryIdentity = previous.toString();
    entry->entryIdentity = identity.toString();
    if ((entry->generation == 1 && !entry->previousEntryIdentity.isEmpty()) ||
        (entry->generation > 1 &&
         !validIdentity(
             entry->previousEntryIdentity,
             QStringLiteral(
                 "update-signing-key-ring-continuity-entry:sha256:"))) ||
        entry->envelopeSha256 !=
            QString::fromLatin1(QCryptographicHash::hash(
                                    entry->envelope, QCryptographicHash::Sha256)
                                    .toHex()) ||
        entry->entryIdentity != entryIdentity(*entry)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-entry-invalid"));
    }
    return true;
}

bool parseMarker(const QByteArray &bytes, Marker *marker, QString *errorCode)
{
    QJsonObject object;
    if (!parseExactObject(bytes, &object) ||
        !hasExactKeys(object, {
                                  QStringLiteral("schema_version"),
                                  QStringLiteral("trust_anchor_identity"),
                                  QStringLiteral("bootstrap_entry_identity"),
                                  QStringLiteral("marker_identity"),
                              })) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-marker-invalid"));
    }
    marker->trustAnchorIdentity =
        object.value(QStringLiteral("trust_anchor_identity")).toString();
    marker->bootstrapEntryIdentity =
        object.value(QStringLiteral("bootstrap_entry_identity")).toString();
    marker->markerIdentity =
        object.value(QStringLiteral("marker_identity")).toString();
    if (object.value(QStringLiteral("schema_version")).toString() !=
            kMarkerSchema ||
        !validIdentity(marker->trustAnchorIdentity,
                       QStringLiteral("update-signing-trust-anchor:sha256:")) ||
        !validIdentity(
            marker->bootstrapEntryIdentity,
            QStringLiteral(
                "update-signing-key-ring-continuity-entry:sha256:")) ||
        !validIdentity(
            marker->markerIdentity,
            QStringLiteral(
                "update-signing-key-ring-continuity-marker:sha256:")) ||
        marker->markerIdentity != markerIdentity(*marker)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-marker-invalid"));
    }
    return true;
}

bool falseField(const QJsonObject &object, const QString &name)
{
    return object.value(name) == QJsonValue(false);
}

bool parseHead(const QByteArray &bytes, Head *head, QString *errorCode)
{
    QJsonObject object;
    if (!parseExactObject(bytes, &object) ||
        !hasExactKeys(object,
                      {
                          QStringLiteral("schema_version"),
                          QStringLiteral("trust_anchor_identity"),
                          QStringLiteral("chain_length"),
                          QStringLiteral("latest_generation"),
                          QStringLiteral("latest_entry_identity"),
                          QStringLiteral("chain_identity"),
                          QStringLiteral("previous_cache_identity"),
                          QStringLiteral("ring_identity"),
                          QStringLiteral("ring_authority_identity"),
                          QStringLiteral("cache_identity"),
                          QStringLiteral("update_authorized"),
                          QStringLiteral("network_authorized"),
                          QStringLiteral("download_authorized"),
                          QStringLiteral("install_authorized"),
                          QStringLiteral("rollback_authorized"),
                          QStringLiteral("resume_authorized"),
                          QStringLiteral("execution_authorized"),
                          QStringLiteral("anti_rollback_protected"),
                          QStringLiteral("anti_deletion_protected"),
                          QStringLiteral("trusted_time_available"),
                          QStringLiteral("expired_signer_recovery_available"),
                      })) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-head-invalid"));
    }
    head->trustAnchorIdentity =
        object.value(QStringLiteral("trust_anchor_identity")).toString();
    head->latestEntryIdentity =
        object.value(QStringLiteral("latest_entry_identity")).toString();
    head->chainIdentity =
        object.value(QStringLiteral("chain_identity")).toString();
    head->previousCacheIdentity =
        object.value(QStringLiteral("previous_cache_identity")).toString();
    head->ringIdentity =
        object.value(QStringLiteral("ring_identity")).toString();
    head->ringAuthorityIdentity =
        object.value(QStringLiteral("ring_authority_identity")).toString();
    head->cacheIdentity =
        object.value(QStringLiteral("cache_identity")).toString();
    if (object.value(QStringLiteral("schema_version")).toString() !=
            kHeadSchema ||
        !positiveSafeInteger(object.value(QStringLiteral("chain_length")),
                             &head->chainLength) ||
        !positiveSafeInteger(object.value(QStringLiteral("latest_generation")),
                             &head->latestGeneration) ||
        head->chainLength > kMaximumEntries ||
        head->latestGeneration != head->chainLength ||
        !validIdentity(head->trustAnchorIdentity,
                       QStringLiteral("update-signing-trust-anchor:sha256:")) ||
        !validIdentity(
            head->latestEntryIdentity,
            QStringLiteral(
                "update-signing-key-ring-continuity-entry:sha256:")) ||
        !validIdentity(
            head->chainIdentity,
            QStringLiteral(
                "update-signing-key-ring-continuity-chain:sha256:")) ||
        (head->chainLength == 1 && !head->previousCacheIdentity.isEmpty()) ||
        (head->chainLength > 1 &&
         !validIdentity(
             head->previousCacheIdentity,
             QStringLiteral(
                 "update-signing-key-ring-continuity-cache:sha256:"))) ||
        !validIdentity(head->ringIdentity,
                       QStringLiteral("update-signing-key-ring:sha256:")) ||
        !validIdentity(
            head->ringAuthorityIdentity,
            QStringLiteral("update-signing-key-ring-authority:sha256:")) ||
        !validIdentity(
            head->cacheIdentity,
            QStringLiteral(
                "update-signing-key-ring-continuity-cache:sha256:")) ||
        !falseField(object, QStringLiteral("update_authorized")) ||
        !falseField(object, QStringLiteral("network_authorized")) ||
        !falseField(object, QStringLiteral("download_authorized")) ||
        !falseField(object, QStringLiteral("install_authorized")) ||
        !falseField(object, QStringLiteral("rollback_authorized")) ||
        !falseField(object, QStringLiteral("resume_authorized")) ||
        !falseField(object, QStringLiteral("execution_authorized")) ||
        !falseField(object, QStringLiteral("anti_rollback_protected")) ||
        !falseField(object, QStringLiteral("anti_deletion_protected")) ||
        !falseField(object, QStringLiteral("trusted_time_available")) ||
        !falseField(object,
                    QStringLiteral("expired_signer_recovery_available")) ||
        head->cacheIdentity != headIdentity(*head)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-head-invalid"));
    }
    return true;
}

QString entryFileName(const QString &identity)
{
    const int separator = identity.lastIndexOf(QLatin1Char(':'));
    return separator < 0 ? QString()
                         : QStringLiteral("entry-%1.json")
                               .arg(identity.mid(separator + 1));
}

bool ensurePrivateDirectory(const QString &path, QString *errorCode)
{
    const NodeInfo existing = inspectNode(path);
    if (existing.inspected && existing.missing) {
        const QFileInfo info(path);
        if (!QDir(info.absolutePath()).mkdir(info.fileName())) {
            return fail(
                errorCode,
                QStringLiteral("update-signing-key-ring-cache-write-failed"));
        }
#ifndef Q_OS_WIN
        if (!QFile::setPermissions(path, QFileDevice::ReadOwner |
                                             QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner)) {
            return fail(
                errorCode,
                QStringLiteral("update-signing-key-ring-cache-write-failed"));
        }
#endif
        if (!syncPath(info.absolutePath(), true)) {
            return fail(
                errorCode,
                QStringLiteral("update-signing-key-ring-cache-sync-failed"));
        }
    }
    return validPrivateDirectory(path, errorCode);
}

bool writeImmutable(const QString &path, const QByteArray &bytes,
                    qsizetype maximumBytes, QString *errorCode)
{
    if (bytes.isEmpty() || bytes.size() > maximumBytes) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-size-invalid"));
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        file.write(bytes) != bytes.size() || !file.flush()) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-write-failed"));
    }
#ifndef Q_OS_WIN
    if (!file.setPermissions(QFileDevice::ReadOwner |
                             QFileDevice::WriteOwner)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-write-failed"));
    }
#endif
    file.close();
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner |
                                         QFileDevice::WriteOwner)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-write-failed"));
    }
#endif
    const QFileInfo info(path);
    if (!syncPath(path, false) || !syncPath(info.absolutePath(), true)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-sync-failed"));
    }
    QByteArray reread;
    if (!readBoundedPrivateFile(path, maximumBytes, &reread, errorCode) ||
        reread != bytes) {
        return fail(errorCode,
                    QStringLiteral(
                        "update-signing-key-ring-cache-post-commit-invalid"));
    }
    return true;
}

bool writeHead(const QString &root, const Head &head,
               const QString &expectedCurrentCacheIdentity, QString *errorCode)
{
    if (expectedCurrentCacheIdentity.isEmpty()) {
        const NodeInfo current = inspectNode(headPath(root));
        if (!current.inspected || !current.missing) {
            return fail(
                errorCode,
                QStringLiteral(
                    "update-signing-key-ring-cache-continuity-mismatch"));
        }
    } else {
        QByteArray currentBytes;
        Head current;
        if (!readBoundedPrivateFile(headPath(root), kMaximumMetadataBytes,
                                    &currentBytes, errorCode) ||
            !parseHead(currentBytes, &current, errorCode) ||
            current.cacheIdentity != expectedCurrentCacheIdentity) {
            return fail(
                errorCode,
                QStringLiteral(
                    "update-signing-key-ring-cache-continuity-mismatch"));
        }
    }
    const QByteArray bytes = encodeHead(head);
    if (bytes.isEmpty() || bytes.size() > kMaximumMetadataBytes) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-size-invalid"));
    }
    QSaveFile file(headPath(root));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.flush()) {
        file.cancelWriting();
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-write-failed"));
    }
#ifndef Q_OS_WIN
    if (!file.setPermissions(QFileDevice::ReadOwner |
                             QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-write-failed"));
    }
#endif
    if (!file.commit()) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-write-failed"));
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(headPath(root), QFileDevice::ReadOwner |
                                                   QFileDevice::WriteOwner)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-write-failed"));
    }
#endif
    if (!syncPath(headPath(root), false) || !syncPath(cachePath(root), true)) {
        return fail(
            errorCode,
            QStringLiteral("update-signing-key-ring-cache-sync-failed"));
    }
    return true;
}

SnapshotResult snapshotError(SnapshotState state, const QString &errorCode,
                             bool present)
{
    SnapshotResult result;
    result.state = state;
    result.present = present;
    result.errorCode = errorCode;
    return result;
}

SnapshotResult readSnapshot(const QString &root, const QByteArray &rootIdentity)
{
    if (!validStateRoot(root, rootIdentity)) {
        return snapshotError(
            SnapshotState::Unavailable,
            QStringLiteral("update-signing-key-ring-cache-root-invalid"),
            false);
    }
    const NodeInfo cacheNode = inspectNode(cachePath(root));
    if (cacheNode.inspected && cacheNode.missing) {
        return snapshotError(
            SnapshotState::Empty,
            QStringLiteral("update-signing-key-ring-cache-empty"), false);
    }
    QString errorCode;
    if (!validPrivateDirectory(cachePath(root), &errorCode)) {
        return snapshotError(SnapshotState::Invalid, errorCode, true);
    }

    const QSet<QString> allowed{
        kMarkerFileName,
        kHeadFileName,
        kObjectsDirectoryName,
    };
    QDirIterator cacheIterator(
        cachePath(root),
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDirIterator::NoIteratorFlags);
    int cacheEntryCount = 0;
    while (cacheIterator.hasNext()) {
        cacheIterator.next();
        ++cacheEntryCount;
        if (cacheEntryCount > allowed.size()
            || !allowed.contains(cacheIterator.fileName())) {
            return snapshotError(
                SnapshotState::Invalid,
                QStringLiteral(
                    "update-signing-key-ring-cache-inventory-invalid"),
                true);
        }
    }
    if (!validPrivateDirectory(objectsPath(root), &errorCode)) {
        return snapshotError(SnapshotState::Invalid, errorCode, true);
    }
    const NodeInfo objectsNode = inspectNode(objectsPath(root));

    QByteArray markerBytes;
    QByteArray headBytes;
    if (!readBoundedPrivateFile(markerPath(root), kMaximumMetadataBytes,
                                &markerBytes, &errorCode) ||
        !readBoundedPrivateFile(headPath(root), kMaximumMetadataBytes,
                                &headBytes, &errorCode)) {
        return snapshotError(SnapshotState::Invalid, errorCode, true);
    }
    Snapshot snapshot;
    if (!parseMarker(markerBytes, &snapshot.marker, &errorCode) ||
        !parseHead(headBytes, &snapshot.head, &errorCode)) {
        return snapshotError(SnapshotState::Invalid, errorCode, true);
    }

    static const QRegularExpression entryNamePattern(
        QStringLiteral("\\Aentry-[0-9a-f]{64}\\.json\\z"));
    QMap<quint64, Entry> byGeneration;
    qsizetype totalEnvelopeBytes = 0;
    QDirIterator objectIterator(
        objectsPath(root),
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDirIterator::NoIteratorFlags);
    int objectCount = 0;
    while (objectIterator.hasNext()) {
        objectIterator.next();
        ++objectCount;
        if (objectCount > kMaximumEntries) {
            return snapshotError(
                SnapshotState::Invalid,
                QStringLiteral(
                    "update-signing-key-ring-cache-chain-size-invalid"),
                true);
        }
        const QFileInfo info = objectIterator.fileInfo();
        if (!entryNamePattern.match(info.fileName()).hasMatch()) {
            return snapshotError(
                SnapshotState::Invalid,
                QStringLiteral(
                    "update-signing-key-ring-cache-inventory-invalid"),
                true);
        }
        QByteArray entryBytes;
        if (!readBoundedPrivateFile(info.absoluteFilePath(), kMaximumEntryBytes,
                                    &entryBytes, &errorCode)) {
            return snapshotError(SnapshotState::Invalid, errorCode, true);
        }
        Entry entry;
        if (!parseEntry(entryBytes, &entry, &errorCode) ||
            info.fileName() != entryFileName(entry.entryIdentity) ||
            byGeneration.contains(entry.generation)) {
            return snapshotError(
                SnapshotState::Invalid,
                errorCode.isEmpty()
                    ? QStringLiteral(
                          "update-signing-key-ring-cache-generation-conflict")
                    : errorCode,
                true);
        }
        totalEnvelopeBytes += entry.envelope.size();
        if (totalEnvelopeBytes > kMaximumChainBytes) {
            return snapshotError(
                SnapshotState::Invalid,
                QStringLiteral(
                    "update-signing-key-ring-cache-capacity-exceeded"),
                true);
        }
        byGeneration.insert(entry.generation, entry);
    }
    if (objectCount == 0) {
        return snapshotError(
            SnapshotState::Invalid,
            QStringLiteral("update-signing-key-ring-cache-chain-size-invalid"),
            true);
    }
    for (int index = 1; index <= byGeneration.size(); ++index) {
        const quint64 generation = static_cast<quint64>(index);
        if (!byGeneration.contains(generation)) {
            return snapshotError(
                SnapshotState::Invalid,
                QStringLiteral("update-signing-key-ring-cache-generation-gap"),
                true);
        }
        const Entry entry = byGeneration.value(generation);
        if (generation == 1) {
            if (!entry.previousEntryIdentity.isEmpty()) {
                return snapshotError(
                    SnapshotState::Invalid,
                    QStringLiteral(
                        "update-signing-key-ring-cache-chain-invalid"),
                    true);
            }
        } else if (entry.previousEntryIdentity !=
                   snapshot.entries.constLast().entryIdentity) {
            return snapshotError(
                SnapshotState::Invalid,
                QStringLiteral("update-signing-key-ring-cache-chain-invalid"),
                true);
        }
        snapshot.entries.append(entry);
    }
    if (snapshot.head.chainLength !=
            static_cast<quint64>(snapshot.entries.size()) ||
        snapshot.head.latestGeneration !=
            snapshot.entries.constLast().generation ||
        snapshot.head.latestEntryIdentity !=
            snapshot.entries.constLast().entryIdentity ||
        snapshot.marker.bootstrapEntryIdentity !=
            snapshot.entries.constFirst().entryIdentity ||
        snapshot.marker.trustAnchorIdentity !=
            snapshot.head.trustAnchorIdentity ||
        snapshot.head.chainIdentity !=
            chainIdentity(snapshot.head.trustAnchorIdentity,
                          snapshot.entries)) {
        return snapshotError(
            SnapshotState::Invalid,
            QStringLiteral("update-signing-key-ring-cache-pointer-invalid"),
            true);
    }
    const NodeInfo cacheAfter = inspectNode(cachePath(root));
    const NodeInfo objectsAfter = inspectNode(objectsPath(root));
    if (!validStateRoot(root, rootIdentity) ||
        !sameNode(cacheNode, cacheAfter) ||
        !sameNode(objectsNode, objectsAfter)) {
        return snapshotError(
            SnapshotState::Invalid,
            QStringLiteral(
                "update-signing-key-ring-cache-directory-identity-drift"),
            true);
    }

    SnapshotResult result;
    result.state = SnapshotState::Valid;
    result.present = true;
    result.snapshot = snapshot;
    return result;
}

Observation observationError(State state, const QString &errorCode,
                             bool present = false)
{
    Observation result;
    result.state = state;
    result.present = present;
    result.errorCode = errorCode;
    return result;
}

Observation
observe(const QString &root, const QByteArray &rootIdentity,
        const UpdateSigningKeyRing::TrustAnchorAuthority &trustAnchor,
        qint64 nowMs, Snapshot *loadedSnapshot = nullptr)
{
    if (!trustAnchor.isValid()) {
        return observationError(
            State::Unavailable,
            QStringLiteral("update-signing-key-ring-cache-anchor-unavailable"));
    }
    const SnapshotResult stored = readSnapshot(root, rootIdentity);
    if (stored.state == SnapshotState::Empty) {
        Observation result =
            observationError(State::Empty, stored.errorCode, false);
        result.trustAnchorIdentity = trustAnchor.anchorIdentity();
        return result;
    }
    if (stored.state == SnapshotState::Unavailable) {
        return observationError(State::Unavailable, stored.errorCode,
                                stored.present);
    }
    if (stored.state == SnapshotState::Invalid) {
        return observationError(State::Invalid, stored.errorCode,
                                stored.present);
    }
    const Snapshot &snapshot = stored.snapshot;
    if (snapshot.head.trustAnchorIdentity != trustAnchor.anchorIdentity()) {
        return observationError(
            State::Invalid,
            QStringLiteral("update-signing-key-ring-cache-anchor-mismatch"),
            true);
    }

    QVector<QByteArray> envelopes;
    envelopes.reserve(snapshot.entries.size());
    for (const Entry &entry : snapshot.entries)
        envelopes.append(entry.envelope);
    const UpdateSigningKeyRing::EnvelopeChainResult verification =
        UpdateSigningKeyRing::verifyEnvelopeChain(envelopes, trustAnchor,
                                                  nowMs);

    Observation result;
    result.present = true;
    result.integrityVerified =
        verification.status !=
        UpdateSigningKeyRing::EnvelopeChainStatus::Invalid;
    result.verificationErrorCode = verification.errorCode.isEmpty()
        ? verification.strictVerificationError
        : verification.errorCode;
    result.trustAnchorIdentity = snapshot.head.trustAnchorIdentity;
    result.generation = snapshot.head.latestGeneration;
    result.latestEntryIdentity = snapshot.head.latestEntryIdentity;
    result.chainIdentity = snapshot.head.chainIdentity;
    result.cacheIdentity = snapshot.head.cacheIdentity;
    result.previousCacheIdentity = snapshot.head.previousCacheIdentity;
    result.ringIdentity = snapshot.head.ringIdentity;
    result.ringAuthorityIdentity = snapshot.head.ringAuthorityIdentity;
    if (verification.status ==
        UpdateSigningKeyRing::EnvelopeChainStatus::Invalid) {
        result.state = State::Invalid;
        result.errorCode =
            QStringLiteral("update-signing-key-ring-cache-chain-invalid");
        return result;
    }
    if (verification.checkpoints.size() != snapshot.entries.size()) {
        return observationError(
            State::Invalid,
            QStringLiteral(
                "update-signing-key-ring-cache-chain-binding-invalid"),
            true);
    }
    QVector<Entry> verifiedPrefix;
    verifiedPrefix.reserve(snapshot.entries.size());
    QString previousPrefixCacheIdentity;
    Head expectedHead;
    for (int index = 0; index < snapshot.entries.size(); ++index) {
        const Entry &entry = snapshot.entries.at(index);
        const UpdateSigningKeyRing::EnvelopeChainCheckpoint &checkpoint =
            verification.checkpoints.at(index);
        if (checkpoint.generation != entry.generation
            || checkpoint.ringIdentity.isEmpty()
            || checkpoint.authorityIdentity.isEmpty()) {
            return observationError(
                State::Invalid,
                QStringLiteral(
                    "update-signing-key-ring-cache-chain-binding-invalid"),
                true);
        }
        verifiedPrefix.append(entry);
        expectedHead = makeHead(
            snapshot.head.trustAnchorIdentity, verifiedPrefix,
            previousPrefixCacheIdentity, checkpoint.ringIdentity,
            checkpoint.authorityIdentity);
        previousPrefixCacheIdentity = expectedHead.cacheIdentity;
    }
    if (verification.generation != result.generation ||
        verification.trustAnchorIdentity != result.trustAnchorIdentity ||
        verification.ringIdentity != result.ringIdentity ||
        verification.authorityIdentity != result.ringAuthorityIdentity ||
        snapshot.head.previousCacheIdentity !=
            expectedHead.previousCacheIdentity ||
        snapshot.head.cacheIdentity != expectedHead.cacheIdentity) {
        return observationError(
            State::Invalid,
            QStringLiteral(
                "update-signing-key-ring-cache-chain-binding-invalid"),
            true);
    }
    if (verification.status ==
        UpdateSigningKeyRing::EnvelopeChainStatus::Authoritative) {
        if (!verification.authority.isValid()) {
            return observationError(
                State::Invalid,
                QStringLiteral(
                    "update-signing-key-ring-cache-authority-invalid"),
                true);
        }
        result.state = State::Authoritative;
        result.authority = verification.authority;
    } else if (verification.status ==
               UpdateSigningKeyRing::EnvelopeChainStatus::
                   CachedButNotAuthoritative) {
        if (verification.authority.isValid()) {
            return observationError(
                State::Invalid,
                QStringLiteral(
                    "update-signing-key-ring-cache-authority-invalid"),
                true);
        }
        result.state = State::CachedButNotAuthoritative;
        result.errorCode =
            QStringLiteral("update-signing-key-ring-cache-not-authoritative");
    }
    if (loadedSnapshot && result.state != State::Invalid) {
        *loadedSnapshot = snapshot;
    }
    return result;
}

CommitResult commitError(const QString &errorCode,
                         const Observation &observation = Observation())
{
    CommitResult result;
    result.errorCode = errorCode;
    result.observation = observation;
    return result;
}

CommitResult committedResult(const Observation &observation, bool idempotent)
{
    CommitResult result;
    result.committed = !idempotent;
    result.idempotent = idempotent;
    result.postCommitVerified = true;
    result.observation = observation;
    return result;
}

bool validEnvelopeRequest(const QByteArray &envelope, qint64 nowMs)
{
    return !envelope.isEmpty() && envelope.size() <= kMaximumEnvelopeBytes &&
           nowMs > 0 && static_cast<quint64>(nowMs) <= kMaximumSafeJsonInteger;
}

} // namespace

Store::Store(const QString &stateRoot)
{
    const QFileInfo info(stateRoot);
    if (!info.isDir() || info.isSymLink()) return;
    m_stateRoot = info.canonicalFilePath();
    const NodeInfo root = inspectNode(m_stateRoot);
    if (!root.inspected || root.missing || !root.directory || root.linkLike) {
        m_stateRoot.clear();
        return;
    }
    m_stateRootIdentity = root.identity;
}

Observation
Store::load(const UpdateSigningKeyRing::TrustAnchorAuthority &trustAnchor,
            qint64 nowMs) const
{
    if (m_stateRoot.isEmpty()) {
        return observationError(
            State::Unavailable,
            QStringLiteral("update-signing-key-ring-cache-root-invalid"));
    }
    QLockFile lock(lockPath(m_stateRoot));
    if (!lock.tryLock(0)) {
        return observationError(
            State::Unavailable,
            QStringLiteral("update-signing-key-ring-cache-busy"));
    }
    return observe(m_stateRoot, m_stateRootIdentity, trustAnchor, nowMs);
}

CommitResult
Store::bootstrap(const QByteArray &generationOneEnvelope,
                 const UpdateSigningKeyRing::TrustAnchorAuthority &trustAnchor,
                 qint64 nowMs) const
{
    if (m_stateRoot.isEmpty() || !trustAnchor.isValid() ||
        !validEnvelopeRequest(generationOneEnvelope, nowMs)) {
        return commitError(
            QStringLiteral("update-signing-key-ring-cache-request-invalid"));
    }
    const Entry entry = makeEntry(1, generationOneEnvelope, QString());

    QLockFile lock(lockPath(m_stateRoot));
    if (!lock.tryLock(0)) {
        return commitError(
            QStringLiteral("update-signing-key-ring-cache-busy"));
    }
    Snapshot currentSnapshot;
    const Observation current = observe(m_stateRoot, m_stateRootIdentity,
                                        trustAnchor, nowMs, &currentSnapshot);
    if ((current.state == State::Authoritative ||
         current.state == State::CachedButNotAuthoritative) &&
        current.generation == 1 &&
        current.latestEntryIdentity == entry.entryIdentity) {
        const bool exactEnvelope =
            currentSnapshot.entries.size() == 1 &&
            currentSnapshot.entries.constFirst().envelope ==
                generationOneEnvelope;
        if (!exactEnvelope) {
            return commitError(QStringLiteral(
                "update-signing-key-ring-cache-continuity-mismatch"));
        }
        return committedResult(current, true);
    }
    if (current.state != State::Empty) {
        return commitError(
            current.errorCode.isEmpty()
                ? QStringLiteral(
                      "update-signing-key-ring-cache-already-initialized")
                : current.errorCode,
            current);
    }

    const UpdateSigningKeyRing::EnvelopeChainResult verification =
        UpdateSigningKeyRing::verifyEnvelopeChain(
            QVector<QByteArray>{generationOneEnvelope}, trustAnchor, nowMs);
    if (verification.status !=
            UpdateSigningKeyRing::EnvelopeChainStatus::Authoritative ||
        !verification.authority.isValid() || verification.generation != 1) {
        return commitError(
            QStringLiteral("update-signing-key-ring-cache-bootstrap-invalid"));
    }

    QString errorCode;
    if (!ensurePrivateDirectory(cachePath(m_stateRoot), &errorCode) ||
        !ensurePrivateDirectory(objectsPath(m_stateRoot), &errorCode)) {
        return commitError(errorCode);
    }
    Marker marker;
    marker.trustAnchorIdentity = trustAnchor.anchorIdentity();
    marker.bootstrapEntryIdentity = entry.entryIdentity;
    marker.markerIdentity = markerIdentity(marker);
    const QVector<Entry> entries{entry};
    const Head head =
        makeHead(trustAnchor.anchorIdentity(), entries, QString(),
                 verification.ringIdentity, verification.authorityIdentity);
    if (!writeImmutable(markerPath(m_stateRoot), encodeMarker(marker),
                        kMaximumMetadataBytes, &errorCode) ||
        !writeImmutable(QDir(objectsPath(m_stateRoot))
                            .filePath(entryFileName(entry.entryIdentity)),
                        encodeEntry(entry), kMaximumEntryBytes, &errorCode) ||
        !writeHead(m_stateRoot, head, QString(), &errorCode)) {
        return commitError(errorCode);
    }
    const Observation verified =
        observe(m_stateRoot, m_stateRootIdentity, trustAnchor, nowMs);
    if (verified.state != State::Authoritative ||
        verified.cacheIdentity != head.cacheIdentity ||
        !verified.authority.isValid()) {
        return commitError(QStringLiteral(
            "update-signing-key-ring-cache-post-commit-invalid"));
    }
    return committedResult(verified, false);
}

CommitResult
Store::append(const QByteArray &nextEnvelope,
              const UpdateSigningKeyRing::TrustAnchorAuthority &trustAnchor,
              qint64 nowMs, const QString &expectedCurrentCacheIdentity) const
{
    if (m_stateRoot.isEmpty() || !trustAnchor.isValid() ||
        !validEnvelopeRequest(nextEnvelope, nowMs) ||
        !validIdentity(
            expectedCurrentCacheIdentity,
            QStringLiteral(
                "update-signing-key-ring-continuity-cache:sha256:"))) {
        return commitError(
            QStringLiteral("update-signing-key-ring-cache-request-invalid"));
    }
    QLockFile lock(lockPath(m_stateRoot));
    if (!lock.tryLock(0)) {
        return commitError(
            QStringLiteral("update-signing-key-ring-cache-busy"));
    }
    Snapshot snapshot;
    const Observation current = observe(m_stateRoot, m_stateRootIdentity,
                                        trustAnchor, nowMs, &snapshot);
    if (current.state == State::CachedButNotAuthoritative) {
        const bool exactEnvelope =
            !snapshot.entries.isEmpty() &&
            snapshot.entries.constLast().envelope == nextEnvelope;
        const bool expectationMatches =
            expectedCurrentCacheIdentity == current.cacheIdentity ||
            expectedCurrentCacheIdentity == current.previousCacheIdentity;
        if (exactEnvelope && expectationMatches) {
            return committedResult(current, true);
        }
        return commitError(
            exactEnvelope
                ? QStringLiteral(
                      "update-signing-key-ring-cache-continuity-mismatch")
                : QStringLiteral(
                      "update-signing-key-ring-cache-not-authoritative"),
            current);
    }
    if (current.state != State::Authoritative || !current.authority.isValid()) {
        return commitError(current.errorCode, current);
    }

    const UpdateSigningKeyRing::AuthorityResult rotation =
        UpdateSigningKeyRing::verifyRotation(nextEnvelope, current.authority,
                                             nowMs);
    if (!rotation.ok || !rotation.authority.isValid()) {
        return commitError(
            rotation.errorCode.isEmpty()
                ? QStringLiteral(
                      "update-signing-key-ring-cache-rotation-invalid")
                : rotation.errorCode,
            current);
    }
    if (rotation.idempotent) {
        const bool exactEnvelope =
            !snapshot.entries.isEmpty() &&
            snapshot.entries.constLast().envelope == nextEnvelope;
        const bool expectationMatches =
            expectedCurrentCacheIdentity == current.cacheIdentity ||
            expectedCurrentCacheIdentity == current.previousCacheIdentity;
        if (!exactEnvelope || !expectationMatches) {
            return commitError(QStringLiteral(
                "update-signing-key-ring-cache-continuity-mismatch"));
        }
        return committedResult(current, true);
    }
    if (expectedCurrentCacheIdentity != current.cacheIdentity) {
        return commitError(QStringLiteral(
            "update-signing-key-ring-cache-continuity-mismatch"));
    }
    if (snapshot.entries.size() >= kMaximumEntries) {
        return commitError(
            QStringLiteral("update-signing-key-ring-cache-capacity-exceeded"));
    }
    qsizetype totalBytes = nextEnvelope.size();
    for (const Entry &entry : snapshot.entries) {
        totalBytes += entry.envelope.size();
    }
    if (totalBytes > kMaximumChainBytes) {
        return commitError(
            QStringLiteral("update-signing-key-ring-cache-capacity-exceeded"));
    }
    if (rotation.authority.generation() != current.generation + 1) {
        return commitError(
            QStringLiteral("update-signing-key-ring-cache-generation-gap"));
    }

    const Entry entry = makeEntry(rotation.authority.generation(), nextEnvelope,
                                  snapshot.entries.constLast().entryIdentity);
    snapshot.entries.append(entry);
    const Head head =
        makeHead(trustAnchor.anchorIdentity(), snapshot.entries,
                 current.cacheIdentity, rotation.authority.ringIdentity(),
                 rotation.authority.authorityIdentity());
    QString errorCode;
    if (!writeImmutable(QDir(objectsPath(m_stateRoot))
                            .filePath(entryFileName(entry.entryIdentity)),
                        encodeEntry(entry), kMaximumEntryBytes, &errorCode) ||
        !writeHead(m_stateRoot, head, current.cacheIdentity, &errorCode)) {
        return commitError(errorCode);
    }
    const Observation verified =
        observe(m_stateRoot, m_stateRootIdentity, trustAnchor, nowMs);
    if (verified.state != State::Authoritative ||
        verified.cacheIdentity != head.cacheIdentity ||
        verified.previousCacheIdentity != current.cacheIdentity ||
        !verified.authority.isValid()) {
        return commitError(QStringLiteral(
            "update-signing-key-ring-cache-post-commit-invalid"));
    }
    return committedResult(verified, false);
}

QString stateName(State state)
{
    switch (state) {
    case State::Empty:
        return QStringLiteral("empty");
    case State::Authoritative:
        return QStringLiteral("authoritative");
    case State::CachedButNotAuthoritative:
        return QStringLiteral("cached-but-not-authoritative");
    case State::Invalid:
        return QStringLiteral("invalid");
    case State::Unavailable:
        return QStringLiteral("unavailable");
    }
    return {};
}

} // namespace UpdateSigningKeyRingCache
