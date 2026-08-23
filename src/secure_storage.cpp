#include "secure_storage.h"
#include <QSettings>
#include <QStandardPaths>
#include <QDebug>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QVariant>

#include <limits>

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

bool validStorageKey(const QString &key)
{
    if (key.isEmpty() || key.toUtf8().size() > 1024) return false;
    for (const QChar character : key) {
        if (character.isNull() || character.category() == QChar::Other_Control
                || character.category() == QChar::Other_Surrogate) {
            return false;
        }
    }
    return true;
}

bool decodeUtf8Strict(const QByteArray &bytes, QString *value)
{
    if (!value) return false;
    const QString decoded = QString::fromUtf8(bytes.constData(), bytes.size());
    if (decoded.toUtf8() != bytes) return false;
    *value = decoded;
    return true;
}

SecureStorageReadResult readResult(SecureStorageReadState state,
                                   const QString &value = QString())
{
    SecureStorageReadResult result;
    result.state = state;
    result.value = state == SecureStorageReadState::Found ? value : QString();
    switch (state) {
    case SecureStorageReadState::Found:
        break;
    case SecureStorageReadState::Missing:
        result.errorCode = QStringLiteral("secure-storage-missing");
        break;
    case SecureStorageReadState::Unavailable:
        result.errorCode = QStringLiteral("secure-storage-unavailable");
        break;
    case SecureStorageReadState::Invalid:
        result.errorCode = QStringLiteral("secure-storage-invalid");
        break;
    }
    return result;
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
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        return false;
    }
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

SecureStorageReadResult SecureStorage::loadEncryptedFresh(const QString &key)
{
    if (!validStorageKey(key)) return readResult(SecureStorageReadState::Invalid);

#ifdef Q_OS_WIN
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Aegisy", "AegisyClient");
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        return readResult(SecureStorageReadState::Unavailable);
    }
    const bool storedKeyExists = settings.contains(key);
    if (settings.status() != QSettings::NoError) {
        return readResult(SecureStorageReadState::Unavailable);
    }
    if (!storedKeyExists) {
        return readResult(SecureStorageReadState::Missing);
    }

    const QVariant stored = settings.value(key);
    if (settings.status() != QSettings::NoError) {
        return readResult(SecureStorageReadState::Unavailable);
    }
    if (!stored.isValid() || stored.isNull()) {
        return readResult(SecureStorageReadState::Invalid);
    }
    const QByteArray encoded = stored.toByteArray();
    const QByteArray encrypted = QByteArray::fromBase64(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (encoded.isEmpty() || encrypted.isEmpty() || encrypted.toBase64() != encoded) {
        return readResult(SecureStorageReadState::Invalid);
    }

    QByteArray decrypted;
    if (!decryptWindows(encrypted, &decrypted)) {
        return readResult(SecureStorageReadState::Invalid);
    }
    QString value;
    const bool validUtf8 = decodeUtf8Strict(decrypted, &value);
    decrypted.fill('\0');
    return validUtf8 ? readResult(SecureStorageReadState::Found, value)
                     : readResult(SecureStorageReadState::Invalid);

#elif defined(Q_OS_MAC)
    CFMutableDictionaryRef query = createKeychainQuery(SERVICE_NAME, key);
    if (!query) return readResult(SecureStorageReadState::Unavailable);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef rawResult = NULL;
    const OSStatus status = SecItemCopyMatching(query, &rawResult);
    CFRelease(query);
    if (status == errSecItemNotFound) {
        if (rawResult) CFRelease(rawResult);
        return readResult(SecureStorageReadState::Missing);
    }
    if (status != errSecSuccess) {
        if (rawResult) CFRelease(rawResult);
        return readResult(SecureStorageReadState::Unavailable);
    }
    if (!rawResult || CFGetTypeID(rawResult) != CFDataGetTypeID()) {
        if (rawResult) CFRelease(rawResult);
        return readResult(SecureStorageReadState::Invalid);
    }

    const CFDataRef data = static_cast<CFDataRef>(rawResult);
    const CFIndex length = CFDataGetLength(data);
    if (length < 0 || static_cast<unsigned long long>(length)
            > static_cast<unsigned long long>(std::numeric_limits<int>::max())) {
        CFRelease(rawResult);
        return readResult(SecureStorageReadState::Invalid);
    }
    QByteArray bytes;
    if (length > 0) {
        const UInt8 *dataBytes = CFDataGetBytePtr(data);
        if (!dataBytes) {
            CFRelease(rawResult);
            return readResult(SecureStorageReadState::Invalid);
        }
        bytes = QByteArray(reinterpret_cast<const char *>(dataBytes),
                           static_cast<int>(length));
    }
    CFRelease(rawResult);
    QString value;
    const bool validUtf8 = decodeUtf8Strict(bytes, &value);
    bytes.fill('\0');
    return validUtf8 ? readResult(SecureStorageReadState::Found, value)
                     : readResult(SecureStorageReadState::Invalid);

#else
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("secret-tool"));
    if (executable.isEmpty()) {
        return readResult(SecureStorageReadState::Unavailable);
    }

    QProcess process;
    process.start(executable, {
        QStringLiteral("lookup"),
        QStringLiteral("service"), SERVICE_NAME,
        QStringLiteral("account"), key,
    });
    if (!process.waitForStarted(3000)) {
        return readResult(SecureStorageReadState::Unavailable);
    }
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        return readResult(SecureStorageReadState::Unavailable);
    }
    const QByteArray standardOutput = process.readAllStandardOutput();
    const QByteArray standardError = process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit) {
        return readResult(SecureStorageReadState::Unavailable);
    }
    if (process.exitCode() != 0) {
        if (process.exitCode() == 1 && standardOutput.isEmpty()
                && standardError.trimmed().isEmpty()) {
            return readResult(SecureStorageReadState::Missing);
        }
        return readResult(SecureStorageReadState::Unavailable);
    }

    QByteArray bytes = standardOutput;
    if (bytes.endsWith('\n')) bytes.chop(1);
    QString value;
    const bool validUtf8 = decodeUtf8Strict(bytes, &value);
    bytes.fill('\0');
    return validUtf8 ? readResult(SecureStorageReadState::Found, value)
                     : readResult(SecureStorageReadState::Invalid);
