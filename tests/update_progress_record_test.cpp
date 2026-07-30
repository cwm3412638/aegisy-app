#include "update_progress_record.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>

#include <cstdio>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

bool expect(bool condition, const char *message)
{
    if (!condition) std::fprintf(stderr, "%s\n", message);
    return condition;
}

QString artifactIdentity(char value)
{
    return QStringLiteral("update-artifact-set:sha256:")
        + QString(64, QChar::fromLatin1(value));
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

bool createHardLink(const QString &existingPath, const QString &linkPath)
{
#ifdef Q_OS_WIN
    const QString nativeExisting = QDir::toNativeSeparators(existingPath);
    const QString nativeLink = QDir::toNativeSeparators(linkPath);
    return CreateHardLinkW(
        reinterpret_cast<LPCWSTR>(nativeLink.utf16()),
        reinterpret_cast<LPCWSTR>(nativeExisting.utf16()), nullptr) != 0;
#else
    return ::link(QFile::encodeName(existingPath).constData(),
                  QFile::encodeName(linkPath).constData()) == 0;
#endif
}

} // namespace

int main()
{
    using namespace UpdateProgressRecord;

    QTemporaryDir directory;
    if (!expect(directory.isValid(), "update record directory is unavailable")) {
        return 1;
    }
    Store store(directory.path());
    const QString path = directory.path()
        + QStringLiteral("/aegisy-update-progress.json");
    const QString firstArtifact = artifactIdentity('a');
    const QString secondArtifact = artifactIdentity('b');

    Observation observation = store.load();
    bool ok = expect(!observation.ok && observation.missing
                         && observation.errorCode
                             == QStringLiteral("update-record-missing")
                         && !observation.downloadAuthorized
                         && !observation.installAuthorized
                         && !observation.rollbackAuthorized,
                     "missing update record did not fail closed");

    AdvanceResult result = store.advance(
        10, firstArtifact, Phase::DownloadStarted, 1000);
    ok = expect(!result.ok
                    && result.errorCode
                        == QStringLiteral("update-record-transition-invalid")
                    && !QFileInfo::exists(path),
                "noninitial phase created an update record") && ok;

    result = store.advance(10, firstArtifact, Phase::CandidateEvaluated, 1000);
    ok = expect(result.ok && !result.idempotent && result.postCommitVerified
                    && result.record.releaseSequence == 10
                    && result.record.revision == 1
                    && result.record.previousRecordIdentity.isEmpty()
                    && result.record.recordIdentity.startsWith(
                        QStringLiteral("update-progress-record:sha256:"))
                    && !result.downloadAuthorized && !result.installAuthorized
                    && !result.rollbackAuthorized,
                "first update record was not committed safely") && ok;
    const Record first = result.record;
    const QByteArray firstBytes = readBytes(path);
#ifndef Q_OS_WIN
    const QFileDevice::Permissions firstPermissions =
        QFileInfo(path).permissions();
    ok = expect(
             firstPermissions.testFlag(QFileDevice::ReadOwner)
                 && firstPermissions.testFlag(QFileDevice::WriteOwner)
                 && !(firstPermissions
                      & (QFileDevice::ExeOwner
                         | QFileDevice::ReadGroup
                         | QFileDevice::WriteGroup
                         | QFileDevice::ExeGroup
                         | QFileDevice::ReadOther
                         | QFileDevice::WriteOther
                         | QFileDevice::ExeOther)),
             "update record was not committed with private permissions")
        && ok;
#endif

    observation = store.load();
    ok = expect(observation.ok && !observation.continuityVerified
                    && observation.record.recordIdentity == first.recordIdentity
                    && !observation.downloadAuthorized
                    && !observation.installAuthorized,
                "unanchored record load was not authority-free") && ok;
    observation = store.load(10, 1, first.recordIdentity);
    ok = expect(observation.ok && observation.continuityVerified,
                "exact external record anchor was not recognized") && ok;

    result = store.advance(
        10, firstArtifact, Phase::CandidateEvaluated, 2000, QString());
    ok = expect(result.ok && result.idempotent
                    && result.record.recordIdentity == first.recordIdentity
                    && readBytes(path) == firstBytes,
                "exact first-record retry was not idempotent") && ok;

    result = store.advance(
        10, secondArtifact, Phase::CandidateEvaluated, 2000,
        first.recordIdentity);
    ok = expect(!result.ok
                    && result.errorCode
                        == QStringLiteral("update-record-artifact-conflict"),
                "same-sequence different artifact identity was accepted") && ok;

    result = store.advance(
        10, firstArtifact, Phase::DownloadVerified, 2000,
        first.recordIdentity);
    ok = expect(!result.ok
                    && result.errorCode
                        == QStringLiteral("update-record-transition-invalid"),
                "update phase skip was accepted") && ok;

    result = store.advance(
        10, firstArtifact, Phase::DownloadStarted, 2000,
        artifactIdentity('c'));
    ok = expect(!result.ok
                    && result.errorCode
                        == QStringLiteral("update-record-request-invalid"),
                "malformed record expectation was not rejected") && ok;

    result = store.advance(
        10, firstArtifact, Phase::DownloadStarted, 2000,
        first.recordIdentity);
    ok = expect(result.ok && result.record.revision == 2
                    && result.record.previousRecordIdentity == first.recordIdentity,
                "download-started phase was not committed") && ok;
    const Record downloadStarted = result.record;
    const QByteArray secondBytes = readBytes(path);

    result = store.advance(
        10, firstArtifact, Phase::DownloadStarted, 3000,
        first.recordIdentity);
    ok = expect(result.ok && result.idempotent
                    && result.record.recordIdentity
                        == downloadStarted.recordIdentity,
                "uncertain phase retry was not idempotent against its prior anchor") && ok;

    result = store.advance(
        11, secondArtifact, Phase::CandidateEvaluated, 3000,
        downloadStarted.recordIdentity);
    ok = expect(!result.ok
                    && result.errorCode
                        == QStringLiteral("update-record-transition-invalid"),
                "new release replaced an incomplete update") && ok;

    result = store.advance(
        10, firstArtifact, Phase::DownloadVerified, 3000,
        downloadStarted.recordIdentity);
    ok = expect(result.ok, "download-verified phase was rejected") && ok;
    result = store.advance(
        10, firstArtifact, Phase::InstallStarted, 4000,
        result.record.recordIdentity);
    ok = expect(result.ok, "install-started phase was rejected") && ok;
    result = store.advance(
        10, firstArtifact, Phase::InstallationObserved, 5000,
        result.record.recordIdentity);
    ok = expect(result.ok, "installation-observed phase was rejected") && ok;
    const Record installed = result.record;

    result = store.advance(
        11, secondArtifact, Phase::CandidateEvaluated, 6000,
        installed.recordIdentity);
    ok = expect(result.ok && result.record.releaseSequence == 11
                    && result.record.revision == installed.revision + 1,
                "next release did not follow a terminal update record") && ok;
    const Record nextRelease = result.record;

    result = store.advance(
        12, secondArtifact, Phase::CandidateEvaluated, 6500,
        nextRelease.recordIdentity);
    ok = expect(!result.ok
                    && result.errorCode
                        == QStringLiteral("update-record-artifact-conflict"),
                "one artifact identity was rebound to a different release") && ok;

    result = store.advance(
        10, firstArtifact, Phase::InstallationObserved, 7000,
        nextRelease.recordIdentity);
    ok = expect(!result.ok
                    && result.errorCode == QStringLiteral("update-record-rollback"),
                "release-sequence rollback was accepted") && ok;

    ok = expect(writeBytes(path, secondBytes),
                "rolled-back record fixture could not be written") && ok;
    observation = store.load(
        nextRelease.releaseSequence, nextRelease.revision,
        nextRelease.recordIdentity);
    ok = expect(!observation.ok
                    && observation.errorCode
                        == QStringLiteral("update-record-rollback")
                    && !observation.downloadAuthorized
                    && !observation.installAuthorized,
                "rolled-back record satisfied an external anchor") && ok;
    result = store.advance(
        11, secondArtifact, Phase::CandidateEvaluated, 8000,
        nextRelease.recordIdentity);
    ok = expect(!result.ok
                    && result.errorCode
                        == QStringLiteral("update-record-continuity-mismatch"),
                "rolled-back record resumed from a newer anchor") && ok;

    ok = expect(QFile::remove(path), "update record could not be deleted") && ok;
    observation = store.load(
        nextRelease.releaseSequence, nextRelease.revision,
        nextRelease.recordIdentity);
    ok = expect(!observation.ok && observation.missing
                    && !observation.downloadAuthorized
                    && !observation.installAuthorized,
                "deleted update record retained authority") && ok;
    result = store.advance(
        11, secondArtifact, Phase::CandidateEvaluated, 9000,
        nextRelease.recordIdentity);
    ok = expect(!result.ok
                    && result.errorCode
                        == QStringLiteral("update-record-continuity-mismatch")
                    && !QFileInfo::exists(path),
                "deleted update record was silently reconstructed") && ok;

    result = store.advance(12, firstArtifact, Phase::CandidateEvaluated, 10000);
    ok = expect(result.ok, "fresh authority-free record fixture was rejected") && ok;
    QByteArray corrupt = readBytes(path);
    corrupt.insert(1, QByteArrayLiteral("\"revision\":1,"));
    ok = expect(writeBytes(path, corrupt),
                "duplicate-key record fixture could not be written") && ok;
    observation = store.load();
    ok = expect(!observation.ok
                    && observation.errorCode
                        == QStringLiteral("update-record-json-invalid")
                    && !observation.downloadAuthorized
                    && !observation.installAuthorized,
                "duplicate-key update record was accepted") && ok;
    result = store.advance(12, firstArtifact, Phase::DownloadStarted, 11000);
    ok = expect(!result.ok
                    && result.errorCode
                        == QStringLiteral("update-record-json-invalid")
                    && readBytes(path) == corrupt,
                "corrupt update record was overwritten") && ok;

    QTemporaryDir authorityDirectory;
    ok = expect(authorityDirectory.isValid(),
                "authority-bit fixture directory is unavailable") && ok;
    Store authorityStore(authorityDirectory.path());
    result = authorityStore.advance(
        20, firstArtifact, Phase::CandidateEvaluated, 12000);
    ok = expect(result.ok, "authority-bit record fixture was rejected") && ok;
    const QString authorityPath = authorityDirectory.path()
        + QStringLiteral("/aegisy-update-progress.json");
    QByteArray authorityBytes = readBytes(authorityPath);
    const qsizetype falseOffset = authorityBytes.indexOf(
        QByteArrayLiteral("\"download_authorized\":false"));
    ok = expect(falseOffset >= 0,
                "authority-bit record field was not found") && ok;
    if (falseOffset >= 0) {
        authorityBytes.replace(
            falseOffset,
            QByteArrayLiteral("\"download_authorized\":false").size(),
            QByteArrayLiteral("\"download_authorized\":true"));
        ok = expect(writeBytes(authorityPath, authorityBytes),
                    "authority-bit record fixture could not be written") && ok;
        observation = authorityStore.load();
        ok = expect(!observation.ok
                        && observation.errorCode
                            == QStringLiteral("update-record-value-invalid")
                        && !observation.downloadAuthorized
                        && !observation.installAuthorized,
                    "forged update authority bit was accepted") && ok;
    }

    QTemporaryDir linkDirectory;
    ok = expect(linkDirectory.isValid(),
                "record link fixture directory is unavailable") && ok;
    const QString linkPath = linkDirectory.path()
        + QStringLiteral("/aegisy-update-progress.json");
    const QString linkTarget = linkDirectory.path()
        + QStringLiteral("/record-target.json");
    ok = expect(writeBytes(linkTarget, firstBytes),
                "record hard-link target could not be written") && ok;
    ok = expect(createHardLink(linkTarget, linkPath),
                "record hard link could not be created") && ok;
    Store linkStore(linkDirectory.path());
    observation = linkStore.load();
    ok = expect(!observation.ok
                    && observation.errorCode
                        == QStringLiteral("update-record-path-invalid")
                    && !observation.downloadAuthorized
                    && !observation.installAuthorized,
                "multiply linked update record was accepted") && ok;

#ifndef Q_OS_WIN
    ok = expect(QFile::remove(linkPath),
                "record hard link could not be removed") && ok;
    ok = expect(::symlink(QFile::encodeName(linkTarget).constData(),
                          QFile::encodeName(linkPath).constData()) == 0,
                "record symbolic link could not be created") && ok;
    observation = linkStore.load();
    ok = expect(!observation.ok
                    && observation.errorCode
                        == QStringLiteral("update-record-path-invalid")
                    && !observation.downloadAuthorized
                    && !observation.installAuthorized,
                "symbolic-link update record was accepted") && ok;
#endif

#ifndef Q_OS_WIN
    QTemporaryDir permissionsDirectory;
    ok = expect(permissionsDirectory.isValid(),
                "record permissions fixture directory is unavailable") && ok;
    Store permissionsStore(permissionsDirectory.path());
    result = permissionsStore.advance(
        25, firstArtifact, Phase::CandidateEvaluated, 12500);
    ok = expect(result.ok,
                "record permissions fixture could not be created") && ok;
    const QString permissionsPath = permissionsDirectory.path()
        + QStringLiteral("/aegisy-update-progress.json");
    ok = expect(QFile::setPermissions(
                    permissionsPath,
                    QFileDevice::ReadOwner | QFileDevice::WriteOwner
                        | QFileDevice::ReadGroup),
                "record permissions could not be widened") && ok;
    observation = permissionsStore.load();
    ok = expect(!observation.ok
                    && observation.errorCode
                        == QStringLiteral("update-record-permissions-invalid")
                    && !observation.downloadAuthorized
                    && !observation.installAuthorized,
                "over-permissive update record was accepted") && ok;

    ok = expect(::chmod(QFile::encodeName(permissionsPath).constData(),
                        S_IRUSR | S_IWUSR | S_ISUID) == 0,
                "record special permission bit could not be set") && ok;
    struct stat specialPermissions {};
    ok = expect(::lstat(QFile::encodeName(permissionsPath).constData(),
                        &specialPermissions) == 0
                    && (specialPermissions.st_mode & S_ISUID) != 0,
                "record special permission bit was not retained") && ok;
    observation = permissionsStore.load();
    ok = expect(!observation.ok
                    && observation.errorCode
                        == QStringLiteral("update-record-permissions-invalid")
                    && !observation.downloadAuthorized
                    && !observation.installAuthorized,
                "special Unix permission bit was accepted") && ok;
#endif

    QTemporaryDir lockDirectory;
    ok = expect(lockDirectory.isValid(),
                "record lock fixture directory is unavailable") && ok;
    QLockFile heldLock(lockDirectory.path()
                       + QStringLiteral("/aegisy-update-progress.lock"));
    ok = expect(heldLock.tryLock(0),
                "record lock fixture could not acquire the writer lock") && ok;
    Store lockedStore(lockDirectory.path());
    result = lockedStore.advance(
        30, firstArtifact, Phase::CandidateEvaluated, 13000);
    ok = expect(!result.ok
                    && result.errorCode == QStringLiteral("update-record-busy")
                    && !QFileInfo::exists(
                        lockDirectory.path()
                        + QStringLiteral("/aegisy-update-progress.json")),
                "concurrent update-record writer bypassed the lock") && ok;
    heldLock.unlock();
    result = lockedStore.advance(
        30, firstArtifact, Phase::CandidateEvaluated, 13000);
    ok = expect(result.ok && result.postCommitVerified,
                "update record did not recover after writer lock release") && ok;

    return ok ? 0 : 1;
}
