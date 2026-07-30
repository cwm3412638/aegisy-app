#include "update_progress_record.h"

#include "aap_transport_runtime.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <cmath>
#include <cerrno>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace UpdateProgressRecord {
namespace {

constexpr quint64 kMaximumSafeJsonInteger = 9'007'199'254'740'991ULL;
constexpr qsizetype kMaximumRecordBytes = 16 * 1024;
const QString kSchema = QStringLiteral("aegisy-update-progress-record/0.1");
const QString kRecordFileName = QStringLiteral("aegisy-update-progress.json");
const QString kLockFileName = QStringLiteral("aegisy-update-progress.lock");

enum class FileState {
    Missing,
    RegularSingleLink,
    LinkLike,
    MultipleLinks,
    PermissionsWide,
    Invalid,
};

FileState inspectFile(const QString &path)
{
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND
            ? FileState::Missing : FileState::Invalid;
    }
    BY_HANDLE_FILE_INFORMATION information{};
    const bool inspected = GetFileInformationByHandle(handle, &information) != 0;
    CloseHandle(handle);
    if (!inspected || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return FileState::Invalid;
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return FileState::LinkLike;
    }
    if (information.nNumberOfLinks != 1) return FileState::MultipleLinks;
    return FileState::RegularSingleLink;
#else
    const QByteArray encodedPath = QFile::encodeName(path);
    struct stat information {};
    if (::lstat(encodedPath.constData(), &information) != 0) {
        return errno == ENOENT ? FileState::Missing : FileState::Invalid;
    }
    if (S_ISLNK(information.st_mode)) return FileState::LinkLike;
    if (!S_ISREG(information.st_mode)) return FileState::Invalid;
    if (information.st_nlink != 1) return FileState::MultipleLinks;
    if ((information.st_mode
         & (S_IXUSR | S_IRWXG | S_IRWXO | S_ISUID | S_ISGID | S_ISVTX))
        != 0) {
        return FileState::PermissionsWide;
    }
    return FileState::RegularSingleLink;
#endif
}

QString canonicalRoot(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isDir() || info.isSymLink()) return {};
    return info.canonicalFilePath();
}

QString recordPath(const QString &root)
{
    return root.isEmpty() ? QString() : QDir(root).filePath(kRecordFileName);
}

QString lockPath(const QString &root)
{
    return root.isEmpty() ? QString() : QDir(root).filePath(kLockFileName);
}

bool syncCommittedRecord(const QString &path, const QString &root)
{
#ifdef Q_OS_WIN
    const QString nativePath = QDir::toNativeSeparators(path);
    HANDLE handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION information{};
    const bool valid = GetFileInformationByHandle(handle, &information) != 0
        && (information.dwFileAttributes
            & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0
        && FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    Q_UNUSED(root);
    return valid;
#else
    int fileFlags = O_RDONLY;
#ifdef O_CLOEXEC
    fileFlags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    fileFlags |= O_NOFOLLOW;
#endif
    const int fileDescriptor = ::open(
        QFile::encodeName(path).constData(), fileFlags);
    if (fileDescriptor < 0) return false;
    const bool fileSynced = ::fsync(fileDescriptor) == 0;
    ::close(fileDescriptor);

    int directoryFlags = O_RDONLY;
#ifdef O_CLOEXEC
    directoryFlags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    directoryFlags |= O_DIRECTORY;
#endif
    const int directoryDescriptor = ::open(
        QFile::encodeName(root).constData(), directoryFlags);
    if (directoryDescriptor < 0) return false;
    const bool directorySynced = ::fsync(directoryDescriptor) == 0;
    ::close(directoryDescriptor);
    return fileSynced && directorySynced;
#endif
}

bool hasExactKeys(const QJsonObject &object, const QSet<QString> &expected)
{
    if (object.size() != expected.size()) return false;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!expected.contains(it.key())) return false;
    }
    return true;
}