#endif
}

QString SecureStorage::loadEncrypted(const QString &key)
{
    bool foundInCache = false;
    const QString cached = cachedCredential(key, &foundInCache);
    if (foundInCache) return cached;

    const SecureStorageReadResult result = loadEncryptedFresh(key);
    if (result.state != SecureStorageReadState::Found) return QString();
    cacheCredential(key, result.value);
    return result.value;
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
    return loadEncryptedFresh(key).state == SecureStorageReadState::Found;
#endif
}

bool SecureStorage::remove(const QString &key)
{
#ifdef AEGISY_SECURE_STORAGE_REMOVE_TESTING
    if (failNextSecureStorageRemove.exchange(false)) return false;
#endif
    bool removed = false;
#ifdef Q_OS_MAC
    removed = deleteFromKeychain(SERVICE_NAME, key);
#elif defined(Q_OS_WIN)
    QSettings settings(QSettings::NativeFormat, QSettings::UserScope, "Aegisy", "AegisyClient");
    settings.remove(key);
    settings.sync();
    removed = settings.status() == QSettings::NoError;
#else
    removed = deleteFromSecretService(SERVICE_NAME, key);
#endif
    if (!removed) return false;
    removeCachedCredential(key);
    return true;
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

bool SecureStorage::decryptWindows(const QByteArray &data, QByteArray *decrypted)
{
    if (!decrypted) return false;
    decrypted->clear();
    if (static_cast<unsigned long long>(data.size())
            > static_cast<unsigned long long>(std::numeric_limits<DWORD>::max())) {
        return false;
    }
    DATA_BLOB inputBlob;
    DATA_BLOB outputBlob{};

    inputBlob.pbData = (BYTE*)data.data();
    inputBlob.cbData = data.size();

    if (CryptUnprotectData(&inputBlob, NULL, NULL, NULL, NULL, 0, &outputBlob)) {
        if ((outputBlob.cbData > 0 && !outputBlob.pbData)
                || outputBlob.cbData > static_cast<DWORD>(std::numeric_limits<int>::max())) {
            if (outputBlob.pbData) LocalFree(outputBlob.pbData);
            return false;
        }
        *decrypted = QByteArray(reinterpret_cast<const char *>(outputBlob.pbData),
                                static_cast<int>(outputBlob.cbData));
        if (outputBlob.pbData) LocalFree(outputBlob.pbData);
        return true;
    }

    return false;
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

bool SecureStorage::deleteFromSecretService(const QString &, const QString &)
{
    return false;
}
#endif
