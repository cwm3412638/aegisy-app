#include "secure_storage.h"
#include <QSettings>
#include <QStandardPaths>
#include <QDebug>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>

#ifdef AEGISY_SECURE_STORAGE_REMOVE_TESTING
#include <atomic>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

#ifdef Q_OS_MAC
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>

namespace {

CFStringRef createCfString(const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    return CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(utf8.constData()),
        utf8.size(),
        kCFStringEncodingUTF8,
        false);
}

CFMutableDictionaryRef createKeychainQuery(const QString &service, const QString &account)
{
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFStringRef serviceRef = createCfString(service);
    CFStringRef accountRef = createCfString(account);
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, serviceRef);
    CFDictionarySetValue(query, kSecAttrAccount, accountRef);
    CFRelease(serviceRef);
    CFRelease(accountRef);
    return query;
}

} // namespace
#endif

// 常量定义
static const QString SERVICE_NAME = "AegisyClient";
static const QString TOKEN_KEY = "auth_token";

namespace {

QMutex credentialCacheMutex;
QHash<QString, QString> credentialCache;

#ifdef AEGISY_SECURE_STORAGE_REMOVE_TESTING
std::atomic_bool failNextSecureStorageRemove{false};
#endif

void cacheCredential(const QString &key, const QString &value)
{
    QMutexLocker locker(&credentialCacheMutex);
    credentialCache.insert(key, value);
}

QString cachedCredential(const QString &key, bool *found)
{
    QMutexLocker locker(&credentialCacheMutex);
    const auto it = credentialCache.constFind(key);
    *found = it != credentialCache.cend();
    return *found ? it.value() : QString();
}

void removeCachedCredential(const QString &key)
{
    QMutexLocker locker(&credentialCacheMutex);
    credentialCache.remove(key);
}

} // namespace

bool SecureStorage::isAvailable()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MAC)
    return true;
#else
    return !QStandardPaths::findExecutable(QStringLiteral("secret-tool")).isEmpty();
#endif
}

bool SecureStorage::saveToken(const QString &token)
{
    return saveEncrypted(TOKEN_KEY, token);
}

QString SecureStorage::loadToken()
{
    return loadEncrypted(TOKEN_KEY);
}

bool SecureStorage::clearToken()
{
    return remove(TOKEN_KEY);
}

bool SecureStorage::saveEncrypted(const QString &key, const QString &data)
{
#ifdef Q_OS_WIN
    // Windows: 使用 DPAPI
    QByteArray encrypted = encryptWindows(data.toUtf8());
    if (encrypted.isEmpty()) {
        qWarning() << "DPAPI encryption failed";
        return false;
    }

    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Aegisy", "AegisyClient");
    settings.setValue(key, encrypted.toBase64());
    cacheCredential(key, data);
    return true;

#elif defined(Q_OS_MAC)
    // macOS: 使用 Keychain
    const bool saved = saveToKeychain(SERVICE_NAME, key, data);
    if (saved) {
        cacheCredential(key, data);
    }
    return saved;

#else
    // Linux 不再使用可逆的固定 XOR。没有 Secret Service 时拒绝持久化。
    const bool saved = saveToSecretService(SERVICE_NAME, key, data);
    if (saved) {
        cacheCredential(key, data);
    }
    return saved;
#endif
}

QString SecureStorage::loadEncrypted(const QString &key)
{
    bool foundInCache = false;
    const QString cached = cachedCredential(key, &foundInCache);
    if (foundInCache) {
        return cached;
    }

    QString value;
#ifdef Q_OS_WIN
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Aegisy", "AegisyClient");
    QString encryptedBase64 = settings.value(key).toString();
    if (encryptedBase64.isEmpty()) {
        return QString();
    }

    QByteArray encrypted = QByteArray::fromBase64(encryptedBase64.toUtf8());
    QByteArray decrypted = decryptWindows(encrypted);
    value = QString::fromUtf8(decrypted);

#elif defined(Q_OS_MAC)
    value = loadFromKeychain(SERVICE_NAME, key);

#else
    value = loadFromSecretService(SERVICE_NAME, key);
    if (value.isEmpty()) {
        // 清理旧版本固定 XOR 留下的不可安全使用数据，避免继续误认为已安全保存。
        QSettings settings(QSettings::NativeFormat, QSettings::UserScope,
                           "Aegisy", "AegisyClient");
        settings.remove(key);
    }
#endif

    if (!value.isEmpty()) {
        cacheCredential(key, value);
    }
    return value;
}

bool SecureStorage::contains(const QString &key)
{
    bool foundInCache = false;
    cachedCredential(key, &foundInCache);
    if (foundInCache) {
        return true;
    }

#ifdef Q_OS_WIN
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope,
                       "Aegisy", "AegisyClient");
    return settings.contains(key) && !settings.value(key).toString().isEmpty();
#elif defined(Q_OS_MAC)
    CFMutableDictionaryRef query = createKeychainQuery(SERVICE_NAME, key);
    CFDictionarySetValue(query, kSecReturnAttributes, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = NULL;
    const OSStatus status = SecItemCopyMatching(query, &result);
    if (result) {
        CFRelease(result);
    }
    CFRelease(query);
    return status == errSecSuccess;
#else
    return !loadFromSecretService(SERVICE_NAME, key).isEmpty();
#endif
}