bool positiveSafeInteger(const QJsonValue &value, quint64 *output)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number <= 0 || number > static_cast<double>(kMaximumSafeJsonInteger)) {
        return false;
    }
    *output = static_cast<quint64>(number);
    return true;
}

bool validArtifactSetIdentity(const QString &identity)
{
    static const QRegularExpression pattern(QStringLiteral(
        "\\Aupdate-artifact-set:sha256:[0-9a-f]{64}\\z"));
    return pattern.match(identity).hasMatch();
}

bool validRecordIdentity(const QString &identity)
{
    static const QRegularExpression pattern(QStringLiteral(
        "\\Aupdate-progress-record:sha256:[0-9a-f]{64}\\z"));
    return pattern.match(identity).hasMatch();
}

int phaseRank(Phase phase)
{
    switch (phase) {
    case Phase::CandidateEvaluated: return 0;
    case Phase::DownloadStarted: return 1;
    case Phase::DownloadVerified: return 2;
    case Phase::InstallStarted: return 3;
    case Phase::InstallationObserved: return 4;
    }
    return -1;
}

bool parsePhase(const QString &value, Phase *phase)
{
    if (value == QStringLiteral("candidate-evaluated")) {
        *phase = Phase::CandidateEvaluated;
    } else if (value == QStringLiteral("download-started")) {
        *phase = Phase::DownloadStarted;
    } else if (value == QStringLiteral("download-verified")) {
        *phase = Phase::DownloadVerified;
    } else if (value == QStringLiteral("install-started")) {
        *phase = Phase::InstallStarted;
    } else if (value == QStringLiteral("installation-observed")) {
        *phase = Phase::InstallationObserved;
    } else {
        return false;
    }
    return true;
}

void appendLine(QByteArray *payload, const QByteArray &name, const QString &value)
{
    payload->append(name);
    payload->append('=');
    payload->append(value.toUtf8());
    payload->append('\n');
}

void appendLine(QByteArray *payload, const QByteArray &name, quint64 value)
{
    appendLine(payload, name, QString::number(value));
}

QString identityFor(const Record &record)
{
    QByteArray payload = QByteArrayLiteral("aegisy-update-progress-record/0.1\n");
    appendLine(&payload, QByteArrayLiteral("release_sequence"),
               record.releaseSequence);
    appendLine(&payload, QByteArrayLiteral("artifact_set_identity"),
               record.artifactSetIdentity);
    appendLine(&payload, QByteArrayLiteral("phase"), phaseName(record.phase));
    appendLine(&payload, QByteArrayLiteral("revision"), record.revision);
    appendLine(&payload, QByteArrayLiteral("updated_at_ms"), record.updatedAtMs);
    appendLine(&payload, QByteArrayLiteral("previous_record_identity"),
               record.previousRecordIdentity);
    appendLine(&payload, QByteArrayLiteral("download_authorized"),
               QStringLiteral("false"));
    appendLine(&payload, QByteArrayLiteral("install_authorized"),
               QStringLiteral("false"));
    appendLine(&payload, QByteArrayLiteral("rollback_authorized"),
               QStringLiteral("false"));
    return QStringLiteral("update-progress-record:sha256:%1")
        .arg(QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()));
}

QByteArray encodeRecord(const Record &record)
{
    return QJsonDocument(QJsonObject{
        {QStringLiteral("schema_version"), kSchema},
        {QStringLiteral("release_sequence"),
         static_cast<double>(record.releaseSequence)},
        {QStringLiteral("artifact_set_identity"), record.artifactSetIdentity},
        {QStringLiteral("phase"), phaseName(record.phase)},
        {QStringLiteral("revision"), static_cast<double>(record.revision)},
        {QStringLiteral("updated_at_ms"), static_cast<double>(record.updatedAtMs)},
        {QStringLiteral("previous_record_identity"),
         record.previousRecordIdentity},
        {QStringLiteral("record_identity"), record.recordIdentity},
        {QStringLiteral("download_authorized"), false},
        {QStringLiteral("install_authorized"), false},
        {QStringLiteral("rollback_authorized"), false},
    }).toJson(QJsonDocument::Compact);
}

