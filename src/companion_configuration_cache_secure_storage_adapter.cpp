#include "companion_configuration_cache_secure_storage_adapter.h"

#include "secure_storage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

const char kAuthorityScopePrefix[] =
    "companion/configuration-cache-authority/v1/";
constexpr int kMaximumAuthorityBytes = 16 * 1024;

void fail(QString *errorCode, const QString &code)
{
    if (errorCode) *errorCode = code;
}

bool validAuthorityScope(const QString &scope)
{
    const QString prefix = QString::fromLatin1(kAuthorityScopePrefix);
    if (!scope.startsWith(prefix) || scope.size() != prefix.size() + 64) {
        return false;
    }
    for (const QChar character : scope.mid(prefix.size())) {
        if (!character.isDigit()
                && !(character >= QLatin1Char('a')
                     && character <= QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

bool strictUtf8(const QString &value, QByteArray *bytes)
{
    if (!bytes) return false;
    *bytes = value.toUtf8();
    return !bytes->isEmpty() && bytes->size() <= kMaximumAuthorityBytes
        && !bytes->contains('\0')
        && QString::fromUtf8(bytes->constData(), bytes->size()) == value;
}

bool strictUtf8(const QByteArray &bytes, QString *value)
{
    if (!value || bytes.isEmpty() || bytes.size() > kMaximumAuthorityBytes
            || bytes.contains('\0')) {
        return false;
    }
    const QString decoded = QString::fromUtf8(bytes.constData(), bytes.size());
    if (decoded.toUtf8() != bytes) return false;
    *value = decoded;
    return true;
}

} // namespace

CompanionConfigurationCacheSecureStore::ReadState
SecureStorageCompanionConfigurationCacheAdapter::readFresh(
    const QString &scope, QByteArray *value, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    if (!value || !validAuthorityScope(scope)) {
        fail(errorCode, QStringLiteral("companion-cache-secure-scope-invalid"));
        return ReadState::Invalid;
    }
    value->clear();
    const SecureStorageReadResult result = SecureStorage::loadEncryptedFresh(scope);
    switch (result.state) {
    case SecureStorageReadState::Missing:
        fail(errorCode, result.errorCode);
        return ReadState::Missing;
    case SecureStorageReadState::Unavailable:
        fail(errorCode, result.errorCode);
        return ReadState::Unavailable;
    case SecureStorageReadState::Invalid:
        fail(errorCode, result.errorCode);
        return ReadState::Invalid;
    case SecureStorageReadState::Found:
        if (!strictUtf8(result.value, value)) {
            fail(errorCode, QStringLiteral("companion-cache-secure-value-invalid"));
            return ReadState::Invalid;
        }
        return ReadState::Found;
    }
    fail(errorCode, QStringLiteral("companion-cache-secure-state-invalid"));
    return ReadState::Invalid;
}

CompanionConfigurationCacheSecureStore::WriteOutcome
SecureStorageCompanionConfigurationCacheAdapter::write(
    const QString &scope, const QByteArray &value, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    QString decoded;
    if (!validAuthorityScope(scope) || !strictUtf8(value, &decoded)) {
        fail(errorCode, QStringLiteral("companion-cache-secure-write-invalid"));
        return WriteOutcome::DefiniteFailure;
    }
    if (!SecureStorage::saveEncrypted(scope, decoded)) {
        fail(errorCode, QStringLiteral("companion-cache-secure-write-outcome-unknown"));
        return WriteOutcome::OutcomeUnknown;
    }
    return WriteOutcome::Committed;
}

QString SecureStorageCompanionConfigurationCacheAdapter::prepareLockFilePath(
    const QString &appDataLocation, QString *errorCode)
{
    if (errorCode) errorCode->clear();
    const QFileInfo appDataInfo(appDataLocation);
    if (appDataLocation.isEmpty() || !appDataInfo.isAbsolute()) {
        fail(errorCode, QStringLiteral("companion-cache-app-data-invalid"));
        return {};
    }
    if (!QDir().mkpath(appDataInfo.absoluteFilePath())) {
        fail(errorCode, QStringLiteral("companion-cache-app-data-create-failed"));
        return {};
    }
    const QString directoryPath = QDir(appDataInfo.absoluteFilePath()).filePath(
        QStringLiteral("companion-configuration-cache-v2"));
    QFileInfo directoryInfo(directoryPath);
    if ((directoryInfo.exists()
         && (!directoryInfo.isDir() || directoryInfo.isSymLink()))
            || (!directoryInfo.exists() && !QDir().mkpath(directoryPath))) {
        fail(errorCode, QStringLiteral("companion-cache-lock-directory-invalid"));
        return {};
    }
    directoryInfo.setFile(directoryPath);
    if (!directoryInfo.exists() || !directoryInfo.isDir()
            || directoryInfo.isSymLink() || !directoryInfo.isAbsolute()) {
        fail(errorCode, QStringLiteral("companion-cache-lock-directory-invalid"));
        return {};
    }
#ifdef Q_OS_UNIX
    const QFileDevice::Permissions privatePermissions =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    if (!QFile::setPermissions(directoryPath, privatePermissions)
            || (QFileInfo(directoryPath).permissions()
                & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                   | QFileDevice::ExeGroup | QFileDevice::ReadOther
                   | QFileDevice::WriteOther | QFileDevice::ExeOther))) {
        fail(errorCode, QStringLiteral("companion-cache-lock-directory-permissions-invalid"));
        return {};
    }
#endif
    const QString lockPath = QDir(directoryPath).absoluteFilePath(
        QStringLiteral("cache-authority.lock"));
    const QFileInfo lockInfo(lockPath);
    if (!lockInfo.isAbsolute() || (lockInfo.exists() && lockInfo.isSymLink())) {
        fail(errorCode, QStringLiteral("companion-cache-lock-path-invalid"));
        return {};
    }
    return lockPath;
}