bool SecureStorage::remove(const QString &key)
{
#ifdef AEGISY_SECURE_STORAGE_REMOVE_TESTING
    if (failNextSecureStorageRemove.exchange(false)) return false;
#endif
    removeCachedCredential(key);
#ifdef Q_OS_MAC
    return deleteFromKeychain(SERVICE_NAME, key);
#elif defined(Q_OS_WIN)
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Aegisy", "AegisyClient");
    settings.remove(key);
    return true;
#else
    return deleteFromSecretService(SERVICE_NAME, key);
#endif
}

#ifdef AEGISY_SECURE_STORAGE_REMOVE_TESTING
void SecureStorage::failNextRemoveForTesting()
{
    failNextSecureStorageRemove.store(true);
}
#endif

#ifdef Q_OS_WIN
QByteArray SecureStorage::encryptWindows(const QByteArray &data)
{
    DATA_BLOB inputBlob;
    DATA_BLOB outputBlob;

    inputBlob.pbData = (BYTE*)data.data();
    inputBlob.cbData = data.size();

    if (CryptProtectData(&inputBlob, L"AegisyClient", NULL, NULL, NULL, 0, &outputBlob)) {
        QByteArray result((char*)outputBlob.pbData, outputBlob.cbData);
        LocalFree(outputBlob.pbData);
        return result;
    }

    return QByteArray();
}

QByteArray SecureStorage::decryptWindows(const QByteArray &data)
{
    DATA_BLOB inputBlob;
    DATA_BLOB outputBlob;

    inputBlob.pbData = (BYTE*)data.data();
    inputBlob.cbData = data.size();

    if (CryptUnprotectData(&inputBlob, NULL, NULL, NULL, NULL, 0, &outputBlob)) {
        QByteArray result((char*)outputBlob.pbData, outputBlob.cbData);
        LocalFree(outputBlob.pbData);
        return result;
    }

    return QByteArray();
}
#endif

#ifdef Q_OS_MAC
bool SecureStorage::saveToKeychain(const QString &service, const QString &account, const QString &data)
{
    const QByteArray utf8 = data.toUtf8();
    CFDataRef dataRef = CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(utf8.constData()),
        utf8.size());
    CFMutableDictionaryRef query = createKeychainQuery(service, account);

    CFMutableDictionaryRef update = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(update, kSecValueData, dataRef);
    OSStatus status = SecItemUpdate(query, update);
    CFRelease(update);

    if (status == errSecItemNotFound) {
        CFDictionarySetValue(query, kSecValueData, dataRef);
        status = SecItemAdd(query, NULL);
    }

    CFRelease(dataRef);
    CFRelease(query);
    return status == errSecSuccess;
}

QString SecureStorage::loadFromKeychain(const QString &service, const QString &account)
{
    CFMutableDictionaryRef query = createKeychainQuery(service, account);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = NULL;
    const OSStatus status = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (status != errSecSuccess || !result) {
        return QString();
    }

    CFDataRef data = static_cast<CFDataRef>(result);
    const QString value = QString::fromUtf8(
        reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
        CFDataGetLength(data));
    CFRelease(result);
    return value;
}

bool SecureStorage::deleteFromKeychain(const QString &service, const QString &account)
{
    CFMutableDictionaryRef query = createKeychainQuery(service, account);
    const OSStatus status = SecItemDelete(query);
    CFRelease(query);
    return status == errSecSuccess || status == errSecItemNotFound;
}
#endif

#if !defined(Q_OS_WIN) && !defined(Q_OS_MAC)
bool SecureStorage::saveToSecretService(const QString &service, const QString &account,
                                        const QString &data)
{
    if (!isAvailable()) {
        return false;
    }

    QProcess process;
    process.start(QStringLiteral("secret-tool"), {
        QStringLiteral("store"),
        QStringLiteral("--label=Aegisy Client"),
        QStringLiteral("service"), service,
        QStringLiteral("account"), account,
    });
    if (!process.waitForStarted(3000)) {
        return false;
    }
    process.write(data.toUtf8());
    process.closeWriteChannel();
    return process.waitForFinished(10000)
        && process.exitStatus() == QProcess::NormalExit
        && process.exitCode() == 0;
}

QString SecureStorage::loadFromSecretService(const QString &service, const QString &account)
{
    if (!isAvailable()) {
        return QString();
    }

    QProcess process;
    process.start(QStringLiteral("secret-tool"), {
        QStringLiteral("lookup"),
        QStringLiteral("service"), service,
        QStringLiteral("account"), account,
    });
    if (!process.waitForFinished(5000)
            || process.exitStatus() != QProcess::NormalExit
            || process.exitCode() != 0) {
        return QString();
    }
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

bool SecureStorage::deleteFromSecretService(const QString &service, const QString &account)
{
    if (!isAvailable()) {
        return false;
    }

    QProcess process;
    process.start(QStringLiteral("secret-tool"), {
        QStringLiteral("clear"),
        QStringLiteral("service"), service,
        QStringLiteral("account"), account,
    });
    return process.waitForFinished(5000)
        && process.exitStatus() == QProcess::NormalExit
        && process.exitCode() == 0;
}
#else
bool SecureStorage::saveToSecretService(const QString &, const QString &, const QString &)
{
    return false;
}

QString SecureStorage::loadFromSecretService(const QString &, const QString &)
{
    return QString();
}

bool SecureStorage::deleteFromSecretService(const QString &, const QString &)
{
    return false;
}
#endif