Observation invalidObservation(const QString &errorCode, bool missing = false)
{
    Observation result;
    result.missing = missing;
    result.errorCode = errorCode;
    return result;
}

Observation readCurrent(const QString &root)
{
    if (root.isEmpty()) {
        return invalidObservation(QStringLiteral("update-record-root-invalid"));
    }
    const QString path = recordPath(root);
    const FileState state = inspectFile(path);
    if (state == FileState::Missing) {
        return invalidObservation(QStringLiteral("update-record-missing"), true);
    }
    if (state == FileState::LinkLike || state == FileState::MultipleLinks
        || state == FileState::Invalid) {
        return invalidObservation(QStringLiteral("update-record-path-invalid"));
    }
    if (state == FileState::PermissionsWide) {
        return invalidObservation(
            QStringLiteral("update-record-permissions-invalid"));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return invalidObservation(QStringLiteral("update-record-read-failed"));
    }
    const qint64 size = file.size();
    if (size <= 0 || size > kMaximumRecordBytes) {
        return invalidObservation(QStringLiteral("update-record-size-invalid"));
    }
    const QByteArray raw = file.read(kMaximumRecordBytes + 1);
    if (raw.size() != size || file.error() != QFile::NoError || !file.atEnd()) {
        return invalidObservation(QStringLiteral("update-record-read-failed"));
    }

    using namespace aegisy::aap::transport_runtime;
    TransportJsonValue parsed;
    QString parseError;
    if (!parseTransportJsonRaw(raw, &parsed, &parseError)) {
        return invalidObservation(QStringLiteral("update-record-json-invalid"));
    }
    QJsonValue projected;
    TransportProjectionError projectionError = TransportProjectionError::None;
    if (!projectJsonSafeTransportValue(parsed, &projected, &projectionError)
        || !projected.isObject()) {
        return invalidObservation(QStringLiteral("update-record-json-invalid"));
    }
    const QJsonObject object = projected.toObject();
    if (!hasExactKeys(object, {
            QStringLiteral("schema_version"),
            QStringLiteral("release_sequence"),
            QStringLiteral("artifact_set_identity"),
            QStringLiteral("phase"), QStringLiteral("revision"),
            QStringLiteral("updated_at_ms"),
            QStringLiteral("previous_record_identity"),
            QStringLiteral("record_identity"),
            QStringLiteral("download_authorized"),
            QStringLiteral("install_authorized"),
            QStringLiteral("rollback_authorized"),
        })) {
        return invalidObservation(QStringLiteral("update-record-fields-invalid"));
    }

    Record record;
    const QString schema = object.value(QStringLiteral("schema_version")).toString();
    const QJsonValue artifactIdentity = object.value(
        QStringLiteral("artifact_set_identity"));
    const QJsonValue phaseValue = object.value(QStringLiteral("phase"));
    const QJsonValue previousIdentity = object.value(
        QStringLiteral("previous_record_identity"));
    const QJsonValue recordIdentity = object.value(QStringLiteral("record_identity"));
    if (schema != kSchema
        || !positiveSafeInteger(object.value(QStringLiteral("release_sequence")),
                                &record.releaseSequence)
        || !artifactIdentity.isString()
        || !validArtifactSetIdentity(artifactIdentity.toString())
        || !phaseValue.isString()
        || !parsePhase(phaseValue.toString(), &record.phase)
        || !positiveSafeInteger(object.value(QStringLiteral("revision")),
                                &record.revision)
        || !positiveSafeInteger(object.value(QStringLiteral("updated_at_ms")),
                                &record.updatedAtMs)
        || !previousIdentity.isString() || !recordIdentity.isString()
        || (record.revision == 1 && !previousIdentity.toString().isEmpty())
        || (record.revision > 1
            && !validRecordIdentity(previousIdentity.toString()))
        || !validRecordIdentity(recordIdentity.toString())
        || object.value(QStringLiteral("download_authorized")) != QJsonValue(false)
        || object.value(QStringLiteral("install_authorized")) != QJsonValue(false)
        || object.value(QStringLiteral("rollback_authorized")) != QJsonValue(false)) {
        return invalidObservation(QStringLiteral("update-record-value-invalid"));
    }
    record.artifactSetIdentity = artifactIdentity.toString();
    record.previousRecordIdentity = previousIdentity.toString();
    record.recordIdentity = recordIdentity.toString();
    if (record.recordIdentity != identityFor(record)) {
        return invalidObservation(QStringLiteral("update-record-identity-invalid"));
    }

    Observation result;
    result.ok = true;
    result.record = record;
    return result;
}

AdvanceResult advanceError(const QString &errorCode)
{
    AdvanceResult result;
    result.errorCode = errorCode;
    return result;
}

AdvanceResult commitRecord(const QString &root, const Record &record)
{
    const QString path = recordPath(root);
    const FileState existing = inspectFile(path);
    if (existing != FileState::Missing
        && existing != FileState::RegularSingleLink) {
        return advanceError(QStringLiteral("update-record-path-invalid"));
    }
    const QByteArray encoded = encodeRecord(record);
    if (encoded.isEmpty() || encoded.size() > kMaximumRecordBytes) {
        return advanceError(QStringLiteral("update-record-size-invalid"));
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(encoded) != encoded.size() || !file.flush()) {
        file.cancelWriting();
        return advanceError(QStringLiteral("update-record-write-failed"));
    }
#ifndef Q_OS_WIN
    if (!file.setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return advanceError(QStringLiteral("update-record-write-failed"));
    }
#endif
    if (!file.commit()) {
        return advanceError(QStringLiteral("update-record-write-failed"));
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(
            path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return advanceError(QStringLiteral("update-record-write-failed"));
    }
#endif
    if (!syncCommittedRecord(path, root)) {
        return advanceError(QStringLiteral("update-record-sync-failed"));
    }
    const Observation verified = readCurrent(root);
    if (!verified.ok || verified.record.recordIdentity != record.recordIdentity) {
        return advanceError(QStringLiteral("update-record-post-commit-invalid"));
    }
    AdvanceResult result;
    result.ok = true;
    result.postCommitVerified = true;
    result.record = verified.record;
    return result;
}

} // namespace

QString phaseName(Phase phase)
{
    switch (phase) {
    case Phase::CandidateEvaluated:
        return QStringLiteral("candidate-evaluated");
    case Phase::DownloadStarted:
        return QStringLiteral("download-started");
    case Phase::DownloadVerified:
        return QStringLiteral("download-verified");
    case Phase::InstallStarted:
        return QStringLiteral("install-started");
    case Phase::InstallationObserved:
        return QStringLiteral("installation-observed");
    }
    return {};
}

Store::Store(const QString &stateRoot)
    : m_stateRoot(canonicalRoot(stateRoot))
{
}

Observation Store::load(quint64 minimumReleaseSequence,
                        quint64 minimumRevision,
                        const QString &expectedRecordIdentity) const
{
    Observation result = readCurrent(m_stateRoot);
    if (!result.ok) return result;
    if (minimumReleaseSequence > kMaximumSafeJsonInteger
        || minimumRevision > kMaximumSafeJsonInteger
        || result.record.releaseSequence < minimumReleaseSequence
        || result.record.revision < minimumRevision) {
        return invalidObservation(QStringLiteral("update-record-rollback"));
    }
    if (!expectedRecordIdentity.isEmpty()) {
        if (!validRecordIdentity(expectedRecordIdentity)
            || result.record.recordIdentity != expectedRecordIdentity) {
            return invalidObservation(
                QStringLiteral("update-record-continuity-mismatch"));
        }
        result.continuityVerified = true;
    }
    return result;
}

AdvanceResult Store::advance(quint64 releaseSequence,
                             const QString &artifactSetIdentity,
                             Phase phase,
                             qint64 nowMs,
                             const QString &expectedCurrentIdentity) const
{
    if (m_stateRoot.isEmpty()) {
        return advanceError(QStringLiteral("update-record-root-invalid"));
    }
    if (releaseSequence == 0 || releaseSequence > kMaximumSafeJsonInteger
        || !validArtifactSetIdentity(artifactSetIdentity)
        || phaseRank(phase) < 0 || nowMs <= 0
        || static_cast<quint64>(nowMs) > kMaximumSafeJsonInteger
        || (!expectedCurrentIdentity.isEmpty()
            && !validRecordIdentity(expectedCurrentIdentity))) {
        return advanceError(QStringLiteral("update-record-request-invalid"));
    }

    QLockFile lock(lockPath(m_stateRoot));
    if (!lock.tryLock(0)) {
        return advanceError(QStringLiteral("update-record-busy"));
    }

    const Observation current = readCurrent(m_stateRoot);
    if (!current.ok) {
        if (!current.missing) return advanceError(current.errorCode);
        if (!expectedCurrentIdentity.isEmpty()) {
            return advanceError(QStringLiteral("update-record-continuity-mismatch"));
        }
        if (phase != Phase::CandidateEvaluated) {
            return advanceError(QStringLiteral("update-record-transition-invalid"));
        }
        Record first;
        first.releaseSequence = releaseSequence;
        first.artifactSetIdentity = artifactSetIdentity;
        first.phase = phase;
        first.revision = 1;
        first.updatedAtMs = static_cast<quint64>(nowMs);
        first.recordIdentity = identityFor(first);
        return commitRecord(m_stateRoot, first);
    }

    const Record &previous = current.record;
    if (releaseSequence == previous.releaseSequence
        && artifactSetIdentity == previous.artifactSetIdentity
        && phase == previous.phase) {
        const bool expectationMatches = expectedCurrentIdentity
                == previous.recordIdentity
            || expectedCurrentIdentity == previous.previousRecordIdentity;
        if (!expectationMatches) {
            return advanceError(QStringLiteral("update-record-continuity-mismatch"));
        }
        AdvanceResult result;
        result.ok = true;
        result.idempotent = true;
        result.postCommitVerified = true;
        result.record = previous;
        return result;
    }
    if (releaseSequence < previous.releaseSequence) {
        return advanceError(QStringLiteral("update-record-rollback"));
    }
    if (releaseSequence == previous.releaseSequence
        && artifactSetIdentity != previous.artifactSetIdentity) {
        return advanceError(QStringLiteral("update-record-artifact-conflict"));
    }
    if (releaseSequence > previous.releaseSequence
        && artifactSetIdentity == previous.artifactSetIdentity) {
        return advanceError(QStringLiteral("update-record-artifact-conflict"));
    }
    if (expectedCurrentIdentity != previous.recordIdentity) {
        return advanceError(QStringLiteral("update-record-continuity-mismatch"));
    }
    if (static_cast<quint64>(nowMs) <= previous.updatedAtMs
        || previous.revision == kMaximumSafeJsonInteger) {
        return advanceError(QStringLiteral("update-record-clock-invalid"));
    }

    if (releaseSequence == previous.releaseSequence) {
        if (phaseRank(phase) != phaseRank(previous.phase) + 1) {
            return advanceError(QStringLiteral("update-record-transition-invalid"));
        }
    } else if (previous.phase != Phase::InstallationObserved
               || phase != Phase::CandidateEvaluated) {
        return advanceError(QStringLiteral("update-record-transition-invalid"));
    }

    Record next;
    next.releaseSequence = releaseSequence;
    next.artifactSetIdentity = artifactSetIdentity;
    next.phase = phase;
    next.revision = previous.revision + 1;
    next.updatedAtMs = static_cast<quint64>(nowMs);
    next.previousRecordIdentity = previous.recordIdentity;
    next.recordIdentity = identityFor(next);
    return commitRecord(m_stateRoot, next);
}

} // namespace UpdateProgressRecord
